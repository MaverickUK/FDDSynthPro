// A demo of the drive, front panel and sound together: a virtual PC
// (test/native/virtual_pc.h) uses a real FAT12 disk while scripted turns and
// presses work the front panel. Everything is the firmware's own code: the
// drive, loader and write path, the sound events and mixer, the panel's
// model and screens. scripts/make_showreel.py turns the output into a video.
//
//   showreel <disk.img> <pack folder> <out folder>
//
// Writes to the out folder:
//   frames.raw  the OLED every 100 ms of simulated time (the panel's frame
//               rate): 128 x 64 bytes per frame, 0 or 1 per pixel
//   audio.wav   the mixer's output, the volume following the encoder
//   sounds.txt  each sound event the mixer was given: "<ms from the start> <NAME>"
//   script.txt  what happened when, for the captions: "<frame> CAPTION|LOG|INPUT <text>"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "canvas.h"
#include "dos.h"
#include "image_list.h"
#include "machine.h"
#include "screens.h"
#include "synth.h"
#include "track_source.h"
#include "ui.h"
#include "virtual_pc.h"
#include "wav.h"

namespace {

constexpr uint32_t FRAME_US = 100000;       // cfg::OLED_FRAME_MS
constexpr uint32_t TRACE_US = 50000;        // cfg::HEAD_TRACE_SAMPLE_MS
constexpr uint32_t SWAP_EJECTED_US = 500000;  // main.cpp's SWAP_EJECTED_MS

std::vector<uint8_t> load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

// The card's sample pack, as sd_image::loadPack reads it.
struct PackFiles {
    std::vector<int16_t> samples[8];
    synth::Pack pack;
    std::string name;  // the folder's, as the firmware shows it
    bool load(const std::string& dir) {
        name = dir.substr(dir.find_last_of('/') + 1);
        const char* names[] = {"spinup", "spin", "spindown", "step", "seek", "bump", "insert", "eject"};
        synth::Sample* slots[] = {&pack.spinup, &pack.spin, &pack.spindown, &pack.step,
                                  &pack.seek,   &pack.bump, &pack.insert,   &pack.eject};
        for (int i = 0; i < 8; ++i) {
            std::vector<uint8_t> f = ::load(dir + "/" + names[i] + ".wav");
            WavInfo info;
            if (f.empty() || parseWavHeader(f.data(), f.size(), uint32_t(f.size()), info) != WavError::Ok ||
                !wavIsPlayable(info))
                continue;
            samples[i].resize(info.dataBytes / 2);
            std::memcpy(samples[i].data(), &f[info.dataOffset], info.dataBytes & ~1u);
            *slots[i] = synth::Sample{samples[i].data(), uint32_t(samples[i].size())};
            pack.rate = info.sampleRate;
        }
        return pack.complete();
    }
};

void writeWav(const std::string& path, const std::vector<int16_t>& s, uint32_t rate) {
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    uint32_t bytes = uint32_t(s.size() * 2);
    f.write("RIFF", 4), u32(36 + bytes), f.write("WAVEfmt ", 8);
    u32(16), u16(1), u16(1), u32(rate), u32(rate * 2), u16(2), u16(16);
    f.write("data", 4), u32(bytes);
    f.write(reinterpret_cast<const char*>(s.data()), std::streamsize(bytes));
}

// The front panel as the firmware runs it (panel.cpp, main.cpp's
// servicePanel), watching the virtual machine instead of the board.
class Panel {
public:
    Panel(Machine& m, MemorySource& disk, const PackFiles& pack, const std::string& out)
        : m_(m), disk_(disk), pack_(pack), frames_(out + "/frames.raw", std::ios::binary),
          script_(out + "/script.txt") {
        // The card: four disks (each one is the real disk, really).
        images_.add("DOS622.IMG", 1474560);
        images_.add("GAMES.IMG", 1474560);
        images_.add("MONKEY1.IMG", 1474560);
        images_.add("UTILS720.IMA", 737280);
        m.on_tick = [this](uint32_t now) { tick(now); };
    }

    void start(uint32_t now) {
        t0_ = now;
        next_frame_ = next_trace_ = now;
        started_ = true;
    }

