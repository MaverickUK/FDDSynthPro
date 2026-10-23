#include "rdata_stream.h"

namespace rdata {

// Spreads the 8 bits of a byte to every other bit of 16: abcdefgh -> 0a0b0c0d0e0f0g0h.
static uint16_t spread8(uint8_t b) {
    uint16_t x = b;
    x = (x | (x << 4)) & 0x0F0F;
    x = (x | (x << 2)) & 0x3333;
    x = (x | (x << 1)) & 0x5555;
    return x;
}

static uint32_t spread16(uint16_t w) { return (uint32_t(spread8(uint8_t(w >> 8))) << 16) | spread8(uint8_t(w)); }

uint32_t interleave(uint16_t head0_cells, uint16_t head1_cells) {
    return (spread16(head0_cells) << 1) | spread16(head1_cells);
}

uint16_t headCells(uint32_t slots, uint8_t head) {
    if (head == 0) slots >>= 1;
    uint16_t cells = 0;
    for (int i = 15; i >= 0; --i) cells = uint16_t((cells << 1) | ((slots >> (2 * i)) & 1));
    return cells;
}

void setHead(uint8_t head, const uint16_t* cells, uint32_t words, uint32_t* out) {
    const uint32_t keep = head == 0 ? 0x55555555u : 0xAAAAAAAAu;  // the other head's bits
    const unsigned shift = head == 0 ? 1 : 0;
    for (uint32_t i = 0; i < words; ++i) out[i] = (out[i] & keep) | (spread16(cells[i]) << shift);
}

bool buildCylinder(const ibm::Format& fmt, uint8_t cylinder, const uint8_t* head0_data, const uint8_t* head1_data,
                   uint16_t* scratch, uint32_t* out) {
    if (!ibm::buildTrack(fmt, cylinder, 0, head0_data, scratch)) return false;
    setHead(0, scratch, fmt.track_bytes, out);
    if (!ibm::buildTrack(fmt, cylinder, 1, head1_data, scratch)) return false;
    setHead(1, scratch, fmt.track_bytes, out);
    return true;
}

Divider clockDivider(uint32_t sys_hz, uint32_t data_rate_bps, uint32_t cycles_per_cell) {
    uint64_t cycles_per_s = uint64_t(data_rate_bps) * 2 * cycles_per_cell;
    uint64_t div256 = (uint64_t(sys_hz) * 256 + cycles_per_s / 2) / cycles_per_s;  // rounded
    return Divider{uint16_t(div256 >> 8), uint8_t(div256), div256 * cycles_per_s == uint64_t(sys_hz) * 256};
}

}  // namespace rdata
