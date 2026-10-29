// A virtual PC drives the whole drive through the cable (virtual_pc.h): a
// DOS-like session of boot, file reads, writes and a disk swap, with core 0
// loading cylinders in the background at the SD card's speed. The sound events of
// the session are saved for the sample player
// (.pio/native-tests/pc_session_act.txt).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "check.h"
#include "config.h"
#include "image_list.h"
#include "machine.h"
#include "virtual_pc.h"

using Status = VirtualPc::Status;

namespace {  // this file's own helpers and tests

static std::vector<uint8_t> sectors(const MemorySource& src, uint8_t c, uint8_t h, uint8_t first, uint8_t n) {
    std::vector<uint8_t> out;
    for (uint8_t i = 0; i < n; ++i) out.insert(out.end(), src.sector(c, h, uint8_t(first - 1 + i)),
                                                src.sector(c, h, uint8_t(first - 1 + i)) + 512);
    return out;
}

static unsigned countSounds(const std::vector<activity::Event>& ev, activity::Sound s) {
    unsigned n = 0;
    for (const auto& e : ev) n += e.sound == s;
    return n;
}

static unsigned movingSteps(const std::vector<activity::Event>& ev) {
    unsigned n = 0;
    for (const auto& e : ev) {
        if (e.sound == activity::Sound::Step) ++n;
        if (e.sound == activity::Sound::SeekEnd) n += e.steps - 1u;
    }
    return n;
}

static void saveSounds(const std::vector<activity::Event>& ev, const char* path) {
    FILE* f = std::fopen(path, "w");
    if (!f) return;
    for (const auto& e : ev) {
        char line[64];
        size_t n = activity::format(e, line, sizeof line);
        std::fwrite(line, 1, n, f);
    }
    std::fclose(f);
}

TEST(test_pc_dos_session) {
    Machine m(ibm::HD_1440, Core0Costs{}, 30);  // the head where the last session left it
    VirtualPc pc(m);
    std::vector<uint8_t> buf(18 * 512);

    // POST and boot: motor on, recalibrate, the boot sector.
    pc.motorOn();
    CHECK(pc.diskChanged());  // set since power-on
    CHECK(pc.recalibrate());
    CHECK(!pc.diskChanged());  // the steps, with a disk in, cleared it
    CHECK(pc.readSectors(0, 0, 1, 1, buf.data()) == Status::Ok);
    CHECK(std::memcmp(buf.data(), m.source.sector(0, 0, 0), 512) == 0);

    // DOS: the FAT (9 sectors) and the root directory (14, across both heads).
    CHECK(pc.readSectors(0, 0, 2, 9, buf.data()) == Status::Ok);
    CHECK(std::vector<uint8_t>(buf.begin(), buf.begin() + 9 * 512) == sectors(m.source, 0, 0, 2, 9));
    CHECK(pc.readSectors(0, 0, 11, 8, buf.data()) == Status::Ok);
    CHECK(pc.readSectors(0, 1, 1, 6, buf.data() + 8 * 512) == Status::Ok);
    CHECK(std::memcmp(buf.data() + 8 * 512, m.source.sector(0, 1, 0), 6 * 512) == 0);

    // A file across the disk: a seek, then a whole track.
    CHECK(pc.readSectors(40, 0, 1, 18, buf.data()) == Status::Ok);
    CHECK(buf == sectors(m.source, 40, 0, 1, 18));

    // Write a file: a whole track of new data, then the FAT.
    std::vector<uint8_t> file(18 * 512);
    for (size_t i = 0; i < file.size(); ++i) file[i] = uint8_t(i * 31 + 7);
    CHECK(pc.writeSectors(41, 1, 1, 18, file.data()) == Status::Ok);
    std::vector<uint8_t> fat(512, 0xF7);
    CHECK(pc.writeSectors(0, 0, 2, 1, fat.data()) == Status::Ok);
    CHECK_EQ(m.source.writes, 19u);
    CHECK(std::memcmp(m.source.sector(41, 1, 0), file.data(), file.size()) == 0);

    // Read the file back: from the card, after the head has been away.
    CHECK(pc.readSectors(41, 1, 1, 18, buf.data()) == Status::Ok);
    CHECK(buf == file);
    CHECK(pc.readSectors(0, 0, 2, 1, buf.data()) == Status::Ok);
    CHECK(std::memcmp(buf.data(), fat.data(), 512) == 0);

    // A disk swap: the BIOS sees the change, clears it, and DOS rereads.
    m.d.ejectDisk();
    pc.run(500000);
    m.d.insertDisk(false);
    pc.run(500000);
    CHECK(pc.diskChanged());
    pc.clearDiskChange();
    CHECK(!pc.diskChanged());
    CHECK(pc.readSectors(0, 0, 1, 1, buf.data()) == Status::Ok);

    pc.motorOff();
    pc.run(100000);

    // Everything first time: the loads kept up with every seek.
    CHECK_EQ(pc.retries, 0u);
    for (const auto& w : m.writes) CHECK(w.decoded.bad_crc == 0 && w.decoded.unplaced == 0);

    // The sounds: motor on to off, every step heard, no bumps (the BIOS
    // checks TRK00), the swap.
    const auto& ev = m.sounds;
    CHECK(!ev.empty() && ev.front().sound == activity::Sound::MotorOn);
    CHECK(!ev.empty() && ev.back().sound == activity::Sound::MotorOff);
    CHECK_EQ(movingSteps(ev), unsigned(pc.steps.size()));
    CHECK_EQ(countSounds(ev, activity::Sound::Bump), 0u);
    CHECK_EQ(countSounds(ev, activity::Sound::Eject), 1u);
    CHECK_EQ(countSounds(ev, activity::Sound::Insert), 1u);
    CHECK(countSounds(ev, activity::Sound::SeekStart) >= 3u);  // recalibrate, to 40, back to 0
    saveSounds(ev, ".pio/native-tests/pc_session_act.txt");
}

TEST(test_pc_two_drives) {
    // The emulator as A (1.44 MB) and B (720 KB) on one cable: the BIOS
    // changes drives with A's motor still running, finds B's data rate, and
    // reads and writes both. Each drive keeps its own head, disk and rate.
    Machine m(bus::Setup{bus::Interface::IbmPc, bus::Cable::Twisted, 0b11}, {&ibm::HD_1440, &ibm::DD_720});
    MemorySource& a = m.unit(0).source;
    MemorySource& b = m.unit(1).source;
    VirtualPc pc(m);
    std::vector<uint8_t> buf(18 * 512);

    pc.motorOn();
    CHECK(pc.recalibrate());
    CHECK(pc.readSectors(40, 1, 1, 18, buf.data()) == Status::Ok);
    CHECK(buf == sectors(a, 40, 1, 1, 18));
    CHECK_EQ(m.unit(0).d.head().cylinder(), 40);

    pc.useDrive(1);
    CHECK(pc.diskChanged());  // B's own, set since power-on
    CHECK(pc.detectRate());
    CHECK_EQ(pc.rate_bps, 250000u);
    pc.clearDiskChange();
    CHECK(pc.readSectors(10, 0, 1, 9, buf.data()) == Status::Ok);
    CHECK(std::vector<uint8_t>(buf.begin(), buf.begin() + 9 * 512) == sectors(b, 10, 0, 1, 9));
    CHECK_EQ(m.unit(0).d.head().cylinder(), 40);  // A's head stayed put
    CHECK_EQ(m.unit(1).d.head().cylinder(), 10);

    std::vector<uint8_t> file(9 * 512);
    for (size_t i = 0; i < file.size(); ++i) file[i] = uint8_t(i * 13 + 5);
    CHECK(pc.writeSectors(20, 1, 1, 9, file.data()) == Status::Ok);
    CHECK_EQ(b.writes, 9u);
    CHECK_EQ(a.writes, 0u);
    CHECK(std::memcmp(b.sector(20, 1, 0), file.data(), file.size()) == 0);

    // Back to A, at its own rate, where its head was: no recalibration.
    unsigned recalibrations = pc.recalibrations;
    pc.useDrive(0);
    CHECK_EQ(pc.rate_bps, 500000u);
    CHECK(pc.readSectors(41, 0, 1, 18, buf.data()) == Status::Ok);
    CHECK(buf == sectors(a, 41, 0, 1, 18));
    CHECK(pc.writeSectors(41, 0, 5, 1, file.data()) == Status::Ok);
    CHECK_EQ(a.writes, 1u);
    CHECK_EQ(b.writes, 9u);
    CHECK_EQ(pc.recalibrations, recalibrations);

    // B again, read back what was written: from its own buffer.
    pc.useDrive(1);
    CHECK(pc.readSectors(20, 1, 1, 9, buf.data()) == Status::Ok);
    CHECK(std::vector<uint8_t>(buf.begin(), buf.begin() + 9 * 512) == file);
    CHECK_EQ(pc.retries, 0u);
    for (const auto& w : m.writes) CHECK(w.decoded.bad_crc == 0 && w.decoded.unplaced == 0);

    // One mechanism to hear: the motor started once, though both now run.
    CHECK_EQ(countSounds(m.sounds, activity::Sound::MotorOn), 1u);
    CHECK_EQ(movingSteps(m.sounds), unsigned(pc.steps.size()));
    pc.motorOff();  // B's
    CHECK_EQ(countSounds(m.sounds, activity::Sound::MotorOff), 0u);  // A's still runs
    pc.useDrive(0);
    pc.motorOff();
    CHECK_EQ(countSounds(m.sounds, activity::Sound::MotorOff), 1u);
}

TEST(test_pc_second_drive_ignores_the_first_drives_lines) {
    // Ejecting B leaves A's disk in, and A never sees B's steps.
    Machine m(bus::Setup{bus::Interface::IbmPc, bus::Cable::Straight, 0b11}, {&ibm::HD_1440, &ibm::HD_1440});
    // On a straight cable, A is the 14/10 pair: the virtual PC's twisted A
    // (12/16) reaches drive B.
    CHECK(m.unit(1).d.config().lines.select == cfg::PIN_DRVSB);
    VirtualPc pc(m);
    pc.motorOn();
    CHECK(pc.recalibrate());
    CHECK(pc.seekTo(30));
    CHECK_EQ(m.unit(1).d.head().cylinder(), 30);
    CHECK_EQ(m.unit(0).d.head().cylinder(), 0);
    m.eject(1);
    pc.run(10000);
    CHECK(pc.diskChanged());
    pc.useDrive(1);  // 14/10: drive 0 here
    CHECK(m.unit(0).d.selected());
    CHECK(m.unit(0).d.diskPresent());
}

TEST(test_pc_head_switch_without_settle) {
    // DOS reads clusters across the end of a track: the last sector of
    // head 0, then head 1 straight away, with no step and no settle.
    Machine m;
    VirtualPc pc(m);
    std::vector<uint8_t> buf(2 * 512);
    pc.motorOn();
    pc.recalibrate();
    CHECK(pc.readSectors(5, 0, 18, 1, buf.data()) == Status::Ok);
    CHECK(pc.readSectors(5, 1, 1, 1, buf.data() + 512) == Status::Ok);
    CHECK(std::memcmp(buf.data() + 512, m.source.sector(5, 1, 0), 512) == 0);
    CHECK_EQ(pc.retries, 0u);
}

TEST(test_pc_write_protected) {
    Machine m(ibm::HD_1440, Core0Costs{}, 0, false);
    VirtualPc pc(m);
    std::vector<uint8_t> data(512, 0x55);
    pc.motorOn();
    pc.recalibrate();
    CHECK(pc.writeSectors(3, 0, 1, 1, data.data()) == Status::WriteProtected);
    CHECK_EQ(m.source.writes, 0u);
    CHECK(m.writes.empty());  // WGATE was never asserted
}

TEST(test_pc_dd_disk) {
    Machine m(ibm::DD_720);
    VirtualPc pc(m);
    std::vector<uint8_t> buf(9 * 512);
    pc.motorOn();
    pc.recalibrate();
    // The controller starts at 500 kbit/s: nothing reads until the BIOS
    // finds the disk's rate.
    CHECK(pc.readSectors(0, 0, 1, 1, buf.data()) == Status::SectorNotFound);
    CHECK(pc.detectRate());
    CHECK_EQ(pc.rate_bps, 250000u);
    CHECK(pc.readSectors(79, 1, 1, 9, buf.data()) == Status::Ok);
    CHECK(buf == sectors(m.source, 79, 1, 1, 9));
    std::vector<uint8_t> data(512, 0xA5);
    CHECK(pc.writeSectors(20, 0, 9, 1, data.data()) == Status::Ok);
    CHECK(std::memcmp(m.source.sector(20, 0, 8), data.data(), 512) == 0);
}

TEST(test_pc_gives_up_on_a_card_that_never_answers) {
    // A card so slow (10 s a track) that the cylinder is never ready within
    // the BIOS's three tries: RDATA stays released, and the read fails
    // rather than returning anything stale.
    Machine m(ibm::HD_1440, Core0Costs{10000000, 2500});
    VirtualPc pc(m);
    std::vector<uint8_t> buf(512);
    pc.motorOn();
    pc.recalibrate();
    CHECK(pc.readSectors(60, 0, 1, 1, buf.data()) == Status::SectorNotFound);
    CHECK_EQ(pc.retries, 2u);
}

TEST(test_pc_disk_swaps) {
    // GoTek style: a 1.44 MB disk, swapped for a 720 KB one, then back. The
    // BIOS sees each change, finds each disk's rate, and reads the right
    // disk; writes go to the disk that's in.
    Machine m;
    MemorySource dd(ibm::DD_720);
    VirtualPc pc(m);
    std::vector<uint8_t> buf(512);
    pc.motorOn();
    pc.recalibrate();
    CHECK(pc.readSectors(10, 0, 1, 1, buf.data()) == Status::Ok);

    m.eject();
    pc.run(500000);
    m.insert(dd);
    pc.run(100000);
    CHECK(pc.diskChanged());
    pc.clearDiskChange();
    CHECK(pc.detectRate());
    CHECK_EQ(pc.rate_bps, 250000u);
    CHECK(pc.readSectors(10, 0, 1, 1, buf.data()) == Status::Ok);
    CHECK(std::memcmp(buf.data(), dd.sector(10, 0, 0), 512) == 0);
    std::vector<uint8_t> data(512, 0x3C);
    CHECK(pc.writeSectors(12, 1, 9, 1, data.data()) == Status::Ok);
    CHECK(std::memcmp(dd.sector(12, 1, 8), data.data(), 512) == 0);
    CHECK_EQ(m.source.writes, 0u);  // not the 1.44 MB disk

    m.eject();
    pc.run(500000);
    m.insert(m.source);
    pc.run(100000);
    CHECK(pc.diskChanged());
    pc.clearDiskChange();
    CHECK(pc.detectRate());
    CHECK_EQ(pc.rate_bps, 500000u);
    CHECK(pc.readSectors(10, 0, 1, 1, buf.data()) == Status::Ok);
    CHECK(std::memcmp(buf.data(), m.source.sector(10, 0, 0), 512) == 0);
    CHECK_EQ(countSounds(m.sounds, activity::Sound::Eject), 2u);
    CHECK_EQ(countSounds(m.sounds, activity::Sound::Insert), 2u);
}

TEST(test_pc_write_to_a_failing_card) {
    // The card fails mid-session. The PC's write seems to go (a floppy
    // can't say otherwise), but its read-back shows the old data, and its
    // next write gets write-protect. Re-inserting the disk with the card
    // working again clears it.
    Machine m;
    VirtualPc pc(m);
    std::vector<uint8_t> buf(512), data(512, 0x42), old(m.source.sector(8, 0, 3), m.source.sector(8, 0, 3) + 512);
    pc.motorOn();
    pc.recalibrate();
    m.source.failing = true;
    CHECK(pc.writeSectors(8, 0, 4, 1, data.data()) == Status::Ok);
    CHECK(pc.readSectors(8, 0, 4, 1, buf.data()) == Status::Ok);
    CHECK(buf == old);                               // not the data it wrote
    CHECK(pc.writeSectors(8, 0, 5, 1, data.data()) == Status::WriteProtected);
    CHECK(!m.writes.empty() && lostWrite(m.writes.back()));

    m.source.failing = false;
    m.eject();
    pc.run(500000);
    m.insert(m.source);
    pc.run(100000);
    pc.clearDiskChange();
    CHECK(pc.writeSectors(8, 0, 4, 1, data.data()) == Status::Ok);
    CHECK(pc.readSectors(8, 0, 4, 1, buf.data()) == Status::Ok);
    CHECK(buf == data);
}

// Seeks, reading one sector after each, as a busy DOS program does: every
// load against the settle time, with the panel drawing throughout. Half are
// to a random cylinder, half a single step on (as reading a file does). A
// single step is the hard case: a multi-step seek's own stepping outlasts a
// frame drawn when it began. Returns the retries; `bad` counts sectors
// that read back wrong.
static unsigned seekSession(Machine& m, int seeks, unsigned& bad) {
    VirtualPc pc(m);
    pc.motorOn();
    pc.recalibrate();
    uint32_t seed = 1987;
    std::vector<uint8_t> buf(512);
    bad = 0;
    uint8_t cyl = 0;
    for (int i = 0; i < seeks; ++i) {
        seed = seed * 1103515245 + 12345;
        if (i % 2) cyl = uint8_t(cyl == 79 ? 78 : cyl + 1);
        else cyl = uint8_t((seed >> 8) % 80);
        uint8_t head = uint8_t((seed >> 16) & 1), r = uint8_t(1 + (seed >> 20) % 18);
        if (pc.readSectors(cyl, head, r, 1, buf.data()) != Status::Ok ||
            std::memcmp(buf.data(), m.source.sector(cyl, head, uint8_t(r - 1)), 512) != 0)
            ++bad;
    }
    pc.motorOff();
    return pc.retries;
}

// The I2S queue: what core 0 can be away for before the sound runs dry.
constexpr uint32_t AUDIO_QUEUE_US = uint32_t(uint64_t(cfg::AUDIO_BUFFERS) * cfg::AUDIO_BLOCK_FRAMES * 1000000 / 22050);

TEST(test_pc_panel_keeps_up) {
    // The front panel drawing a frame every 100 ms (2 ms each, pessimistic)
    // never makes a cylinder late for the PC's 15 ms settle time, and never
    // keeps audio waiting longer than a fraction of its queue.
    Machine m;
    unsigned bad = 0;
    CHECK_EQ(seekSession(m, 150, bad), 0u);
    CHECK_EQ(bad, 0u);
    CHECK(m.frames > 0);
    CHECK(m.slowest_ready_us > 0);
    CHECK(m.slowest_ready_us <= VirtualPc::SETTLE_US);
    CHECK(m.longest_step_us < AUDIO_QUEUE_US / 4);  // 46 ms of audio queued
    std::printf("    slowest cylinder ready %.1f ms after the last step (settle %.0f ms)\n", m.slowest_ready_us / 1000.0,
                VirtualPc::SETTLE_US / 1000.0);
}

TEST(test_pc_panel_frame_rate) {
    // Frames are held back only while a load is under way, so even through
    // constant seeking the panel keeps nearly all of its 10 frames a second.
    Machine m;
    VirtualPc pc(m);
    pc.motorOn();
    pc.recalibrate();
    uint32_t t0 = pc.now;
    unsigned f0 = m.frames;
    std::vector<uint8_t> buf(512);
    for (int i = 0; i < 60; ++i) pc.readSectors(uint8_t((i * 37) % 80), 0, 1, 1, buf.data());
    double seconds = (pc.now - t0) / 1e6;
    unsigned frames = m.frames - f0;
    CHECK(frames >= unsigned(seconds * 10 * 0.95));
    CHECK(frames <= unsigned(seconds * 10) + 1);
}

// The worst case for the settle time: a frame starts just as the head steps
// a single cylinder. The load waits for the frame, then takes two reads and
// two encodes (7 ms with these costs). Returns how long after the step the
// cylinder was ready.
static uint32_t readyAfterStepDuringFrame(uint32_t draw_us) {
    Core0Costs costs;
    costs.draw_us = draw_us;
    Machine m(ibm::HD_1440, costs);
    VirtualPc pc(m);
    pc.motorOn();
    pc.recalibrate();
    std::vector<uint8_t> buf(512);
    pc.readSectors(10, 0, 1, 1, buf.data());  // settled on cylinder 10, nothing loading
    pc.run(5000);
    uint32_t due = m.nextFrameUs();
    while (int32_t(pc.now - due) < 0) pc.run(50);  // the frame has just started
    m.slowest_ready_us = 0;
    pc.seekTo(11);
    pc.run(40000);
    return m.slowest_ready_us;
}

TEST(test_pc_panel_margin) {
    // How slow could the board's frames be, at worst, before a cylinder is
    // late for the PC's 15 ms settle time? The frame and the load share core
    // 0, so the answer is the settle time less the load. Require room to
    // spare over the 2 ms assumed.
    uint32_t slowest_ok = 0;
    for (uint32_t draw_us = 500; draw_us <= 8000; draw_us += 250) {
        uint32_t ready = readyAfterStepDuringFrame(draw_us);
        const uint32_t load_us = 2 * Core0Costs{}.read_us + 2 * Core0Costs{}.encode_us;
        CHECK(ready >= load_us && ready <= draw_us + load_us);  // the frame's rest, then the load
        if (ready > VirtualPc::SETTLE_US) break;
        slowest_ok = draw_us;
    }
    std::printf("    panel margin: frames of up to %.2f ms keep every cylinder within the settle time "
                "(2 ms assumed)\n", slowest_ok / 1000.0);
    CHECK(slowest_ok >= 3500);
    // And the frame really does delay it: a slower frame, a later cylinder.
    CHECK(readyAfterStepDuringFrame(4000) > readyAfterStepDuringFrame(1000));
}

TEST(test_image_list) {
    ImageList l;
    CHECK(l.add("games.IMG", 1474560));
    CHECK(l.add("Apps.ima", 737280));
    CHECK(l.add("dos622.img", 1474560));
    CHECK(!l.add("notes.txt", 1474560));      // not an image name
    CHECK(!l.add("big.img", 2949120));        // 2.88 MB: not served
    CHECK(!l.add("x.img", 1000));
    CHECK_EQ(l.count(), 3u);
    CHECK_STR(l.name(0), "Apps.ima");  // name order, ignoring case
    CHECK_STR(l.name(1), "dos622.img");
    CHECK_STR(l.name(2), "games.IMG");
    CHECK_EQ(l.size(0), 737280u);
    CHECK_EQ(l.next(2), 0u);  // wraps
    CHECK_EQ(l.prev(0), 2u);
    ImageList empty;
    CHECK_EQ(empty.next(0), 0u);
}

}  // namespace

void runVirtualPcTests() {
    RUN(test_pc_dos_session);
    RUN(test_pc_two_drives);
    RUN(test_pc_second_drive_ignores_the_first_drives_lines);
    RUN(test_pc_head_switch_without_settle);
    RUN(test_pc_write_protected);
    RUN(test_pc_dd_disk);
    RUN(test_pc_gives_up_on_a_card_that_never_answers);
    RUN(test_pc_disk_swaps);
    RUN(test_pc_write_to_a_failing_card);
    RUN(test_pc_panel_keeps_up);
    RUN(test_pc_panel_frame_rate);
    RUN(test_pc_panel_margin);
    RUN(test_image_list);
}
