#include "ibm_track.h"

#include "amiga_track.h"
#include "crc16.h"
#include "mfm.h"

namespace ibm {

// ---- One track's layout --------------------------------------------------------------

// In the track's own bytes; positions multiply by its scale.
static uint32_t indexBytes(const Track& t) {
    return t.gap4a + (t.iam ? t.presyncBytes() + t.syncBytes() + t.gap1 : 0);
}
static uint32_t idFieldBytes(const Track& t) { return t.presyncBytes() + t.syncBytes() + 4 + 2 + t.gap2; }
static uint32_t dataFieldBytes(const Track& t) {
    return t.presyncBytes() + t.syncBytes() + t.sectorBytes() + 2 + t.gap3;
}

uint32_t Track::idFieldStart(uint16_t slot) const {
    return (indexBytes(*this) + slot * (idFieldBytes(*this) + dataFieldBytes(*this))) * scale;
}

uint32_t Track::dataFieldStart(uint16_t slot) const { return idFieldStart(slot) + idFieldBytes(*this) * scale; }

uint32_t Track::usedBytes() const { return idFieldStart(sectors); }

void Track::sectorMap(uint8_t cylinder, uint8_t head, uint16_t* out) const {
    // Greaseweazle's sec_map: from the skewed start, each sector `interleave`
    // slots after the last, moving on past slots already taken.
    if (!sectors) return;
    bool taken[MAX_SECTORS] = {};
    unsigned pos = (unsigned(cylinder) * cskew + unsigned(head) * hskew) % sectors;
    for (uint16_t s = 0; s < sectors; ++s) {
        while (taken[pos]) pos = (pos + 1) % sectors;
        out[pos] = s;
        taken[pos] = true;
        pos = (pos + interleave) % sectors;
    }
}

// ---- The disk ------------------------------------------------------------------------

// Calls `each(cylinder, head)` for every track in file order until it returns true.
template <typename Each>
static void fileOrder(const Format& f, Each each) {
    uint8_t sides[2] = {0, 1};
    if (f.heads == 2 && (f.file_layout & LAYOUT_SIDES_SWAPPED)) sides[0] = 1, sides[1] = 0;
    auto cylinderAt = [&](uint8_t side, unsigned i) {
        bool reverse = f.file_layout & (side ? LAYOUT_REVERSE_SIDE1 : LAYOUT_REVERSE_SIDE0);
        return uint8_t(reverse ? f.cyls - 1 - i : i);
    };
    if (f.file_layout & LAYOUT_SEQUENTIAL) {
        for (unsigned s = 0; s < f.heads; ++s)
            for (unsigned i = 0; i < f.cyls; ++i)
                if (each(cylinderAt(sides[s], i), sides[s])) return;
    } else {
        for (unsigned i = 0; i < f.cyls; ++i)
            for (unsigned s = 0; s < f.heads; ++s)
                if (each(cylinderAt(sides[s], i), sides[s])) return;
    }
}

uint32_t Format::trackOffset(uint8_t cylinder, uint8_t head) const {
    uint32_t at = 0;
    fileOrder(*this, [&](uint8_t c, uint8_t h) {
        if (c == cylinder && h == head) return true;
        at += trackDataBytes(c, h);
        return false;
    });
    return at;
}

uint32_t Format::imageBytes() const {
    uint32_t n = 0;
    fileOrder(*this, [&](uint8_t c, uint8_t h) {
        n += trackDataBytes(c, h);
        return false;
    });
    return n;
}

uint32_t Format::maxTrackDataBytes() const {
    if (encoding == ENCODING_RAW) return uint32_t(track_bytes) * 2;
    uint32_t most = 0;
    for (unsigned k = 0; k < kind_count; ++k)
        if (kinds[k].dataBytes() > most) most = kinds[k].dataBytes();
    return most;
}

bool Format::finish() {
    if (!cyls || cyls > MAX_CYLINDERS || heads < 1 || heads > 2 || step < 1 || step > 4 || !rpm || !rate ||
        kind_count < 1 || kind_count > MAX_KINDS)
        return false;
    uint32_t words = uint32_t(uint64_t(rate) * 15 / (2 * rpm));
    if (words > MAX_TRACK_WORDS) return false;
    uint32_t needed = 0;  // the longest layout
    for (unsigned k = 0; k < kind_count; ++k) {
        const Track& t = kinds[k];
        uint32_t used = encoding == ENCODING_AMIGA ? amiga::usedWords(t.sectors) : t.usedBytes();
        if (t.sectors > MAX_SECTORS || t.size_code > 6 || !t.interleave || t.dataBytes() > MAX_TRACK_DATA)
            return false;
        if (encoding == ENCODING_AMIGA && t.sectorBytes() != amiga::SECTOR_BYTES) return false;
        if (used > needed) needed = used;
    }
    track_bytes = uint16_t(words);
    stretched_period_us = 0;
    if (needed > words) {
        // Too long by a little: a longer track, with 1% of gap before the
        // index, and a revolution that takes that long.
        uint32_t most = words + words * STRETCH_PCT / 100;
        if (needed > most || needed > MAX_TRACK_WORDS) return false;
        uint32_t stretched = needed + words / 100;
        if (stretched > most) stretched = most;
        if (stretched > MAX_TRACK_WORDS) stretched = MAX_TRACK_WORDS;
        track_bytes = uint16_t(stretched);
        stretched_period_us = uint32_t(uint64_t(stretched) * 8000000u / rate);
    }
    return lastPhysicalCylinder() <= MAX_CYLINDERS;
}

// ---- Greaseweazle's rules for what's left on auto ------------------------------------

// The standard gaps (Greaseweazle's MFMGaps and FMGaps): gap 3 by sector
// size code.
struct Gaps {
    uint8_t gap1, gap2, gap4a;
    uint8_t gap3[8];
};
static const Gaps MFM_GAPS = {50, 22, 80, {32, 54, 84, 116, 255, 255, 255, 255}};
static const Gaps FM_GAPS = {26, 11, 40, {27, 42, 58, 138, 255, 255, 255, 255}};

// The track's length in cells with the given gaps, gap 3 included.
static uint32_t lengthCells(const TrackSpec& s, uint32_t gap1, uint32_t gap2, uint32_t gap3, uint32_t gap4a) {
    uint32_t presync = s.fm ? 6 : 12, sync = s.fm ? 1 : 4;
    uint32_t idx = gap4a + (s.iam ? presync + sync + gap1 : 0);
    uint32_t per_sector = (presync + sync + 4 + 2 + gap2) + (presync + sync) + (2 + gap3);
    uint32_t sizes = s.sectors > 0 ? uint32_t(s.sectors) * (128u << s.size_code) : 0;
    return (idx + per_sector * uint32_t(s.sectors) + sizes) * 16;
}

uint32_t autoRate(const TrackSpec& s, uint32_t rpm) {
    const Gaps& g = s.fm ? FM_GAPS : MFM_GAPS;
    uint32_t len = lengthCells(s, s.gap1 < 0 ? g.gap1 : s.gap1, s.gap2 < 0 ? g.gap2 : s.gap2,
                               s.gap3 < 0 ? 0 : s.gap3, s.gap4a < 0 ? g.gap4a : s.gap4a);
    // MFM: 250, 500 or 1000 kbit/s; FM: 125 or 250.
    unsigned first = s.fm ? 0 : 1, last = s.fm ? 1 : 3, i = first;
    for (; i <= last; ++i) {
        uint32_t most = (50000u * 300 / rpm) << i;
        most += most * 3 / 100;
        if (len < most) break;
    }
    if (i > last) i = last;
    return (125000u << i);
}

bool resolve(const TrackSpec& s, uint32_t rate, uint32_t rpm, Track& out) {
    if (s.sectors < 0 || s.sectors > int(MAX_SECTORS) || (s.sectors > 0 && (s.size_code < 0 || s.size_code > 6)) ||
        s.first_id < 0 || s.first_id > 255 || s.interleave < 1 || s.cskew < 0 || s.hskew < 0 || !rpm)
        return false;
    const Gaps& g = s.fm ? FM_GAPS : MFM_GAPS;
    uint32_t gap1 = s.gap1 < 0 ? g.gap1 : uint32_t(s.gap1);
    uint32_t gap2 = s.gap2 < 0 ? (!s.fm && rate >= 1000000 ? 41 : g.gap2) : uint32_t(s.gap2);
    uint32_t gap3 = s.gap3 < 0 ? 0 : uint32_t(s.gap3);
    uint32_t gap4a = s.gap4a < 0 ? g.gap4a : uint32_t(s.gap4a);
    uint32_t len = lengthCells(s, gap1, gap2, gap3, gap4a);
    uint32_t rev = uint32_t(uint64_t(rate) * 400 * 300 / 1000 / rpm);  // cells a revolution
    if (s.sectors > 0 && s.gap3 < 0) {
        uint32_t space = rev > len ? rev - len : 0;
        uint32_t fit = space / (16 * uint32_t(s.sectors));
        gap3 = fit < g.gap3[s.size_code] ? fit : g.gap3[s.size_code];
        len += 16 * uint32_t(s.sectors) * gap3;
    }
    // At least 1% of a revolution before the index, counting the last gap 3;
    // if that doesn't fit, half of gap 4a goes.
    uint32_t pre_index = rev / 100;
    if (s.sectors > 0) pre_index = pre_index > gap3 * 16 ? pre_index - gap3 * 16 : 0;
    len += pre_index;
    if (len > rev && s.gap4a < 0) gap4a /= 2;

    out = Track{};
    out.sectors = uint16_t(s.sectors);
    out.size_code = uint8_t(s.size_code < 0 ? 2 : s.size_code);
    out.first_id = uint8_t(s.first_id);
    out.head_id = int16_t(s.head_id);
    out.interleave = uint8_t(s.interleave);
    out.cskew = uint8_t(s.cskew);
    out.hskew = uint8_t(s.hskew);
    out.iam = s.iam;
    out.fm = s.fm;
    if (gap1 > 255 || gap2 > 255 || gap3 > 255 || gap4a > 255) return false;
    out.gap1 = uint8_t(gap1);
    out.gap2 = uint8_t(gap2);
    out.gap3 = uint8_t(gap3);
    out.gap4a = uint8_t(gap4a);
    return true;
}

bool uniformFormat(uint8_t cyls, uint8_t heads, const TrackSpec& spec, uint32_t rate, uint32_t rpm, Format& out) {
    out = Format{};
    out.cyls = cyls;
    out.heads = heads;
    out.rpm = uint16_t(rpm);
    out.rate = rate ? rate : autoRate(spec, rpm);
    return resolve(spec, out.rate, rpm, out.kinds[0]) && out.finish();
}

Format amigaDD() {
    Format f = fixedFormat(80, 2, pcTrack(amiga::SECTORS, 0), 250000, 300);
    f.encoding = ENCODING_AMIGA;
    return f;
}

bool formatForSize(uint32_t bytes, Format& out) {
    struct Known {
        uint8_t cyls, heads, sectors, gap3, interleave, cskew;
        uint32_t rate;
        uint16_t rpm;
    };
    // Greaseweazle's ibm.160 ... ibm.dmf.
    static const Known KNOWN[] = {
        {40, 1, 8, 84, 1, 0, 250000, 300},   {40, 1, 9, 84, 1, 0, 250000, 300},
        {40, 2, 8, 84, 1, 0, 250000, 300},   {40, 2, 9, 84, 1, 0, 250000, 300},
        {80, 2, 9, 84, 1, 0, 250000, 300},   {80, 2, 10, 30, 1, 0, 250000, 300},
        {80, 2, 15, 84, 1, 0, 500000, 360},  {80, 2, 18, 108, 1, 0, 500000, 300},
        {80, 2, 21, 12, 2, 3, 500000, 300},
    };
    for (const Known& k : KNOWN) {
        if (bytes != uint32_t(k.cyls) * k.heads * k.sectors * 512) continue;
        TrackSpec spec;
        spec.sectors = k.sectors;
        spec.size_code = 2;
        spec.gap3 = k.gap3;
        spec.interleave = k.interleave;
        spec.cskew = k.cskew;
        return uniformFormat(k.cyls, k.heads, spec, k.rate, k.rpm, out);
    }
    return false;
}

// ---- The encoder ---------------------------------------------------------------------

// A track's cells: MFM or FM bytes, each sent as one word, or (scale 2,
// a track at half the disk's rate) as two, each cell followed by an empty one.
class TrackCells {
public:
    TrackCells(uint16_t* out, uint32_t words, bool fm, uint8_t scale)
        : out_(out), words_(words), fm_(fm), scale_(scale) {}

