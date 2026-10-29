// A PC's BIOS and floppy controller (an AT with an 82077-style FDC), driving
// a Machine through the cable. It reads by watching the drive's RDATA cells
// go past under the head, and writes by sending WDATA into the drive's
// capture with the controller's timing, so the whole loop runs: pins, drive
// model, loader, streamer buffer, write path, image.
//
// What it models, from the BIOS and controller's behaviour:
//   - Drive A, after the cable's twist: select and motor on pins 12/16.
//     Drive B on 14/10 (useDrive). Motor on, then 500 ms before the first
//     access. Changing drives leaves the last one's motor running, as the
//     BIOS does until its motor-off timeout.
//   - Steps every 3 ms (SRT for 1.44 MB), STEP held 2 us; 15 ms head settle.
//   - RECALIBRATE steps out until TRK00, checking it before each step, up
//     to 79 steps.
//   - Finding a sector: read ID fields as they come round; give up after
//     two index pulses ("sector not found").
//   - A sector write: after its ID field, skip gap 2, then WGATE on and the
//     data field (12 x 00, A1 A1 A1, FB, data, CRC, one 4E), WGATE off.
//   - Errors: three tries, recalibrating between them, as INT 13h does.
//   - Disk change: the BIOS reads DSKCHG before an access; a change is
//     cleared by stepping (here: to cylinder 1 and back).
//   - Data rate: the controller reads at one rate (500 kbit/s for 1.44 MB,
//     250 for 720 KB). At the wrong rate it finds nothing; after a disk
//     change the BIOS tries each rate in turn (media detection).
#pragma once

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "crc16.h"
#include "machine.h"
#include "mfm.h"
#include "mfm_decode.h"

class VirtualPc : public VirtualHost {
public:
    enum class Status : uint8_t { Ok, SectorNotFound, WriteProtected, NotReady };

    static constexpr uint32_t STEP_US = 3000;
    static constexpr uint32_t STEP_PULSE_US = 2;
    static constexpr uint32_t SETTLE_US = 15000;
    static constexpr uint32_t MOTOR_START_US = 500000;
    static constexpr int TRIES = 3;

    explicit VirtualPc(Machine& m) : VirtualHost(m.driveModels(), cfg::PIN_DRVSB, cfg::PIN_MOTEB), m_(m) {
        m_.attach(*this);
    }

    // Drive A (0) or B (1) for what follows, as the BIOS changes drives:
    // the last one's motor keeps running, and the new one's starts (500 ms)
    // unless it already runs. The BIOS keeps each drive's head position and
    // data rate; a drive it hasn't used yet is recalibrated.
    void useDrive(int drive) {
        if (drive == drive_) return;
        set(select_, false);
        known_cylinder_[drive_] = cylinder;
        known_rate_[drive_] = rate_bps;
        drive_ = drive;
        select_ = drive ? cfg::PIN_DRVSA : cfg::PIN_DRVSB;
        motor_ = drive ? cfg::PIN_MOTEA : cfg::PIN_MOTEB;
        cylinder = known_cylinder_[drive];
        rate_bps = known_rate_[drive];
        motorOn();
    }
    int drive() const { return drive_; }

    // Lets time pass, with the drive's core 0 working in the background.
    void run(uint32_t us) {
        while (us) {
            uint32_t slice = us > 1000 ? 1000 : us;
            wait(slice);
            us -= slice;
            m_.tick(now);
        }
    }

    void motorOn() {
        set(select_, true);
        if (motor_on_[drive_]) return;
        set(motor_, true);
        motor_on_[drive_] = true;
        run(MOTOR_START_US);
    }
    void motorOff() {
        set(motor_, false);
        set(select_, false);
        motor_on_[drive_] = false;
        run(10000);
    }

    bool recalibrate() {
        ++recalibrations;
        stepDir(false);
        for (int i = 0; i < 79 && !trk0(); ++i) stepOnce();
        run(SETTLE_US);
        cylinder = trk0() ? 0 : -1;
        return cylinder == 0;
    }

    bool seekTo(int cyl) {
        if (cylinder < 0 && !recalibrate()) return false;
        if (cyl == cylinder) return true;
        stepDir(cyl > cylinder);
        for (int n = cyl > cylinder ? cyl - cylinder : cylinder - cyl; n > 0; --n) stepOnce();
        run(SETTLE_US);
        cylinder = cyl;
        return true;
    }

    bool diskChanged() { return cable().dskchg; }

    // The BIOS's way of clearing a disk change: step away and back.
    void clearDiskChange() {
        int back = cylinder < 0 ? 0 : cylinder;
        seekTo(back == 0 ? 1 : back - 1);
        seekTo(back);
    }

    // INT 13h, function 2: `count` sectors from (cyl, head, first), on one track.
    Status readSectors(uint8_t cyl, uint8_t head, uint8_t first, uint8_t count, uint8_t* out) {
        return access(cyl, head, first, count, out, nullptr);
    }
    // INT 13h, function 3.
    Status writeSectors(uint8_t cyl, uint8_t head, uint8_t first, uint8_t count, const uint8_t* data) {
        if (cable().wpt) return Status::WriteProtected;
        return access(cyl, head, first, count, nullptr, data);
    }

    // The controller's data rate, as the BIOS last set it.
    uint32_t rate_bps = 500000;

