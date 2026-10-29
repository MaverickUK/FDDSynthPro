#include "hfe.h"

#include <cstring>

namespace hfe {

namespace {

uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

// HFE keeps each byte's cells least significant first; the drive, most.
uint8_t reverse8(uint8_t b) {
    b = uint8_t((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = uint8_t((b & 0xCC) >> 2 | (b & 0x33) << 2);
    return uint8_t((b & 0xAA) >> 1 | (b & 0x55) << 1);
}

// Cells, MSB first, into a buffer of `cap` cells; or only counted (no buffer).
class Bits {
public:
    Bits(uint8_t* out, uint32_t cap) : out_(out), cap_(cap) {}
    void put(uint8_t cells, unsigned n) {  // the top n of `cells`
        for (unsigned i = 0; i < n; ++i, ++len_) {
            if (!out_ || len_ >= cap_) continue;
            uint8_t& b = out_[len_ / 8];
            uint8_t mask = uint8_t(0x80 >> (len_ % 8));
            if ((cells << i) & 0x80)
                b |= mask;
            else
                b &= uint8_t(~mask);
        }
    }
    uint32_t size() const { return len_; }

private:
    uint8_t* out_;
    uint32_t cap_;
    uint32_t len_ = 0;
};

bool bit(const uint8_t* b, uint32_t i) { return (b[i / 8] >> (7 - i % 8)) & 1; }
void setBit(uint8_t* b, uint32_t i, bool v) {
    uint8_t mask = uint8_t(0x80 >> (i % 8));
    if (v)
        b[i / 8] |= mask;
    else
        b[i / 8] &= uint8_t(~mask);
}

// Reverses cells [from, to).
void reverseBits(uint8_t* b, uint32_t from, uint32_t to) {
    while (from + 1 < to) {
        --to;
        bool x = bit(b, from), y = bit(b, to);
        setBit(b, from, y);
        setBit(b, to, x);
        ++from;
    }
}

}  // namespace

const char* parseHeader(const uint8_t* b, size_t len, Header& out) {
    if (len < 22) return "too short for an HFE";
    if (std::memcmp(b, "HXCPICFE", 8) == 0)
        out.version = 1;
    else if (std::memcmp(b, "HXCHFEV3", 8) == 0)
        out.version = 3;
    else
        return "not an HFE file";
    if (b[8] > 1) return "an HFE format revision this drive doesn't know";
    out.cyls = b[9];
    out.heads = b[10];
    out.encoding = b[11];
    out.bitrate = le16(b + 12);
    out.rpm = le16(b + 14);
    out.table_block = le16(b + 18);
    out.write_allowed = b[20] != 0;
    out.double_step = b[21] == 0;
    if (!out.cyls) return "no cylinders";
    if (out.heads < 1 || out.heads > 2) return "not one or two sides";
    if (out.bitrate < 50 || out.bitrate > 1000) return "a bit rate the drive can't stream";
    return nullptr;
}

// One side's bytes of a track, as stored (after the bit reversal), through
// `each(byte)`, a block at a time.
template <typename Each>
static bool sideBytes(File& f, uint16_t block, uint16_t len, uint8_t head, Each each) {
    uint32_t todo = len / 2;
    uint8_t chunk[256];
    for (uint32_t b = 0; todo; ++b) {
        uint32_t n = todo > 256 ? 256 : todo;
        if (!f.read((uint32_t(block) + b) * BLOCK + head * 256u, chunk, n)) return false;
        for (uint32_t i = 0; i < n; ++i) each(reverse8(chunk[i]));
        todo -= n;
    }
    return true;
}

// v3: a track's bytes as cells, opcodes followed. `index` is where F1 came
// (or 0); `rates` counts F2s far from `nominal`.
class Expander {
public:
    Expander(Bits& out, uint32_t nominal) : out_(out), nominal_(nominal) {}
    void byte(uint8_t x) {
        switch (state_) {
            case State::Rate:
                // A change of over 4% is a real one (Greaseweazle nudges by 1
                // tick to keep time, 3% at HD).
                if ((x > nominal_ ? x - nominal_ : nominal_ - x) * 25 > nominal_) variable = true;
                state_ = State::Byte;
                return;
            case State::SkipCount:
                skip_ = x & 7;
                state_ = State::SkipByte;
                return;
            case State::SkipByte:
                // The last 8 - n cells of x; a random byte is weak.
                if (x == OP_RAND) x = 0;
                out_.put(uint8_t(x << skip_), 8 - skip_);
                state_ = State::Byte;
                return;
            case State::Byte:
                break;
        }
        if (x == OP_NOP) return;
        if (x == OP_INDEX) {
            if (!index_seen) index = out_.size(), index_seen = true;
            return;
        }
        if (x == OP_BITRATE) {
            state_ = State::Rate;
            return;
        }
        if (x == OP_SKIPBITS) {
            state_ = State::SkipCount;
            return;
        }
        if (x == OP_RAND) x = 0;  // weak: no flux, served as noise
        out_.put(x, 8);
    }

    uint32_t index = 0;
    bool index_seen = false;
    bool variable = false;

private:
    enum class State : uint8_t { Byte, Rate, SkipCount, SkipByte };
    Bits& out_;
    uint32_t nominal_;
    State state_ = State::Byte;
    unsigned skip_ = 0;
};

const char* Disk::open(File& file, bool writable) {
    file_ = &file;
    uint8_t block0[BLOCK];
    if (!file.read(0, block0, sizeof block0)) return "can't read its header";
    if (const char* why = parseHeader(block0, sizeof block0, header_)) return why;
    if (header_.cyls > ibm::MAX_CYLINDERS) return "too many cylinders";
    uint8_t table[ibm::MAX_CYLINDERS * 4];
    if (!file.read(uint32_t(header_.table_block) * BLOCK, table, header_.cyls * 4u)) return "can't read its track table";
    uint32_t longest = 0;  // cells
    for (unsigned c = 0; c < header_.cyls; ++c) {
        track_block_[c] = le16(table + 4 * c);
        track_len_[c] = le16(table + 4 * c + 2);
        if (track_len_[c] / 2 * 8u > longest) longest = track_len_[c] / 2 * 8u;
    }
    // v3: the opcodes make a track shorter than its bytes. Measure each.
    variable_rate_tracks_ = 0;
    if (header_.version == 3) {
        longest = 0;
        uint32_t nominal = V3_TICKS_HZ / (2000u * header_.bitrate);
        for (uint8_t c = 0; c < header_.cyls; ++c)
            for (uint8_t h = 0; h < header_.heads; ++h) {
                Bits count(nullptr, 0);
                Expander ex(count, nominal);
                if (!sideBytes(file, track_block_[c], track_len_[c], h, [&](uint8_t b) { ex.byte(b); }))
                    return "can't read a track";
                if (count.size() > longest) longest = count.size();
                if (ex.variable) ++variable_rate_tracks_;
            }
    }
    uint32_t words = (longest + 15) / 16;
    if (!words) return "empty tracks";
    if (words > ibm::MAX_TRACK_WORDS) return "tracks longer than the drive holds (an ED disk?)";

    fmt_ = ibm::Format{};
    fmt_.encoding = ibm::ENCODING_RAW;
    fmt_.cyls = header_.cyls;
    fmt_.heads = header_.heads;
    fmt_.step = header_.double_step ? 2 : 1;
    fmt_.rate = uint32_t(header_.bitrate) * 1000;
    fmt_.track_bytes = uint16_t(words);
    // A revolution is a track's length at the bit rate, whatever the rpm says.
    fmt_.stretched_period_us = uint32_t(uint64_t(words) * 8000000u / fmt_.rate);
    fmt_.rpm = uint16_t((60000000u + fmt_.stretched_period_us / 2) / fmt_.stretched_period_us);
    writable_ = writable && header_.write_allowed && header_.version == 1;
    return nullptr;
}

bool Disk::readTrack(uint8_t cylinder, uint8_t head, uint8_t* out) {
    if (!file_ || cylinder >= header_.cyls || head >= header_.heads) return false;
    const uint32_t cells = uint32_t(fmt_.track_bytes) * 16;
    Bits bits(out, cells);
    uint32_t index = 0;
    bool ok;
    if (header_.version == 1) {
        ok = sideBytes(*file_, track_block_[cylinder], track_len_[cylinder], head,
                       [&](uint8_t b) { bits.put(b, 8); });
    } else {
        Expander ex(bits, V3_TICKS_HZ / (2000u * header_.bitrate));
        ok = sideBytes(*file_, track_block_[cylinder], track_len_[cylinder], head, [&](uint8_t b) { ex.byte(b); });
        index = ex.index;
    }
    uint32_t len = bits.size() < cells ? bits.size() : cells;
    if (!ok || !len) return false;
    // The index first.
    if (index && index < len) {
        reverseBits(out, 0, index);
        reverseBits(out, index, len);
        reverseBits(out, 0, len);
    }
    // A shorter track carries on from its start: the disk loops.
    for (uint32_t i = len; i < cells; ++i) setBit(out, i, bit(out, i - len));
    return true;
}

bool Disk::writeTrackBytes(uint8_t cylinder, uint8_t head, uint32_t at, const uint8_t* data, uint32_t n) {
    if (!writable_ || cylinder >= header_.cyls || head >= header_.heads) return false;
    uint32_t side_len = track_len_[cylinder] / 2;
    if (!side_len) return false;
    // Byte k of the track is byte k of the side's data (v1: no opcodes), round
    // the track's own length where the drive's is longer.
    while (n) {
        uint32_t k = at % side_len;
        uint32_t run = 256 - k % 256;  // to the end of this block's half
        if (run > n) run = n;
        if (run > side_len - k) run = side_len - k;
        uint8_t chunk[256];
        for (uint32_t i = 0; i < run; ++i) chunk[i] = reverse8(data[i]);
        uint32_t offset = (uint32_t(track_block_[cylinder]) + k / 256) * BLOCK + head * 256u + k % 256;
        if (!file_->write(offset, chunk, run)) return false;
        at += run;
        data += run;
        n -= run;
    }
    return file_->flush();
}

}  // namespace hfe
