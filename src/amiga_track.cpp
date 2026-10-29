#include "amiga_track.h"

#include <cstring>

#include "mfm.h"

namespace amiga {

namespace {

// Bits 7, 5, 3, 1 of a byte (its odd bits), packed into 4; and 6, 4, 2, 0.
uint8_t odd4(uint8_t x) { return uint8_t(((x >> 4) & 8) | ((x >> 3) & 4) | ((x >> 2) & 2) | ((x >> 1) & 1)); }
uint8_t even4(uint8_t x) { return odd4(uint8_t(x << 1)); }

// The other way: odd bits `o` and even bits `e` back into a byte.
uint8_t join4(uint8_t o, uint8_t e) {
    uint8_t x = 0;
    for (int i = 3; i >= 0; --i) x = uint8_t((x << 2) | (((o >> i) & 1) << 1) | ((e >> i) & 1));
    return x;
}

// A field of `n` bytes (n even), as the Amiga sends it: its odd bits, then its even bits.
void encodeField(mfm::Encoder& e, const uint8_t* field, size_t n) {
    for (size_t k = 0; k < n; k += 2) e.byte(uint8_t(odd4(field[k]) << 4 | odd4(field[k + 1])));
    for (size_t k = 0; k < n; k += 2) e.byte(uint8_t(even4(field[k]) << 4 | even4(field[k + 1])));
}

// ...and back: `split` is the field's `n` data bytes as received.
void decodeField(const uint8_t* split, size_t n, uint8_t* out) {
    for (size_t k = 0; k < n / 2; ++k) {
        uint8_t o = split[k], e = split[n / 2 + k];
        out[2 * k] = join4(uint8_t(o >> 4), uint8_t(e >> 4));
        out[2 * k + 1] = join4(uint8_t(o & 15), uint8_t(e & 15));
    }
}

void putLong(uint8_t* out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out[i] = uint8_t(v >> (24 - 8 * i));
}

uint32_t getLong(const uint8_t* in) {
    return uint32_t(in[0]) << 24 | uint32_t(in[1]) << 16 | uint32_t(in[2]) << 8 | in[3];
}

}  // namespace

uint32_t checksum(const uint8_t* data, size_t n) {
    uint32_t c = 0;
    for (size_t i = 0; i + 4 <= n; i += 4) c ^= getLong(data + i);
    return (c ^ (c >> 1)) & 0x55555555u;
}

bool buildTrack(uint8_t cylinder, uint8_t head, unsigned sectors, const uint8_t* data, uint16_t* out, uint32_t words) {
    if (usedWords(sectors) > words) return false;
    mfm::Encoder e(out, words);
    e.fill(0x00, GAP_WORDS);
    for (unsigned s = 0; s < sectors; ++s) {
        // The info and label, checksummed together.
        uint8_t header[4 + 16] = {0xFF, uint8_t(cylinder * 2 + head), uint8_t(s), uint8_t(sectors - s)};
        const uint8_t* bytes = data + s * SECTOR_BYTES;
        uint8_t sums[8];
        putLong(sums, checksum(header, sizeof header));
        putLong(sums + 4, checksum(bytes, SECTOR_BYTES));
        e.sync(mfm::SYNC_A1);
        e.sync(mfm::SYNC_A1);
        encodeField(e, header, 4);
        encodeField(e, header + 4, 16);
        encodeField(e, sums, 4);
        encodeField(e, sums + 4, 4);
        encodeField(e, bytes, SECTOR_BYTES);
        e.fill(0x00, 2);
    }
    e.fill(0x00, words - e.size());
    return !e.overflowed() && e.size() == words;
}

// ---- Reading what the Amiga writes ----------------------------------------------------

void SectorScanner::reset() {
    shift_ = 0;
    in_sector_ = false;
    count_ = 0;
    used_ = 0;
}

void SectorScanner::addCell(bool cell) {
    shift_ = shift_ << 1 | cell;
    if (shift_ == (uint32_t(mfm::SYNC_A1) << 16 | mfm::SYNC_A1)) {
        // Two syncs: a sector starts (even if the last was cut short).
        in_sector_ = true;
        cells_ = words_ = 0;
        word_ = 0;
        return;
    }
    if (!in_sector_) return;
    word_ = uint16_t(word_ << 1 | cell);
    if (++cells_ < 16) return;
    cells_ = 0;
    raw_[words_++] = mfm::decodeWord(word_);
    if (words_ == sizeof raw_) {
        in_sector_ = false;
        finishSector();
    }
}

void SectorScanner::finishSector() {
    if (count_ == MAX_SECTORS || used_ + SECTOR_BYTES > store_len_) return;
    uint8_t header[4 + 16], sums[8];
    decodeField(raw_, 4, header);
    decodeField(raw_ + 4, 16, header + 4);
    decodeField(raw_ + 20, 4, sums);
    decodeField(raw_ + 24, 4, sums + 4);
    uint8_t* data = store_ + used_;
    decodeField(raw_ + 28, SECTOR_BYTES, data);
    Sector& s = sectors_[count_++];
    s.track = header[1];
    s.sector = header[2];
    s.ok = header[0] == 0xFF && getLong(sums) == checksum(header, sizeof header) &&
           getLong(sums + 4) == checksum(data, SECTOR_BYTES);
    s.data_offset = uint32_t(used_);
    used_ += SECTOR_BYTES;
}

}  // namespace amiga
