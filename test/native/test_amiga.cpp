// Tests for the Amiga: AmigaDOS tracks (src/amiga_track.*), the drive as an
// Amiga sees it (latched motor, /RDY and the drive ID, /CHNG on pin 2), and
// a virtual Amiga reading and writing whole tracks through the whole drive,
// as trackdisk.device does: from wherever the head is, with no index.
// Greaseweazle checks the tracks themselves (test_track_encoder.py).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <random>
#include <vector>

#include "amiga_pio.h"
#include "amiga_track.h"
#include "bus.h"
#include "check.h"
#include "drive.h"
#include "image_list.h"
#include "machine.h"
#include "mfm.h"
#include "settings.h"
#include "virtual_hosts.h"

namespace {  // this file's own helpers and tests

// A pin sample with these lines asserted (low), the rest released.
uint32_t sample(std::initializer_list<unsigned> asserted_pins) {
    uint32_t s = fdd::INPUT_MASK;
    for (unsigned p : asserted_pins) s &= ~fdd::maskOf(p);
    return s;
}

std::vector<uint8_t> randomTrack(uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<uint8_t> data(amiga::SECTORS * amiga::SECTOR_BYTES);
    for (uint8_t& b : data) b = uint8_t(rng());
    return data;
}

// Every cell of `words`, from word `start`, round the track `count` cells.
template <typename Each>
void cellsFrom(const std::vector<uint16_t>& words, size_t start_cell, size_t count, Each each) {
    size_t total = words.size() * 16;
    for (size_t i = 0; i < count; ++i) {
        size_t c = (start_cell + i) % total;
        each(bool((words[c / 16] >> (15 - c % 16)) & 1));
    }
}

TEST(test_amiga_checksum) {
    // Greaseweazle's: the XOR of the longs, folded onto the data bits.
    const uint8_t data[8] = {0x12, 0x34, 0x56, 0x78, 0xFF, 0x00, 0xFF, 0x00};
    uint32_t x = 0x12345678u ^ 0xFF00FF00u;
    CHECK_EQ(amiga::checksum(data, 8), (x ^ (x >> 1)) & 0x55555555u);
}

TEST(test_amiga_track_round_trip) {
    // A track, read from any point round it for a revolution and a sector:
    // every sector, good, in order of where the read began.
    std::vector<uint8_t> data = randomTrack(1);
    std::vector<uint16_t> words(6250);
    CHECK(amiga::buildTrack(40, 1, amiga::SECTORS, data.data(), words.data(), words.size()));
    for (size_t start : {size_t(0), size_t(12345), size_t(77777)}) {
        std::vector<uint8_t> store(12 * 1024);
        amiga::SectorScanner scan(store.data(), store.size());
        cellsFrom(words, start, words.size() * 16 + amiga::SECTOR_WORDS * 16, [&](bool c) { scan.addCell(c); });
        bool all = true;
        std::vector<bool> seen(amiga::SECTORS);
        for (size_t i = 0; i < scan.count(); ++i) {
            const auto& s = scan.sector(i);
            all = all && s.ok && s.track == 81 && s.sector < amiga::SECTORS &&
                  std::memcmp(&store[s.data_offset], &data[s.sector * 512], 512) == 0;
            if (s.sector < amiga::SECTORS) seen[s.sector] = true;
        }
        CHECK(all);
        for (bool s : seen) CHECK(s);
    }
    // A flipped data bit fails the data checksum.
    words[200] ^= 0x0100;
    std::vector<uint8_t> store(12 * 1024);
    amiga::SectorScanner scan(store.data(), store.size());
    cellsFrom(words, 0, words.size() * 16, [&](bool c) { scan.addCell(c); });
    CHECK(scan.count() == amiga::SECTORS);
    CHECK(!scan.sector(0).ok);
    CHECK(scan.sector(1).ok);
}

// Feeds the same cells to one scanner a cell at a time and to another an
// interval at a time (the cells before the first 1 are skipped by both).
struct ScannerPair {
    std::vector<uint8_t> store_a = std::vector<uint8_t>(16 * 1024), store_b = std::vector<uint8_t>(16 * 1024);
    amiga::SectorScanner cells{store_a.data(), store_a.size()}, intervals{store_b.data(), store_b.size()};
    void feed(const std::vector<bool>& bits) {
        size_t first = 0;
        while (first < bits.size() && !bits[first]) ++first;
        unsigned n = 0;
        for (size_t i = first + 1; i < bits.size(); ++i) {
            ++n;
            if (!bits[i]) {
                if (n == 16) {  // a run too long for one interval: give up on it, both alike
                    for (unsigned k = 0; k < n; ++k) cells.addCell(false);
                    for (unsigned k = 0; k < n; ++k) intervals.addCell(false);
                    n = 0;
                }
                continue;
            }
            for (unsigned k = 1; k < n; ++k) cells.addCell(false);
            cells.addCell(true);
            intervals.addInterval(n);
            n = 0;
        }
    }
    bool same() const {
        if (cells.count() != intervals.count()) return false;
        for (size_t i = 0; i < cells.count(); ++i) {
            const auto& x = cells.sector(i);
            const auto& y = intervals.sector(i);
            if (x.track != y.track || x.sector != y.sector || x.ok != y.ok || x.data_offset != y.data_offset)
                return false;
        }
        return store_a == store_b;
    }
};

std::vector<bool> trackBits(const std::vector<uint16_t>& words, size_t start, size_t count) {
    std::vector<bool> bits;
    cellsFrom(words, start, count, [&](bool c) { bits.push_back(c); });
    return bits;
}

TEST(test_amiga_interval_path_matches_cells) {
    std::mt19937 rng(5);
    for (int round = 0; round < 20; ++round) {
        std::vector<uint8_t> data = randomTrack(uint32_t(round + 10));
        std::vector<uint16_t> words(6250);
        CHECK(amiga::buildTrack(uint8_t(round * 4 % 80), uint8_t(round & 1), amiga::SECTORS, data.data(), words.data(),
                                words.size()));
        std::vector<bool> bits = trackBits(words, rng() % (6250 * 16), 6250 * 16 + amiga::SECTOR_WORDS * 16);
        // From the third round on, flip runs of cells: sectors break off and
        // syncs turn up in odd places.
        for (int k = 0; round >= 2 && k < round * 10; ++k) {
            size_t at = rng() % bits.size();
            for (size_t j = at; j < bits.size() && j < at + rng() % 24; ++j) bits[j] = rng() & 1;
        }
        ScannerPair p;
        p.feed(bits);
        CHECK(p.same());
        if (round == 0) CHECK_EQ(p.intervals.count(), size_t(amiga::SECTORS));
    }
}

TEST(test_amiga_interval_path_sync_on_a_word_boundary) {
    // A sector's last two words are syncs, so the second ends exactly where
    // the sector's last word does. Cell by cell, the sync wins: that sector
    // is never finished, and a new one starts (soon cut off by the next real
    // sector's syncs). The interval path must not finish it either.
    std::vector<uint8_t> data = randomTrack(3);
    std::vector<uint16_t> words(6250);
    CHECK(amiga::buildTrack(10, 0, amiga::SECTORS, data.data(), words.data(), words.size()));
    // The first sector: its two syncs at word GAP_WORDS, then 540 words.
    size_t last = amiga::GAP_WORDS + 2 + 539;
    words[last - 1] = mfm::SYNC_A1;
    words[last] = mfm::SYNC_A1;
    ScannerPair p;
    p.feed(trackBits(words, 0, 6250 * 16));
    CHECK(p.same());
    unsigned good = 0;
    for (size_t i = 0; i < p.intervals.count(); ++i) good += p.intervals.sector(i).ok;
    CHECK_EQ(p.intervals.count(), size_t(amiga::SECTORS - 1));
    CHECK_EQ(good, amiga::SECTORS - 1);
}

TEST(test_adf_format) {
    ibm::Format f = ibm::amigaDD();
    CHECK(f.finish());
    CHECK_EQ(f.imageBytes(), amiga::ADF_BYTES);
    CHECK_EQ(f.track_bytes, 6250);
    CHECK(ImageList::imageName("Workbench1.3.ADF"));
    CHECK(ImageList::adfName("game.adf"));
    CHECK(!ImageList::adfName("game.img"));
}

// ---- The drive as an Amiga sees it ---------------------------------------------------

drive::Config amigaDrive(uint8_t unit) {
    drive::Config c;
    bus::Setup s{bus::Interface::Amiga, bus::Cable::Twisted, uint8_t(1u << unit)};
    c.lines = bus::lines(s);
    c.interface = bus::Interface::Amiga;
    c.spindle.spin_up_us = 1000;
    return c;
}

TEST(test_amiga_select_lines_and_settings) {
    bus::Setup s{bus::Interface::Amiga, bus::Cable::Twisted, 0b11};
    CHECK(bus::lines(s, 0) == (bus::Lines{cfg::PIN_MOTEA, cfg::PIN_MOTEB}));  // DF0: pin 10
    CHECK(bus::lines(s, 1) == (bus::Lines{cfg::PIN_DRVSB, cfg::PIN_MOTEB}));  // DF1: pin 12
    const char text[] = "interface=amiga\ndrive=df0,df1\npin02=chg\n";
    settings::Settings parsed = settings::parse(text, sizeof text - 1);
    CHECK(parsed.bus.interface == bus::Interface::Amiga);
    CHECK_EQ(parsed.bus.units, 0b11);
    CHECK(parsed.bus.pin2_chng);
    char out[settings::FILE_MAX];
    size_t n = settings::format(parsed, out, sizeof out);
    CHECK(settings::parse(out, n) == parsed);
    CHECK(std::string(out, n).find("drive=df0,df1\npin02=chg\n") != std::string::npos);
    // Pin 2 is a PC's density select: never driven there.
    const char pc[] = "interface=ibmpc\npin02=chg\n";
    CHECK(!settings::parse(pc, sizeof pc - 1).bus.pin2_chng);
    CHECK_EQ(settings::parse("drive=df1\ninterface=amiga\n", 26).bus.units, 0b10);
}

TEST(test_amiga_motor_latches_at_select) {
    drive::Drive d(amigaDrive(0));
    d.insertDisk(false);
    // The motor line alone does nothing...
    d.onSample(0, sample({cfg::PIN_MOTEB}));
    CHECK(!d.spindle().motorOn());
    // ...until the drive is selected with it asserted.
    d.onSample(10, sample({cfg::PIN_MOTEB, cfg::PIN_MOTEA}));
    CHECK(d.spindle().motorOn());
    // Deselected, and the motor line released (DF1's turn): DF0 keeps spinning.
    d.onSample(20, sample({cfg::PIN_DRVSB}));
    CHECK(d.spindle().motorOn());
    // Selected again with the line released: off.
    d.onSample(30, sample({cfg::PIN_MOTEA}));
    CHECK(!d.spindle().motorOn());
}

TEST(test_amiga_ready_and_id) {
    drive::Drive d(amigaDrive(0));
    d.insertDisk(false);
    // Motor off: /RDY gives the drive's ID, all ones (asserted): a 3.5-inch DD drive.
    d.onSample(0, sample({cfg::PIN_MOTEA}));
    CHECK(fdd::asserted(drive::outputPins(d.outputs(0), bus::Interface::Amiga), cfg::PIN_DSKCHG));
    // Motor on: not ready until it's at speed.
    d.onSample(100, sample({}));
    d.onSample(200, sample({cfg::PIN_MOTEB, cfg::PIN_MOTEA}));
    CHECK(!d.outputs(500).ready);
    CHECK(d.outputs(1300).ready);
    // Deselected: released.
    d.onSample(2000, sample({cfg::PIN_MOTEB}));
    CHECK(!fdd::asserted(drive::outputPins(d.outputs(2000), bus::Interface::Amiga), cfg::PIN_DSKCHG));
}

TEST(test_pin_2_disk_change) {
    // /CHNG on pin 2, after the carrier's rework and with pin02=chg.
    drive::Outputs o;
    o.dskchg = true;
    CHECK(fdd::asserted(drive::outputPins(o, bus::Interface::Amiga, true), cfg::PIN_CHNG));
    CHECK(fdd::asserted(drive::outputPins(o, bus::Interface::Shugart, true), cfg::PIN_CHNG));
    CHECK(!fdd::asserted(drive::outputPins(o, bus::Interface::Amiga, false), cfg::PIN_CHNG));  // not reworked
    CHECK(!fdd::asserted(drive::outputPins(o, bus::Interface::IbmPc, true), cfg::PIN_CHNG));   // the PC's
    o.dskchg = false;
    CHECK(!fdd::asserted(drive::outputPins(o, bus::Interface::Amiga, true), cfg::PIN_CHNG));
}

// ---- An Amiga's short selects, as the board sees them --------------------------------

// The drive on the board, driven the way an A1200's trackdisk drives it (seen
// on the bench, 6 Nov 2026): the select asserted for 4 us at a time to read
// the ID, and for about 20 us around each step, with DIR set and cleared
// alongside it. Core 1 samples the inputs once a pass (PASS_US); with
// `edges`, the PIO samplers also give the inputs at the instant STEP or the
// select falls, and the gate shows the levels from the last pass whenever
// the select is asserted, as amiga_pio does. Without, the drive is core 1's
// alone, as it was in 0.23.10.
class BoardAmiga {
public:
    static constexpr uint32_t PASS_US = 7;