    // Scripted controls: an input at `at`, and a few in a row.
    void input(uint32_t at, ui::Input in) { inputs_.push_back({at, in}); }
    void turn(uint32_t at, int detents, uint32_t gap_us = 110000) {
        for (int i = 0; i < (detents < 0 ? -detents : detents); ++i)
            input(at + uint32_t(i) * gap_us, detents < 0 ? ui::Input::TurnDown : ui::Input::TurnUp);
    }

    void caption(const char* text) { line("CAPTION", text); }
    void log(const std::string& text) { line("LOG", text.c_str()); }

    bool diskIn() const { return m_.d.diskPresent() && image_ == 1; }
    const std::vector<std::pair<uint32_t, int>>& volumes() const { return volumes_; }
    uint32_t t0() const { return t0_; }
    uint32_t frames() const { return frame_count_; }

private:
    struct Scheduled {
        uint32_t at;
        ui::Input in;
    };

    void line(const char* kind, const char* text) { script_ << frame_count_ << " " << kind << " " << text << "\n"; }

    ui::Status status(uint32_t now) const {
        ui::Status s;
        s.images = &images_;
        s.image = image_;
        s.image_name = images_.name(image_);
        s.kb = 1440;
        s.disk_in = m_.d.diskPresent();
        s.write_protected = m_.d.writeProtected();
        s.swapping = swap_at_ != 0;
        s.motor = m_.tracker.motorOn();
        s.cylinder = m_.d.head().cylinder();
        s.head = m_.d.side();
        s.activity = m_.gate.capturing()                    ? ui::Activity::Write
                     : m_.tracker.seeking()                 ? ui::Activity::Seek
                     : m_.d.streams(now, m_.loader.ready()) ? ui::Activity::Read
                                                            : ui::Activity::Idle;
        s.rotation = s.motor ? uint8_t((now % 200000) * 256 / 200000) : 0;
        s.pack = pack_.name.c_str();
        s.sounds_loaded = pack_.pack.loadedMask();
        s.rate = pack_.pack.rate;
        s.pack_kb = pack_.pack.bytes() / 1024;
        s.loads = m_.loader.loads();
        for (const BurstResult& w : m_.writes) s.written += w.persisted, s.bad_crc += w.decoded.bad_crc;
        return s;
    }

    void tick(uint32_t now) {
        if (!started_) return;
        uint32_t ms = (now - t0_) / 1000;
        ui::Status s = status(now);
        for (Scheduled& i : inputs_) {
            if (i.at && int32_t(now - i.at) >= 0) {
                static const char* const names[] = {"TurnUp", "TurnDown", "Press", "Action", "LongAction"};
                line("INPUT", names[int(i.in)]);
                model_.input(i.in, s, ms);
                i.at = 0;  // done
            }
        }
        model_.tick(s, ms);

        // The model's requests, as servicePanel acts on them.
        size_t to;
        if (!swap_at_ && model_.takeSwap(to)) {
            if (m_.d.diskPresent()) m_.eject();
            swap_to_ = to;
            swap_at_ = now;
            log(std::string("Panel: insert ") + images_.name(to));
        }
        if (swap_at_ && now - swap_at_ >= SWAP_EJECTED_US) {
            image_ = swap_to_;
            m_.insert(disk_);  // (every name on the card is this one disk)
            swap_at_ = 0;
        }
        if (model_.takeEjectToggle()) {
            if (m_.d.diskPresent()) {
                m_.eject();
                log("Panel: eject");
            } else {
                m_.insert(disk_);
                log("Panel: insert");
            }
        }
        if (volumes_.empty() || volumes_.back().second != model_.volume()) volumes_.push_back({now, model_.volume()});

        while (int32_t(now - next_trace_) >= 0) {
            trace_.add(uint8_t(m_.d.head().cylinder()));
            next_trace_ += TRACE_US;
        }
        while (int32_t(now - next_frame_) >= 0) {
            screens::draw(canvas_, model_, status(now), trace_);
            char px[Canvas::WIDTH * Canvas::HEIGHT];
            for (int y = 0; y < Canvas::HEIGHT; ++y)
                for (int x = 0; x < Canvas::WIDTH; ++x) px[y * Canvas::WIDTH + x] = char(canvas_.get(x, y) ? 1 : 0);
            frames_.write(px, sizeof px);
            ++frame_count_;
            next_frame_ += FRAME_US;
        }
    }

