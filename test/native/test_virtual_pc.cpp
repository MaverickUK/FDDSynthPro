// A virtual PC drives the whole drive through the cable (virtual_pc.h): a
// DOS-like session of boot, file reads, writes and a disk swap, with core 0
// loading cylinders in the background at SDIO speed. The sound events of
// the session are saved for the sample player
// (.pio/native-tests/pc_session_act.txt).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "check.h"
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
    RUN(test_pc_head_switch_without_settle);
    RUN(test_pc_write_protected);
    RUN(test_pc_dd_disk);
    RUN(test_pc_gives_up_on_a_card_that_never_answers);
    RUN(test_pc_disk_swaps);
    RUN(test_image_list);
}