    explicit BoardAmiga(bool edges, uint8_t start_cylinder = 5) : edges_(edges), d_(config(start_cylinder)) {
        d_.insertDisk(false);
        d_.useStepLatch(true);
        d_.useEdgeSamples(edges);
    }

    // trackdisk's drive ID: the motor latched on, then off, then 32 selects,
    // each reading /RDY 2 us in.
    uint32_t readId() {
        pulse(sample({cfg::PIN_MOTEA, cfg::PIN_MOTEB}), 2);
        pulse(sample({cfg::PIN_MOTEA}), 2);
        uint32_t id = 0;
        for (int bit = 0; bit < 32; ++bit) {
            set(sample({cfg::PIN_MOTEA}));
            run(2);
            id = id << 1 | (cable(cfg::PIN_DSKCHG) ? 1u : 0u);
            run(2);
            set(IDLE);
            run(4);
        }
        return id;
    }

    // Out until /TRK0, read 3 us after selecting, as trackdisk's recalibrate;
    // the steps taken.
    int recalibrate() {
        int steps = 0;
        for (; steps < 85; ++steps) {
            set(sample({cfg::PIN_MOTEA}));
            run(3);
            bool trk0 = cable(cfg::PIN_TRK00);
            if (trk0) {
                set(IDLE);
                break;
            }
            stepPulse(sample({cfg::PIN_MOTEA}));
            set(IDLE);
            run(3000);
        }
        return steps;
    }