    Machine& m_;
    MemorySource& disk_;
    const PackFiles& pack_;
    std::ofstream frames_;
    std::ofstream script_;
    ImageList images_;
    size_t image_ = 0;  // the list starts at the top
    ui::Model model_;
    ui::HeadTrace trace_;
    Canvas canvas_;
    std::vector<Scheduled> inputs_;
    std::vector<std::pair<uint32_t, int>> volumes_;
    uint32_t t0_ = 0, next_frame_ = 0, next_trace_ = 0, swap_at_ = 0;
    size_t swap_to_ = 0;
    uint32_t frame_count_ = 0;
    bool started_ = false;
};

// The mixer over the whole session: each sound event and volume change at
// its time, as core 0 hands them over.
std::vector<int16_t> mix(const PackFiles& pack, const std::vector<activity::Event>& sounds,
                         const std::vector<std::pair<uint32_t, int>>& volumes, uint32_t t0, uint32_t frames) {
    const uint32_t rate = pack.pack.rate;
    std::vector<int16_t> out(size_t(uint64_t(frames) * FRAME_US * rate / 1000000));
    synth::Mixer mixer(pack.pack);
    auto at = [&](uint32_t t) { return size_t(uint64_t(t - t0) * rate / 1000000); };
    auto renderTo = [&](size_t n) {
        n = n > out.size() ? out.size() : n;
        if (n > mixer.position()) mixer.render(&out[mixer.position()], n - mixer.position());
    };
    size_t v = 0;
    for (const activity::Event& e : sounds) {
        if (int32_t(e.time_us - t0) < 0) continue;  // before the reel starts
        while (v < volumes.size() && int32_t(volumes[v].first - e.time_us) <= 0) {
            renderTo(at(volumes[v].first));
            mixer.setVolume(volumes[v++].second);
        }
        renderTo(at(e.time_us));
        mixer.event(e);
    }
    for (; v < volumes.size(); ++v) {
        renderTo(at(volumes[v].first));
        mixer.setVolume(volumes[v].second);
    }
    renderTo(out.size());
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: showreel <disk.img> <pack folder> <out folder>\n");
        return 2;
    }
    std::vector<uint8_t> image = load(argv[1]);
    if (image.size() != 1474560) {
        std::fprintf(stderr, "%s: expected a 1.44 MB image\n", argv[1]);
        return 1;
    }
    PackFiles pack;
    if (!pack.load(argv[2])) {
        std::fprintf(stderr, "%s: not a complete sample pack\n", argv[2]);
        return 1;
    }
    const std::string out = argv[3];

    Machine m(ibm::HD_1440, Core0Costs{}, 0);
    MemorySource disk(ibm::HD_1440, std::move(image));
    VirtualPc pc(m);
    Panel panel(m, disk, pack, out);
    // As at power-on: the card's first image is in (the generated disk
    // stands in for DOS622.IMG). The reel starts once that has settled.
    pc.run(200000);
    panel.start(pc.now);
    const uint32_t S = 1000000;  // a second, in simulated microseconds

    panel.caption("Power on: the card's first disk is in");
    panel.log("FDDSynthPro 0.7.0 drive, sample pack " + pack.name);
    panel.turn(pc.now + S, 5);  // 75% (the power-on volume) up to 100%
    pc.run(2 * S);

    panel.caption("Choose another disk: press, turn, press");
    panel.input(pc.now + S / 4, ui::Input::Press);
    panel.turn(pc.now + S, -2, 350000);  // down past GAMES to MONKEY1, then back up to GAMES
    panel.turn(pc.now + 2 * S, 1);
    panel.input(pc.now + 2 * S + 700000, ui::Input::Press);
    pc.run(4 * S);  // the swap: out for half a second, then GAMES goes in
    if (!panel.diskIn()) {
        std::fprintf(stderr, "the swap didn't happen\n");
        return 1;
    }

