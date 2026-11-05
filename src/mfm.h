// MFM encoding: how a floppy stores bits as flux transitions. Pure C++,
// host-tested.
//
// Each data bit becomes two cells: a clock cell, then the data cell. The
// data cell is 1 for a 1. The clock cell is 1 only between two 0 data bits,
// so 1s (flux transitions) are never adjacent and never more than three
// cells apart. At 500 kbit/s (HD) a cell is 1 us.
//
// A byte becomes a 16-bit word of cells, first cell in the top bit, which is
// the order RDATA sends them. Encoding needs the previous data bit, for the
// first clock cell.
//
// Sync marks break the clock rule on purpose, so a controller can find them
// in a stream of data: A1 with a missing clock (0x4489) before an ID or data
// field, C2 with a missing clock (0x5224) before the index mark.
#pragma once

#include <cstddef>
#include <cstdint>

#include "ram_func.h"

namespace mfm {

constexpr uint16_t SYNC_A1 = 0x4489;
constexpr uint16_t SYNC_C2 = 0x5224;

// The cells for one byte, given the data bit sent just before it.
constexpr uint16_t encodeByte(uint8_t byte, bool previous_bit) {
    uint16_t cells = 0;
    for (int i = 7; i >= 0; --i) {
        bool bit = (byte >> i) & 1;
        bool clock = !previous_bit && !bit;
        cells = uint16_t((cells << 2) | (clock << 1) | bit);
        previous_bit = bit;
    }
    return cells;
}

// encodeByte for every byte, after a 0 bit. The previous bit only decides
// the first clock cell (the top bit), which a 1 before it clears. A lookup
// for the encoder's inner loop: the loop above takes 8 steps a byte, and a
// track is 12,500 bytes.
//
// Not const, so it lives in RAM (.data) on the board rather than in flash:
// reading flash goes through the XIP cache that core 1's code runs from.
// The constexpr constructor fills it at compile time; nothing writes it.
struct EncodeTable {
    uint16_t cells[256];
    constexpr EncodeTable() : cells() {
        for (unsigned b = 0; b < 256; ++b) cells[b] = encodeByte(uint8_t(b), false);
    }
};
inline EncodeTable ENCODE_TABLE;

ALWAYS_INLINE uint16_t encodeByteFast(uint8_t byte, bool previous_bit) {
    uint16_t cells = ENCODE_TABLE.cells[byte];
    return previous_bit ? uint16_t(cells & 0x7FFF) : cells;
}

// The data bits of a word of cells (its odd cells). Sync words decode to
// their byte: 0x4489 -> A1, 0x5224 -> C2.
constexpr uint8_t decodeWord(uint16_t cells) {
    uint8_t byte = 0;
    for (int i = 7; i >= 0; --i) byte = uint8_t((byte << 1) | ((cells >> (2 * i)) & 1));
    return byte;
}

// The same without a loop, for the write decoder's every byte: gathers the
// odd cells pairwise into the low byte.
ALWAYS_INLINE uint8_t decodeWordFast(uint16_t cells) {
    uint32_t x = cells & 0x5555u;
    x = (x | x >> 1) & 0x3333u;
    x = (x | x >> 2) & 0x0F0Fu;
    x = (x | x >> 4) & 0x00FFu;
    return uint8_t(x);
}

// Appends encoded bytes to a buffer of cell words, keeping track of the last
// data bit. Writes past `capacity` are dropped and counted as an overflow.
class Encoder {
public:
    Encoder(uint16_t* out, size_t capacity) : out_(out), capacity_(capacity) {}

    void byte(uint8_t b) { put(encodeByteFast(b, last_bit_)); }
    void fill(uint8_t b, size_t count) {
        for (size_t i = 0; i < count; ++i) byte(b);
    }
    void bytes(const uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; ++i) byte(p[i]);
    }
    // A sync word, sent as is.
    void sync(uint16_t cells) { put(cells); }

    size_t size() const { return size_; }
    bool overflowed() const { return overflowed_; }

private:
    void put(uint16_t cells) {
        last_bit_ = cells & 1;
        if (size_ < capacity_)
            out_[size_++] = cells;
        else
            overflowed_ = true;
    }

    uint16_t* out_;
    size_t capacity_;
    size_t size_ = 0;
    bool last_bit_ = false;
    bool overflowed_ = false;
};

}  // namespace mfm