    // In by `n`, DIR set with the select and cleared with it.
    void stepIn(int n) {
        for (int i = 0; i < n; ++i) {
            set(sample({cfg::PIN_MOTEA, cfg::PIN_DIR}));
            stepPulse(sample({cfg::PIN_MOTEA, cfg::PIN_DIR}));
            set(IDLE);
            run(3000);
        }
    }

    // A game's loader: DIR set in the same write that pulls STEP low, so at
    // STEP's falling edge it can still read as before (SWIV, on the A1200).
    void stepWithDir(int n, bool inwards) {
        for (int i = 0; i < n; ++i) {
            set(sample({cfg::PIN_MOTEA}));
            run(8);
            uint32_t step = sample({cfg::PIN_MOTEA, cfg::PIN_STEP});
            if (inwards) step &= ~fdd::maskOf(cfg::PIN_DIR);
            set(step, /*dir_late=*/true);
            run(2);
            set(step | fdd::maskOf(cfg::PIN_STEP));
            run(10);
            set(IDLE);
            run(3000);
        }
    }

    int cylinder() const { return d_.head().cylinder(); }
    drive::Drive& drive() { return d_; }

private:
    static constexpr uint32_t IDLE = fdd::INPUT_MASK;

    static drive::Config config(uint8_t start) {
        drive::Config c;
        c.lines = bus::lines(bus::Setup{bus::Interface::Amiga, bus::Cable::Twisted, 1});
        c.interface = bus::Interface::Amiga;
        c.start_cylinder = start;
        c.spindle.spin_up_us = 1000;
        return c;
    }

