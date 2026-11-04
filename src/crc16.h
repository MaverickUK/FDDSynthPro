// CRC-16-CCITT as IBM floppy formats use it: polynomial 0x1021, initial
// value 0xFFFF, no reflection, no final XOR ("CRC-16/CCITT-FALSE"). It covers
// the three A1 sync bytes, the address mark and the field, and is stored
// high byte first. Pure C++, host-tested.
#pragma once

#include <cstddef>
#include <cstdint>

#include "ram_func.h"

namespace crc16 {

constexpr uint16_t INITIAL = 0xFFFF;

constexpr uint16_t update(uint16_t crc, uint8_t byte) {
    crc ^= uint16_t(byte) << 8;
    for (int i = 0; i < 8; ++i) crc = (crc & 0x8000) ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
    return crc;
}

// update() for every byte from a CRC of 0, a byte at a time instead of a bit
// at a time. Not const, so it lives in RAM on the board, not in flash (the
// XIP cache is core 1's: see mfm::EncodeTable); filled at compile time.
struct Table {
    uint16_t v[256];
    constexpr Table() : v() {
        for (unsigned i = 0; i < 256; ++i) v[i] = update(0, uint8_t(i));
    }
};
inline Table TABLE;

ALWAYS_INLINE uint16_t updateFast(uint16_t crc, uint8_t byte) {
    return uint16_t((crc << 8) ^ TABLE.v[uint8_t((crc >> 8) ^ byte)]);
}

inline uint16_t compute(const uint8_t* p, size_t n, uint16_t crc = INITIAL) {
    for (size_t i = 0; i < n; ++i) crc = updateFast(crc, p[i]);
    return crc;
}

}  // namespace crc16
