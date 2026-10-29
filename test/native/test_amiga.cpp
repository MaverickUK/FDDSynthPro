// Tests for the Amiga: AmigaDOS tracks (src/amiga_track.*), the drive as an
// Amiga sees it (latched motor, /RDY and the drive ID, /CHNG on pin 2), and
// a virtual Amiga reading and writing whole tracks through the whole drive,
// as trackdisk.device does: from wherever the head is, with no index.
// Greaseweazle checks the tracks themselves (test_track_encoder.py).
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <random>
#include <vector>

#include "amiga_track.h"
#include "bus.h"
#include "check.h"
#include "drive.h"
#include "image_list.h"
#include "machine.h"
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
    RUN(test_adf_format);
    RUN(test_amiga_select_lines_and_settings);
    RUN(test_amiga_motor_latches_at_select);
    RUN(test_amiga_ready_and_id);
    RUN(test_pin_2_disk_change);
    RUN(test_virtual_amiga_reads_and_writes_tracks);
}