    // STEP low for 2 us, 8 us into the select, then the rest of 20 us.
    void stepPulse(uint32_t selected) {
        run(8);
        set(selected & ~fdd::maskOf(cfg::PIN_STEP));
        run(2);
        set(selected);
        run(10);
    }

    void pulse(uint32_t pins, uint32_t us) {
        set(pins);
        run(us);
        set(IDLE);
        run(us);
    }

    // The Amiga changes its lines: the samplers catch the edges. With
    // `dir_late`, DIR changes in the same write as STEP falls, and the
    // sampler (a couple of cycles in) still sees the old DIR at the fall.
    void set(uint32_t pins, bool dir_late = false) {
        bool step_fell = fdd::asserted(pins, cfg::PIN_STEP) && !fdd::asserted(pins_, cfg::PIN_STEP);
        bool step_rose = !fdd::asserted(pins, cfg::PIN_STEP) && fdd::asserted(pins_, cfg::PIN_STEP);
        bool select_fell = fdd::asserted(pins, cfg::PIN_MOTEA) && !fdd::asserted(pins_, cfg::PIN_MOTEA);
        uint32_t at_fall = pins;
        if (dir_late) at_fall = (pins & ~fdd::maskOf(cfg::PIN_DIR)) | (pins_ & fdd::maskOf(cfg::PIN_DIR));
        pins_ = pins;
        if (step_fell) {
            step_edge_ = true;  // the GPIO block's latch
            fall_ = at_fall;
        }
        if (edges_ && select_fell) d_.onSelectEdge(now_, pins);
        if (edges_ && step_rose) d_.onStepEdge(now_, rise_dir_ ? stepWord(fall_, pins) : fall_);
    }

