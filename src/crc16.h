// CRC-16-CCITT as IBM floppy formats use it: polynomial 0x1021, initial
// value 0xFFFF, no reflection, no final XOR ("CRC-16/CCITT-FALSE"). It covers
// the three A1 sync bytes, the address mark and the field, and is stored
// high byte first. Pure C++, host-tested.
#pragma once

#include <cstddef>
#include <cstdint>

namespace crc16 {

constexpr uint16_t INITIAL = 0xFFFF;

constexpr uint16_t update(uint16_t crc, uint8_t byte) {
    crc ^= uint16_t(byte) << 8;
    for (int i = 0; i < 8; ++i) crc = (crc & 0x8000) ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
    return crc;
}

inline uint16_t compute(const uint8_t* p, size_t n, uint16_t crc = INITIAL) {
    for (size_t i = 0; i < n; ++i) crc = update(crc, p[i]);
    return crc;
}

}  // namespace crc16
