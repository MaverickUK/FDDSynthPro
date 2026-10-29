// AmigaDOS tracks: what an Amiga's trackdisk.device reads and writes, and
// how ADF images hold them. Pure C++, host-tested against Greaseweazle's
// amiga.amigados codec (host/tests/test_track_encoder.py), cell for cell.
//
// The Amiga reads and writes a whole track at a time, with no index mark
// and no gaps between sectors. One track, from the index (Greaseweazle's
// layout):
//   128 x 00
//   then 11 sectors, in order (DD), each 544 bytes:
//     00 00            (the sync words below replace them: 4489 4489)
//     info             FF, track (cylinder x 2 + head), sector, sectors to the gap
//     label            16 x 00
//     header checksum  of info and label
//     data checksum    of the data
//     data             512 bytes
//     00 00
//   00 to the end of the track
// Each field's bytes are sent odd bits first, then even bits: a field of n
// bytes is n bytes of its odd bits (bit 7, 5, 3, 1 of each byte, packed)
// then n of its even bits, all MFM-encoded as usual. The sync words are
// MFM's A1 with a missing clock (0x4489), as on a PC, but there are two
// and no mark byte.
#pragma once

#include <cstddef>
#include <cstdint>

namespace amiga {

constexpr unsigned SECTORS = 11;  // DD; HD's 22 need a 150 RPM drive
constexpr unsigned SECTOR_BYTES = 512;
constexpr unsigned SECTOR_WORDS = 544;  // a sector's cell words, syncs included
constexpr unsigned GAP_WORDS = 128;     // before the first sector
constexpr uint32_t ADF_BYTES = 80 * 2 * SECTORS * SECTOR_BYTES;  // 901,120

// The bytes a track's sectors take before the gap to the index.
constexpr uint32_t usedWords(unsigned sectors) { return GAP_WORDS + sectors * SECTOR_WORDS; }

// AmigaDOS's checksum of `n` bytes (a multiple of 4): the XOR of their
// big-endian longs, folded onto the data bits.
uint32_t checksum(const uint8_t* data, size_t n);

// Builds the track for (cylinder, head) into `out` (`words` cell words).
// `data` holds its `sectors` sectors in order. False if they don't fit.
bool buildTrack(uint8_t cylinder, uint8_t head, unsigned sectors, const uint8_t* data, uint16_t* out, uint32_t words);

// Finds sectors in a stream of cells, as the Amiga writes them: two sync
// words, then the sector. Each good one's data goes into `store`.
class SectorScanner {
public:
    struct Sector {
        uint8_t track;   // cylinder x 2 + head
        uint8_t sector;  // from 0
        bool ok;         // format byte and both checksums good
        uint32_t data_offset;
    };
    static constexpr size_t MAX_SECTORS = 24;

    SectorScanner(uint8_t* store, size_t store_len) : store_(store), store_len_(store_len) {}

    void reset();
    void addCell(bool cell);

    size_t count() const { return count_; }
    const Sector& sector(size_t i) const { return sectors_[i]; }

private:
    void finishSector();

    uint8_t* store_;
    size_t store_len_;
    uint32_t shift_ = 0;     // the last 32 cells
    bool in_sector_ = false;
    unsigned cells_ = 0;     // cells into the current word
    unsigned words_ = 0;     // words of the sector collected (after the syncs)
    uint16_t word_ = 0;
    uint8_t raw_[SECTOR_WORDS - 4];  // the sector's bytes, still split odd/even
    Sector sectors_[MAX_SECTORS];
    size_t count_ = 0;
    size_t used_ = 0;  // bytes of `store` taken
};

}  // namespace amiga