    // fdd_step's word, as the board takes it apart (amiga_pio::stepPins).
    static uint32_t stepWord(uint32_t fall, uint32_t rise) {
        auto raw = [](uint32_t p) { return (p >> (cfg::FIRST_INPUT_PIN - cfg::BUS_FIRST_PIN)) & 0x3FFu; };
        return amiga_pio::stepPins(raw(fall) << 10 | raw(rise));
    }

public:
    bool rise_dir_ = true;  // false: DIR as STEP fell (0.24.0 as first written)

private:

    void run(uint32_t us) {
        for (uint32_t end = now_ + us; now_ != end; ++now_)
            if (now_ % PASS_US == 0) pass();
    }

    // Core 1's pass: the sample, then the outputs onto the pins.
    void pass() {
        d_.onSample(now_, pins_, step_edge_);
        step_edge_ = false;
        selected_levels_ = drive::outputPins(d_.outputsSelected(now_), bus::Interface::Amiga);
        levels_ = drive::outputPins(d_.outputs(now_), bus::Interface::Amiga);
    }

    // What the Amiga reads on an output now.
    bool cable(unsigned pin) const {
        uint32_t levels = edges_ ? (fdd::asserted(pins_, cfg::PIN_MOTEA) ? selected_levels_ : fdd::OUTPUT_MASK) : levels_;
        return fdd::asserted(levels, pin);
    }