    // Media detection, after a disk change: the rate at which the boot
    // sector reads, trying 500 then 250 kbit/s. False if neither does.
    bool detectRate() {
        for (uint32_t rate : {500000u, 250000u}) {
            rate_bps = rate;
            if (seekTo(0)) {
                set(cfg::PIN_SIDE1, false);
                std::vector<uint8_t> boot(m_.format().sectorBytes());
                if (find(0, 0, 1, boot.data())) return true;
            }
        }
        return false;
    }

    unsigned retries = 0;
    unsigned recalibrations = 0;

private:
    void stepDir(bool in) {
        set(cfg::PIN_DIR, in);
        wait(10);
    }
    void stepOnce() {
        set(cfg::PIN_STEP, true);
        wait(STEP_PULSE_US);
        set(cfg::PIN_STEP, false);
        run(STEP_US - STEP_PULSE_US);
    }

    Status access(uint8_t cyl, uint8_t head, uint8_t first, uint8_t count, uint8_t* out, const uint8_t* data) {
        uint32_t size = m_.format().sectorBytes();
        for (int attempt = 0; attempt < TRIES; ++attempt) {
            if (attempt > 0) {
                ++retries;
                recalibrate();
            }
            if (!seekTo(cyl)) continue;
            set(cfg::PIN_SIDE1, head == 1);
            bool all = true;
            for (uint8_t i = 0; i < count && all; ++i) {
                if (data)
                    all = writeOne(cyl, head, uint8_t(first + i), data + i * size);
                else
                    all = readOne(cyl, head, uint8_t(first + i), out + i * size);
            }
            if (all) return Status::Ok;
        }
        return Status::SectorNotFound;
    }

    // Watches RDATA from now until the ID field of (cyl, head, r) has gone
    // past, and, if `out`, its data field too. False after two index pulses.
    bool find(uint8_t cyl, uint8_t head, uint8_t r, uint8_t* out) {
        std::vector<uint8_t> store(1024);
        mfm::Field fields[8];
        mfm::FieldScanner scanner(store.data(), store.size(), fields, 8);
        scanner.reset(uint16_t(m_.format().sectorBytes()));
        const uint32_t per_byte = m_.usPerByte();
        unsigned indexes = 0;
        uint32_t last_pos = position();
        bool id_seen = false;
        // Two index pulses is 2 revolutions; with no index (motor off) the
        // controller would wait forever, so stop after 3 revolutions' time.
        for (uint32_t bytes = 0; bytes < 3u * m_.format().track_bytes; ++bytes) {
            uint32_t pos = position();
            if (pos < last_pos && ++indexes == 2) return false;
            last_pos = pos;
            // At the wrong rate the controller's data separator never
            // locks: it sees nothing it can use.
            uint16_t cells = rate_bps == m_.format().dataRate() ? m_.rdataCells(now, head, pos) : 0;
            for (int b = 15; b >= 0; --b) scanner.addCell((cells >> b) & 1);
            run(per_byte);
            if (!scanner.count()) continue;
            const mfm::Field& f = scanner.field(scanner.count() - 1);
            if (f.mark == mfm::MARK_IDAM) {
                id_seen = f.crc_ok && f.id[0] == cyl && f.id[1] == head && f.id[2] == r;
                if (id_seen && !out) return true;
            } else if (id_seen && f.crc_ok) {
                std::memcpy(out, &store[f.data_offset], f.data_len);
                return true;
            } else {
                id_seen = false;
            }
            scanner.reset(uint16_t(m_.format().sectorBytes()));  // one field at a time
        }
        return false;
    }

    bool readOne(uint8_t cyl, uint8_t head, uint8_t r, uint8_t* out) { return find(cyl, head, r, out); }

    bool writeOne(uint8_t cyl, uint8_t head, uint8_t r, const uint8_t* data) {
        if (!find(cyl, head, r, nullptr)) return false;
        // The ID field's CRC has just gone past: skip gap 2, then write.
        run(m_.format().gap2 * m_.usPerByte());
        uint32_t size = m_.format().sectorBytes();
        std::vector<uint16_t> cells(ibm::PRESYNC_BYTES + 4 + size + 2 + 1);
        mfm::Encoder e(cells.data(), cells.size());
        e.fill(0x00, ibm::PRESYNC_BYTES);
        for (int i = 0; i < 3; ++i) e.sync(mfm::SYNC_A1);
        e.byte(mfm::MARK_DAM);
        e.bytes(data, size);
        uint16_t crc = crc16::INITIAL;
        for (int i = 0; i < 3; ++i) crc = crc16::update(crc, 0xA1);
        crc = crc16::update(crc, mfm::MARK_DAM);
        crc = crc16::compute(data, size, crc);
        e.byte(uint8_t(crc >> 8));
        e.byte(uint8_t(crc));
        e.byte(0x4E);

        set(cfg::PIN_WGATE, true);
        // WDATA: one interval per 1 cell, in capture ticks.
        uint32_t since = 0;
        for (uint16_t w : cells)
            for (int b = 15; b >= 0; --b) {
                since += Machine::TICKS_PER_CELL;
                if ((w >> b) & 1) {
                    m_.captureInterval(since);
                    since = 0;
                }
            }
        run(uint32_t(cells.size()) * m_.usPerByte());
        set(cfg::PIN_WGATE, false);
        return true;
    }

    // Where the disk is under the head, in bytes from the index.
    uint32_t position() {
        return uint32_t(uint64_t(selectedDrive().spindle().phaseUs(now)) * m_.format().dataRate() / 8000000u);
    }

    Machine& m_;
    int drive_ = 0;
    int known_cylinder_[2] = {-1, -1};
    uint32_t known_rate_[2] = {500000, 500000};
    bool motor_on_[2] = {false, false};
};
