// HFE flux images: each track's cells as recorded, rather than its sectors,
// so a disk keeps whatever a sector image can't (copy protection's bad
// CRCs, duplicate sector IDs, long tracks, weak bits, and formats this drive
// has no encoder for). The HxC emulator's format, and FlashFloppy's;
// Greaseweazle writes it (`gw read disk.hfe`). Pure C++, host-tested against
// Greaseweazle's own HFE files.
//
// The file (all little-endian):
//   block 0    the header: "HXCPICFE" (v1) or "HXCHFEV3" (v3), the format
//              revision, cylinders, sides, encoding, bit rate (kbit/s), rpm,
//              interface, the track table's block, write allowed (FF),
//              single step (FF; 00: double step)
//   the track table: for each cylinder, its data's first block and its
//              length in bytes (both sides together)
//   track data: blocks of 512 bytes, 256 of side 0 then 256 of side 1,
//              each byte's cells least significant first
// Cells come at twice the bit rate (an MFM data bit is two cells). FM is
// recorded at double rate, each of its cells doubled: the drive streams it
// as it is.
//
// v3 adds opcodes in the track data (after the bit reversal): F0 nothing,
// F1 the index is here, F2 n the cell time is now n ticks of 36 MHz, F3 n b
// only the last 8 - n cells of b, F4 a byte of random cells (weak bits).
// The drive streams at one rate, so cell times are kept as an average:
// tracks whose rate changes (Copylock and the like) are counted as such and
// served at the disk's rate. Weak bits are served as random flux, new on
// every revolution (CylinderLoader).
//
// As the drive holds it, a track is its cells, MSB first, two bytes a cell
// word (the streamer's order), every track the same number of words: the
// longest's. A shorter track carries on from its start, as the disk loops.
// A run of 8 empty cells, which no recording has, marks weak bits: no flux
// reads as noise on a real drive too.
//
// Writes splice the host's cells into the track (v1 only: a v3 track's
// bytes don't map evenly to its cells), and go back into the file at once.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "track_source.h"

namespace hfe {

constexpr uint32_t BLOCK = 512;
constexpr uint8_t OP_NOP = 0xF0, OP_INDEX = 0xF1, OP_BITRATE = 0xF2, OP_SKIPBITS = 0xF3, OP_RAND = 0xF4;
constexpr uint32_t V3_TICKS_HZ = 36000000;  // F2's unit

// The bytes of a file, wherever they live (the SD card, PSRAM, a test's
// memory).
class File {
public:
    virtual ~File() = default;
    virtual bool read(uint32_t at, uint8_t* out, uint32_t n) = 0;
    virtual bool write(uint32_t at, const uint8_t* data, uint32_t n) {
        (void)at, (void)data, (void)n;
        return false;
    }
    // After a run of writes: on the card, so it holds them even if the power goes.
    virtual bool flush() { return true; }
};

// A file held in memory (tests and tools).
class MemoryFile : public File {
public:
    MemoryFile(uint8_t* bytes, uint32_t size) : bytes_(bytes), size_(size) {}
    bool read(uint32_t at, uint8_t* out, uint32_t n) override {
        if (at > size_ || n > size_ - at) return false;
        std::memcpy(out, bytes_ + at, n);
        return true;
    }
    bool write(uint32_t at, const uint8_t* data, uint32_t n) override {
        if (at > size_ || n > size_ - at) return false;
        std::memcpy(bytes_ + at, data, n);
        return true;
    }

private:
    uint8_t* bytes_;
    uint32_t size_;
};

struct Header {
    uint8_t version = 1;  // 1 or 3
    uint8_t cyls = 0;
    uint8_t heads = 0;
    uint8_t encoding = 0xFF;  // HxC's: 00 IBM MFM, 01 Amiga MFM, 02 IBM FM...
    uint16_t bitrate = 0;     // kbit/s
    uint16_t rpm = 0;         // often 0
    bool write_allowed = false;
    bool double_step = false;
    uint16_t table_block = 1;
};

// Reads the header from the first bytes of a file. Returns why it isn't an
// HFE the drive can serve, or nullptr.
const char* parseHeader(const uint8_t* block0, size_t len, Header& out);

class Disk : public TrackSource {
public:
    // Reads the header and track table; for v3, measures each track (its
    // opcodes make it shorter than its bytes). Returns why the file can't
    // be served, or nullptr. `writable`: the file may be written (the
    // card's read-only flag); v3 never is.
    const char* open(File& file, bool writable);

    const Header& header() const { return header_; }
    const ibm::Format& format() const override { return fmt_; }
    bool writable() const override { return writable_; }
    // The track's cells (format().track_bytes words, as bytes, MSB first).
    bool readTrack(uint8_t cylinder, uint8_t head, uint8_t* out) override;
    bool writeTrackBytes(uint8_t cylinder, uint8_t head, uint32_t at, const uint8_t* data, uint32_t n) override;

    // Tracks whose cell time changes along them (v3): served at the disk's rate.
    unsigned variableRateTracks() const { return variable_rate_tracks_; }

private:
    File* file_ = nullptr;
    Header header_;
    ibm::Format fmt_;
    bool writable_ = false;
    uint16_t track_block_[ibm::MAX_CYLINDERS] = {};
    uint16_t track_len_[ibm::MAX_CYLINDERS] = {};  // bytes, both sides
    unsigned variable_rate_tracks_ = 0;
};

}  // namespace hfe