    bool edges_;
    drive::Drive d_;
    uint32_t pins_ = IDLE;
    uint32_t fall_ = IDLE;
    uint32_t now_ = 0;
    bool step_edge_ = false;
    uint32_t levels_ = fdd::OUTPUT_MASK, selected_levels_ = fdd::OUTPUT_MASK;
};

TEST(test_amiga_short_selects_by_core_1_alone) {
    // 0.23.10 on the A1200: the ID read garbled, track 0 never seen (the head
    // knocking at the stop), steps lost or taken the wrong way.
    BoardAmiga a(false);
    uint32_t id = a.readId();
    int steps = a.recalibrate();
    a.stepIn(40);
    std::printf("    core 1 alone: ID %08x, recalibrate %d steps, cylinder %d (40 wanted)\n", unsigned(id), steps,
                a.cylinder());
    CHECK(id != 0xFFFFFFFFu || steps != 5 || a.cylinder() != 40);
}

TEST(test_amiga_short_selects_with_edge_samples) {
    // With the PIO samplers and gate: a 3.5-inch DD drive's ID (all ones),
    // track 0 seen on the first select there, and every step counted the
    // right way.
    BoardAmiga a(true);
    CHECK_EQ(a.readId(), 0xFFFFFFFFu);
    CHECK(!a.drive().spindle().motorOn());  // the ID's selects latched it off
    CHECK_EQ(a.recalibrate(), 5);
    CHECK_EQ(a.cylinder(), 0);
    a.stepIn(40);
    CHECK_EQ(a.cylinder(), 40);
}

TEST(test_amiga_dir_set_with_the_step) {
    // A game's loader setting DIR in the same write as STEP falls: taken at
    // the fall, DIR is the old one and the head goes the wrong way (SWIV
    // stepped out from 79 when it asked for in); taken as STEP rises, right.
    BoardAmiga at_fall(true, 40);
    at_fall.rise_dir_ = false;
    at_fall.stepWithDir(5, true);
    CHECK(at_fall.cylinder() != 45);
    BoardAmiga at_rise(true, 40);
    at_rise.stepWithDir(5, true);
    CHECK_EQ(at_rise.cylinder(), 45);
    at_rise.stepWithDir(7, false);
    CHECK_EQ(at_rise.cylinder(), 38);
}

TEST(test_amiga_edge_samples_ignore_other_drives) {
    // DF1's select and steps on the shared lines: DF0 neither steps nor
    // latches its motor from them.
    drive::Drive d(amigaDrive(0));
    d.useEdgeSamples(true);
    d.onSelectEdge(0, sample({cfg::PIN_MOTEA, cfg::PIN_MOTEB}));
    CHECK(d.spindle().motorOn());
    uint8_t before = d.head().cylinder();
    CHECK(!d.onStepEdge(10, sample({cfg::PIN_DRVSB, cfg::PIN_DIR})).stepped);
    CHECK_EQ(d.head().cylinder(), before);
    CHECK(d.onStepEdge(20, sample({cfg::PIN_MOTEA, cfg::PIN_DIR})).stepped);
    CHECK_EQ(d.head().cylinder(), before + 1);
    // A sample a pass later, deselected: no second step, and the latch holds.
    d.onSample(30, sample({cfg::PIN_MOTEB}), true);
    CHECK_EQ(d.head().cylinder(), before + 1);
    CHECK(d.spindle().motorOn());
}

// ---- A virtual Amiga -----------------------------------------------------------------

// trackdisk.device's way with DF0: motor on (latched at select), wait for
// /RDY, seek, then read a track's worth of DMA from wherever the head is, or
// write a whole track (WGATE for a revolution and a little more) likewise.
class VirtualAmiga : public VirtualHost {
public:
    static constexpr uint32_t STEP_US = 3000;  // trackdisk's step rate
    static constexpr uint32_t SETTLE_US = 18000;

    explicit VirtualAmiga(Machine& m) : VirtualHost(m.driveModels(), cfg::PIN_MOTEA, cfg::PIN_MOTEB), m_(m) {
        m_.attach(*this);
    }

    void run(uint32_t us) {
        while (us) {
            uint32_t slice = us > 1000 ? 1000 : us;
            wait(slice);
            us -= slice;
            m_.tick(now);
        }
    }

    // Motor on: the line first, then select, which latches it. Then /RDY.
    bool motorOn() {
        set(motor_, true);
        set(select_, true);
        for (int i = 0; i < 1000 && !cable().ready; ++i) run(1000);
        return cable().ready;
    }

    bool seek(int cyl) {
        set(cfg::PIN_DIR, false);
        for (int i = 0; i < 90 && !trk0(); ++i) step();
        if (!trk0()) return false;
        set(cfg::PIN_DIR, true);
        for (int i = 0; i < cyl; ++i) step();
        run(SETTLE_US);
        return true;
    }

    // A revolution and a sector of RDATA through the Amiga's sector decoder.
    std::vector<uint8_t> readTrack(uint8_t cyl, uint8_t head, unsigned& good) {
        set(cfg::PIN_SIDE1, head == 1);
        std::vector<uint8_t> store(12 * 1024), track(amiga::SECTORS * amiga::SECTOR_BYTES);
        amiga::SectorScanner scan(store.data(), store.size());
        const ibm::Format& f = m_.format();
        uint32_t per_byte = 8000000u / f.dataRate();
        for (uint32_t n = 0; n < f.track_bytes + amiga::SECTOR_WORDS + 4; ++n) {
            uint16_t cells = m_.rdataCells(now, head, position());
            for (int b = 15; b >= 0; --b) scan.addCell((cells >> b) & 1);
            run(per_byte);
        }
        good = 0;
        std::vector<bool> got(amiga::SECTORS);
        for (size_t i = 0; i < scan.count(); ++i) {
            const auto& s = scan.sector(i);
            if (!s.ok || s.track != cyl * 2 + head || s.sector >= amiga::SECTORS || got[s.sector]) continue;
            got[s.sector] = true;
            ++good;
            std::memcpy(&track[s.sector * 512], &store[s.data_offset], 512);
        }
        return track;
    }