    panel.caption("The PC sees the disk change and spins it up");
    panel.log("BIOS: motor on");
    pc.motorOn();
    panel.log("BIOS: recalibrate (step out to track 0)");
    pc.recalibrate();
    panel.log(std::string("BIOS: disk changed = ") + (pc.diskChanged() ? "yes" : "no"));
    pc.clearDiskChange();
    if (!pc.detectRate()) {
        std::fprintf(stderr, "the PC couldn't read the disk\n");
        return 1;
    }
    panel.log("BIOS: 500 kbit/s, 1.44 MB media");

    panel.caption("DOS reads the boot sector, FAT and directory");
    Dos dos(pc);
    if (!dos.mount()) {
        std::fprintf(stderr, "not a FAT12 disk\n");
        return 1;
    }
    panel.log("A:\\> DIR");
    for (const Dos::Entry& e : dos.dir()) {
        char l[64];
        std::snprintf(l, sizeof l, "  %-12s %7u", e.name.c_str(), unsigned(e.size));
        panel.log(l);
    }
    pc.run(S);

    auto readFile = [&](const char* name) {
        for (const Dos::Entry& e : dos.dir())
            if (e.name == name) {
                std::vector<uint8_t> data;
                panel.log(std::string("A:\\> COPY ") + name + " NUL");
                if (!dos.readFile(e, data)) {
                    std::fprintf(stderr, "couldn't read %s\n", name);
                    std::exit(1);
                }
                char l[64];
                std::snprintf(l, sizeof l, "  %u bytes read", unsigned(data.size()));
                panel.log(l);
            }
    };

    panel.caption("Reading a game, and turning the volume down");
    panel.turn(pc.now + S, -8);  // 100% down to 60%
    readFile("GAME.EXE");
    pc.run(S / 2);

    panel.caption("The head trace: a file near the middle of the disk");
    panel.input(pc.now + S / 4, ui::Input::Action);
    pc.run(S);
    readFile("LEVEL9.DAT");
    pc.run(S / 2);
    readFile("README.TXT");
    pc.run(S / 2);
    readFile("LEVEL9.DAT");
    pc.run(S);

    panel.caption("The sample pack, and the drive's stats");
    panel.input(pc.now, ui::Input::Action);
    pc.run(2 * S);
    panel.input(pc.now, ui::Input::Action);
    pc.run(2 * S);
    panel.input(pc.now, ui::Input::Action);  // home
    pc.run(S / 2);

    panel.caption("Saving a game, with the volume back up");
    panel.turn(pc.now + S / 2, 8, 90000);  // back up to 100%
    std::vector<uint8_t> save(24 * 1024);
    for (size_t i = 0; i < save.size(); ++i) save[i] = uint8_t(i * 31 + 7);
    panel.log("A:\\> COPY CON SAVE1.DAT");
    if (!dos.writeFile("SAVE1.DAT", save)) {
        std::fprintf(stderr, "couldn't write SAVE1.DAT\n");
        return 1;
    }
    panel.log("  24576 bytes written: data, both FATs, the directory");
    pc.run(S / 2);
    readFile("SAVE1.DAT");
    pc.run(S);

    panel.caption("Motor off");
    panel.log("BIOS: motor off");
    pc.motorOff();
    pc.run(3 * S);

    panel.caption("Hold the action button to eject");
    panel.input(pc.now + S / 2, ui::Input::LongAction);
    pc.run(3 * S);

    std::vector<int16_t> audio = mix(pack, m.sounds, panel.volumes(), panel.t0(), panel.frames());
    writeWav(out + "/audio.wav", audio, pack.pack.rate);
    std::ofstream sounds(out + "/sounds.txt");
    for (const activity::Event& e : m.sounds)
        if (int32_t(e.time_us - panel.t0()) >= 0)
            sounds << (e.time_us - panel.t0()) / 1000 << " " << activity::soundName(e.sound) << "\n";
    std::printf("SHOWREEL frames=%u seconds=%.1f retries=%u sectors_read=%u written=%u\n", panel.frames(),
                panel.frames() / 10.0, pc.retries, dos.sectorsRead, dos.sectorsWritten);
    return pc.retries == 0 ? 0 : 1;
}