    // A data byte: FM with every clock cell set; MFM after the last bit.
    void byte(uint8_t b) {
        put(fm_ ? fmCells(b, 0xFF) : mfm::encodeByte(b, last_bit_));
        last_bit_ = b & 1;
    }
    void fill(uint8_t b, size_t n) {
        for (size_t i = 0; i < n; ++i) byte(b);
    }
    void bytes(const uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; ++i) byte(p[i]);
    }
    // MFM: a sync word as is (A1 or C2, missing a clock).
    void sync(uint16_t cells) {
        put(cells);
        last_bit_ = cells & 1;
    }
    // FM: a mark, with its clocks (C7 or D7: missing some).
    void mark(uint8_t b, uint8_t clock) {
        put(fmCells(b, clock));
        last_bit_ = b & 1;
    }
    // Gap to the end of the track: whole bytes, then half of one if the
    // track's an odd number of words at scale 2.
    void finish(uint8_t gap) {
        while (size_ + scale_ <= words_) byte(gap);
        if (size_ < words_) push(stretch(uint8_t((fm_ ? fmCells(gap, 0xFF) : mfm::encodeByte(gap, last_bit_)) >> 8)));
    }

    uint32_t size() const { return size_; }
    bool overflowed() const { return overflowed_; }

    // An FM byte's cells: each clock bit, then its data bit.
    static uint16_t fmCells(uint8_t data, uint8_t clock) {
        uint16_t w = 0;
        for (int i = 7; i >= 0; --i) w = uint16_t(w << 2 | ((clock >> i) & 1) << 1 | ((data >> i) & 1));
        return w;
    }

private:
    // 8 cells as 16: each one, then an empty one.
    static uint16_t stretch(uint8_t cells) {
        uint16_t w = 0;
        for (int i = 7; i >= 0; --i) w = uint16_t(w << 2 | ((cells >> i) & 1) << 1);
        return w;
    }
    void put(uint16_t cells) {
        if (scale_ == 1) {
            push(cells);
        } else {
            push(stretch(uint8_t(cells >> 8)));
            push(stretch(uint8_t(cells)));
        }
    }
    void push(uint16_t w) {
        if (size_ < words_)
            out_[size_++] = w;
        else
            overflowed_ = true;
    }