    // The whole track, from wherever the head is, as WDATA intervals.
    void writeTrack(uint8_t cyl, uint8_t head, const std::vector<uint8_t>& data) {
        set(cfg::PIN_SIDE1, head == 1);
        std::vector<uint16_t> words(m_.format().track_bytes);
        amiga::buildTrack(cyl, head, amiga::SECTORS, data.data(), words.data(), uint32_t(words.size()));
        // The Amiga's buffer: the sectors and a little gap; a revolution in
        // all, in real time: core 0 decodes as it arrives, as on the board
        // (a track is twice the capture ring).
        set(cfg::PIN_WGATE, true);
        uint32_t since = 0;
        uint32_t per_word = 8000000u / m_.format().dataRate();  // 16 cells
        for (uint16_t w : words) {
            for (int b = 15; b >= 0; --b) {
                since += Machine::TICKS_PER_CELL;
                if ((w >> b) & 1) {
                    m_.captureInterval(since);
                    since = 0;
                }
            }
            wait(per_word);
            m_.tick(now);
        }
        set(cfg::PIN_WGATE, false);
        run(2000);
    }

private:
    void step() {
        set(cfg::PIN_STEP, true);
        wait(2);
        set(cfg::PIN_STEP, false);
        run(STEP_US);
    }
    uint32_t position() {
        return uint32_t(uint64_t(selectedDrive().spindle().phaseUs(now)) * m_.format().dataRate() / 8000000u);
    }

    Machine& m_;
};

TEST(test_virtual_amiga_reads_and_writes_tracks) {
    bus::Setup amiga{bus::Interface::Amiga, bus::Cable::Twisted, 0b01};
    ibm::Format adf = ibm::amigaDD();
    Machine m(amiga, {&adf}, Core0Costs{}, 0);
    VirtualAmiga a(m);
    CHECK(a.motorOn());
    unsigned good = 0;
    for (int cyl : {0, 40, 79}) {
        CHECK(a.seek(cyl));
        for (uint8_t head = 0; head < 2; ++head) {
            std::vector<uint8_t> got = a.readTrack(uint8_t(cyl), head, good);
            CHECK_EQ(good, amiga::SECTORS);
            CHECK(std::memcmp(got.data(), m.source.sector(uint8_t(cyl), head, 0), got.size()) == 0);
        }
    }
    // Write cylinder 79 head 1 from wherever the disk is, then read it back:
    // from the live track, and from the image.
    std::vector<uint8_t> data = randomTrack(7);
    a.writeTrack(79, 1, data);
    CHECK_EQ(m.source.writes, amiga::SECTORS);
    CHECK(std::memcmp(m.source.sector(79, 1, 0), data.data(), data.size()) == 0);
    std::vector<uint8_t> back = a.readTrack(79, 1, good);
    CHECK_EQ(good, amiga::SECTORS);
    CHECK(back == data);
    for (const auto& w : m.writes) CHECK(w.decoded.bad_crc == 0 && w.decoded.unplaced == 0);
    // And from the image after the head has been away.
    CHECK(a.seek(3));
    CHECK(a.seek(79));
    back = a.readTrack(79, 1, good);
    CHECK(back == data);
}

}  // namespace

void runAmigaTests() {
    RUN(test_amiga_checksum);
    RUN(test_amiga_track_round_trip);
    RUN(test_amiga_interval_path_matches_cells);
    RUN(test_amiga_interval_path_sync_on_a_word_boundary);
    RUN(test_adf_format);
    RUN(test_amiga_select_lines_and_settings);
    RUN(test_amiga_motor_latches_at_select);
    RUN(test_amiga_ready_and_id);
    RUN(test_pin_2_disk_change);
    RUN(test_amiga_short_selects_by_core_1_alone);
    RUN(test_amiga_short_selects_with_edge_samples);
    RUN(test_amiga_dir_set_with_the_step);
    RUN(test_amiga_edge_samples_ignore_other_drives);
    RUN(test_virtual_amiga_reads_and_writes_tracks);
}