    uint16_t* out_;
    uint32_t words_;
    bool fm_;
    uint8_t scale_;
    uint32_t size_ = 0;
    bool last_bit_ = false;
    bool overflowed_ = false;
};

// A field: its zeros, its sync (MFM: A1 A1 A1 and the mark; FM: the mark
// with clock C7), the field and its CRC, which covers the sync bytes too.
static void syncedField(TrackCells& e, bool fm, uint8_t mark, const uint8_t* field, size_t n) {
    uint16_t crc = crc16::INITIAL;
    if (fm) {
        e.fill(0x00, 6);
        e.mark(mark, 0xC7);
    } else {
        e.fill(0x00, PRESYNC_BYTES);
        for (int i = 0; i < 3; ++i) e.sync(mfm::SYNC_A1);
        for (int i = 0; i < 3; ++i) crc = crc16::update(crc, 0xA1);
        e.byte(mark);
    }
    crc = crc16::update(crc, mark);
    e.bytes(field, n);
    crc = crc16::compute(field, n, crc);
    e.byte(uint8_t(crc >> 8));
    e.byte(uint8_t(crc));
}

bool buildTrack(const Format& fmt, uint8_t cylinder, uint8_t head, const uint8_t* data, uint16_t* out) {
    const Track& t = fmt.track(cylinder, head);
    if (fmt.encoding == ENCODING_AMIGA) return amiga::buildTrack(cylinder, head, t.sectors, data, out, fmt.track_bytes);
    if (t.usedBytes() > fmt.track_bytes) return false;
    TrackCells e(out, fmt.track_bytes, t.fm, t.scale);
    const uint8_t gap = t.fm ? 0xFF : GAP_BYTE;

    e.fill(gap, t.gap4a);
    if (t.iam) {
        if (t.fm) {
            e.fill(0x00, 6);
            e.mark(MARK_IAM, 0xD7);
        } else {
            e.fill(0x00, PRESYNC_BYTES);
            for (int i = 0; i < 3; ++i) e.sync(mfm::SYNC_C2);
            e.byte(MARK_IAM);
        }
        e.fill(gap, t.gap1);
    }

    uint16_t order[MAX_SECTORS];
    t.sectorMap(cylinder, head, order);
    uint8_t h = t.head_id < 0 ? head : uint8_t(t.head_id);
    for (uint16_t slot = 0; slot < t.sectors; ++slot) {
        uint16_t s = order[slot];
        const uint8_t id[4] = {cylinder, h, uint8_t(t.first_id + s), t.size_code};
        syncedField(e, t.fm, MARK_IDAM, id, sizeof id);
        e.fill(gap, t.gap2);
        syncedField(e, t.fm, MARK_DAM, data + s * t.sectorBytes(), t.sectorBytes());
        e.fill(gap, t.gap3);
    }

    e.finish(gap);
    return !e.overflowed() && e.size() == fmt.track_bytes;
}

void buildBlankTrack(const Format& fmt, uint16_t* out) {
    mfm::Encoder e(out, fmt.track_bytes);
    e.fill(GAP_BYTE, fmt.track_bytes);
}

}  // namespace ibm
