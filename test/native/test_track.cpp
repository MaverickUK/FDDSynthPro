// Tests for slice 2's encoding: CRC-16, MFM, the IBM track layout and the
// generated disk. host/tests/test_track_encoder.py then checks whole tracks
// against Greaseweazle's encoder and decoder.
#include <cstdint>
#include <cstring>
#include <vector>

#include "check.h"
#include "crc16.h"
#include "gen_disk.h"
#include "ibm_track.h"
#include "mfm.h"
#include "rdata_stream.h"

namespace {  // this file's own helpers and tests

// ---- CRC-16 ------------------------------------------------------------------

TEST(test_crc16_check_values) {
    const uint8_t check[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK_EQ(crc16::compute(check, sizeof check), 0x29B1);  // CRC-16/CCITT-FALSE check value
    // The ID field of cylinder 0, head 0, sector 1, 512 bytes, as on every PC disk.
    const uint8_t idam[] = {0xA1, 0xA1, 0xA1, 0xFE, 0, 0, 1, 2};
    CHECK_EQ(crc16::compute(idam, sizeof idam), 0xCA6F);
    // A field followed by its own CRC leaves zero.
    const uint8_t with_crc[] = {0xA1, 0xA1, 0xA1, 0xFE, 0, 0, 1, 2, 0xCA, 0x6F};
    CHECK_EQ(crc16::compute(with_crc, sizeof with_crc), 0);
}

// ---- MFM ---------------------------------------------------------------------

TEST(test_mfm_known_words) {
    CHECK_EQ(mfm::encodeByte(0x00, false), 0xAAAA);  // all clocks
    CHECK_EQ(mfm::encodeByte(0x00, true), 0x2AAA);   // no clock after a 1
    CHECK_EQ(mfm::encodeByte(0xFF, false), 0x5555);  // no clocks
    CHECK_EQ(mfm::encodeByte(0x4E, false), 0x9254);  // the gap byte
    CHECK_EQ(mfm::encodeByte(0xA1, false), 0x44A9);  // A1 with its clock...
    CHECK(mfm::encodeByte(0xA1, false) != mfm::SYNC_A1);  // ...which the sync word lacks
    CHECK_EQ(mfm::decodeWord(mfm::SYNC_A1), 0xA1);
    CHECK_EQ(mfm::decodeWord(mfm::SYNC_C2), 0xC2);
}

TEST(test_mfm_round_trip) {
    bool ok = true;
    for (int b = 0; b < 256; ++b)
        for (int prev = 0; prev < 2; ++prev)
            if (mfm::decodeWord(mfm::encodeByte(uint8_t(b), prev)) != b) ok = false;
    CHECK(ok);
}

TEST(test_mfm_encoder) {
    uint16_t out[3];
    mfm::Encoder e(out, 3);
    e.byte(0x01);  // ends in a 1...
    e.byte(0x00);  // ...so no first clock
    e.sync(mfm::SYNC_A1);
    CHECK_EQ(out[1], 0x2AAA);
    CHECK_EQ(e.size(), 3u);
    CHECK(!e.overflowed());
    e.byte(0x00);
    CHECK(e.overflowed());
    CHECK_EQ(e.size(), 3u);
}

// ---- The track ---------------------------------------------------------------

// The cells as one bit string, with the run rules every MFM stream keeps:
// no two 1s together, and at most three 0s between 1s, including across
// the end of the track back to its start.
static bool keepsMfmRules(const std::vector<uint16_t>& words) {
    std::vector<bool> cells;
    for (uint16_t w : words)
        for (int i = 15; i >= 0; --i) cells.push_back((w >> i) & 1);
    size_t n = cells.size();
    size_t first_one = 0;
    while (first_one < n && !cells[first_one]) ++first_one;
    if (first_one == n) return false;
    unsigned zeros = 0;
    for (size_t k = 1; k <= n; ++k) {
        bool c = cells[(first_one + k) % n];
        if (c) {
            if (zeros == 0 || zeros > 3) return false;
            zeros = 0;
        } else {
            ++zeros;
        }
    }
    return true;
}

// A minimal reader for a built track, independent of buildTrack: finds each
// field by its three sync words, checks its CRC, and collects the sectors.
struct ParsedTrack {
    unsigned index_marks = 0;
    unsigned bad_crcs = 0;
    std::vector<std::vector<uint8_t>> ids;   // C H R N per ID field, in order
    std::vector<std::vector<uint8_t>> data;  // data fields, in order
};

static ParsedTrack parseTrack(const std::vector<uint16_t>& w, uint32_t sector_bytes) {
    ParsedTrack t;
    for (size_t i = 0; i + 4 < w.size(); ++i) {
        if (w[i] == mfm::SYNC_C2 && w[i + 1] == mfm::SYNC_C2 && w[i + 2] == mfm::SYNC_C2) {
            if (mfm::decodeWord(w[i + 3]) == ibm::MARK_IAM) ++t.index_marks;
            i += 3;
            continue;
        }
        if (!(w[i] == mfm::SYNC_A1 && w[i + 1] == mfm::SYNC_A1 && w[i + 2] == mfm::SYNC_A1)) continue;
        uint8_t mark = mfm::decodeWord(w[i + 3]);
        size_t n = mark == ibm::MARK_IDAM ? 4 : sector_bytes;
        if (i + 4 + n + 2 > w.size()) break;
        std::vector<uint8_t> field = {0xA1, 0xA1, 0xA1, mark};
        for (size_t k = 0; k < n + 2; ++k) field.push_back(mfm::decodeWord(w[i + 4 + k]));
        if (crc16::compute(field.data(), field.size()) != 0) ++t.bad_crcs;
        std::vector<uint8_t> body(field.begin() + 4, field.end() - 2);
        (mark == ibm::MARK_IDAM ? t.ids : t.data).push_back(body);
        i += 3 + n + 2;
    }
    return t;
}

static std::vector<uint16_t> build(const ibm::Format& fmt, uint8_t cyl, uint8_t head, std::vector<uint8_t>& data) {
    data.resize(fmt.trackDataBytes());
    gen_disk::track(fmt, cyl, head, data.data());
    std::vector<uint16_t> words(fmt.track_bytes);
    CHECK(ibm::buildTrack(fmt, cyl, head, data.data(), words.data()));
    return words;
}

TEST(test_track_formats_fit) {
    CHECK_EQ(ibm::HD_1440.kinds[0].usedBytes(), 12422u);
    CHECK_EQ(ibm::DD_720.kinds[0].usedBytes(), 6068u);
    CHECK_EQ(ibm::HD_1440.trackDataBytes(), 9216u);
    ibm::Format too_long = ibm::HD_1440;
    too_long.kinds[0].gap3 = 120;
    std::vector<uint16_t> words(too_long.track_bytes);
    std::vector<uint8_t> data(too_long.trackDataBytes());
    CHECK(!ibm::buildTrack(too_long, 0, 0, data.data(), words.data()));
}

TEST(test_track_layout_hd) {
    std::vector<uint8_t> data;
    std::vector<uint16_t> w = build(ibm::HD_1440, 0, 0, data);
    // Gap 4a, then the index mark.
    bool gap = true;
    for (int i = 0; i < 80; ++i) gap = gap && mfm::decodeWord(w[i]) == 0x4E;
    CHECK(gap);
    CHECK_EQ(w[0], 0x9254);
    CHECK_EQ(w[92], mfm::SYNC_C2);
    CHECK_EQ(w[94], mfm::SYNC_C2);
    CHECK_EQ(mfm::decodeWord(w[95]), ibm::MARK_IAM);
    // The first ID field: 12 x 00 after gap 1, at byte 146.
    CHECK_EQ(w[158], mfm::SYNC_A1);
    CHECK_EQ(mfm::decodeWord(w[161]), ibm::MARK_IDAM);
    CHECK_EQ(mfm::decodeWord(w[164]), 1);  // R: sector 1
    CHECK_EQ(mfm::decodeWord(w[165]), 2);  // N: 512 bytes
    CHECK_EQ(mfm::decodeWord(w[166]), 0xCA);
    CHECK_EQ(mfm::decodeWord(w[167]), 0x6F);
    // The rest of the track is gap.
    CHECK_EQ(mfm::decodeWord(w[12499]), 0x4E);
}

TEST(test_track_parses_back) {
    const ibm::Format* formats[] = {&ibm::HD_1440, &ibm::DD_720};
    for (const ibm::Format* fmt : formats) {
        for (uint8_t cyl : {0, 1, 40, 79}) {
            for (uint8_t head : {0, 1}) {
                std::vector<uint8_t> data;
                std::vector<uint16_t> w = build(*fmt, cyl, head, data);
                CHECK(keepsMfmRules(w));
                ParsedTrack t = parseTrack(w, fmt->sectorBytes());
                CHECK_EQ(t.index_marks, 1u);
                CHECK_EQ(t.bad_crcs, 0u);
                CHECK_EQ(t.ids.size(), size_t(fmt->kinds[0].sectors));
                CHECK_EQ(t.data.size(), size_t(fmt->kinds[0].sectors));
                bool ids_ok = true, data_ok = true;
                for (size_t s = 0; s < t.ids.size() && s < t.data.size(); ++s) {
                    std::vector<uint8_t> want_id = {cyl, head, uint8_t(s + 1), 2};
                    ids_ok = ids_ok && t.ids[s] == want_id;
                    data_ok = data_ok && std::memcmp(t.data[s].data(), &data[s * 512], 512) == 0;
                }
                CHECK(ids_ok);
                CHECK(data_ok);
            }
        }
    }
}

TEST(test_track_crc_catches_a_flipped_bit) {
    std::vector<uint8_t> data;
    std::vector<uint16_t> w = build(ibm::HD_1440, 5, 1, data);
    w[1000] ^= 0x0001;  // one data cell, inside sector 2's data
    ParsedTrack t = parseTrack(w, 512);
    CHECK_EQ(t.bad_crcs, 1u);
}

// ---- Generated disk ----------------------------------------------------------

// zlib's CRC-32, to compare with Python's zlib.crc32.
static uint32_t crc32(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

TEST(test_gen_disk_known_answers) {
    // The same values as HDDSynthPro, and host/tests/test_track_encoder.py.
    struct { uint32_t lba, crc; } known[] = {
        {0, 0xe1249abbu}, {1, 0xe9018a87u}, {12345, 0xd29a1ff2u},
        {131071, 0x69680a43u}, {0x0FFFFFFFu, 0x907ed2feu},
    };
    uint8_t buf[gen_disk::SECTOR_BYTES];
    for (auto& k : known) {
        gen_disk::sector(k.lba, buf);
        CHECK_EQ(crc32(buf, sizeof buf), k.crc);
    }
}

TEST(test_gen_disk_track_order) {
    // LBA order is IMG order: cylinder 1 head 0 follows cylinder 0 head 1.
    CHECK_EQ(ibm::HD_1440.trackOffset(0, 1) / 512, 18u);
    CHECK_EQ(ibm::HD_1440.trackOffset(1, 0) / 512, 36u);
    CHECK_EQ(ibm::DD_720.trackOffset(79, 1) / 512, 1431u);
    std::vector<uint8_t> t(ibm::HD_1440.trackDataBytes());
    gen_disk::track(ibm::HD_1440, 1, 0, t.data());
    CHECK_EQ(t[0], 36);           // sector 1 of the track names LBA 36
    CHECK_EQ(t[17 * 512], 53);    // sector 18 names LBA 53
}

// ---- The RDATA stream ----------------------------------------------------------

TEST(test_interleave) {
    // Head 0's cell first in each slot, first slot in the top bits.
    CHECK_EQ(rdata::interleave(0x8000, 0x0000), 0x80000000u);
    CHECK_EQ(rdata::interleave(0x0000, 0x8000), 0x40000000u);
    CHECK_EQ(rdata::interleave(0x0001, 0x0001), 0x00000003u);
    CHECK_EQ(rdata::interleave(0xFFFF, 0x0000), 0xAAAAAAAAu);
    bool ok = true;
    for (uint32_t i = 0; i < 70000; i += 7) {
        uint16_t a = uint16_t(i * 2654435761u >> 16), b = uint16_t(i * 40503u);
        uint32_t s = rdata::interleave(a, b);
        ok = ok && rdata::headCells(s, 0) == a && rdata::headCells(s, 1) == b;
    }
    CHECK(ok);
}

TEST(test_build_cylinder) {
    const ibm::Format& fmt = ibm::HD_1440;
    std::vector<uint8_t> d0(fmt.trackDataBytes()), d1(fmt.trackDataBytes());
    gen_disk::track(fmt, 7, 0, d0.data());
    gen_disk::track(fmt, 7, 1, d1.data());
    std::vector<uint16_t> scratch(fmt.track_bytes), t0(fmt.track_bytes), t1(fmt.track_bytes);
    std::vector<uint32_t> cyl(fmt.track_bytes);
    CHECK(rdata::buildCylinder(fmt, 7, d0.data(), d1.data(), scratch.data(), cyl.data()));
    ibm::buildTrack(fmt, 7, 0, d0.data(), t0.data());
    ibm::buildTrack(fmt, 7, 1, d1.data(), t1.data());
    bool same = true;
    for (uint32_t i = 0; i < fmt.track_bytes; ++i)
        same = same && rdata::headCells(cyl[i], 0) == t0[i] && rdata::headCells(cyl[i], 1) == t1[i];
    CHECK(same);
}

TEST(test_clock_divider) {
    CHECK_EQ(ibm::HD_1440.dataRate(), 500000u);
    CHECK_EQ(ibm::DD_720.dataRate(), 250000u);
    // 150 MHz: 9.375 for HD (16 MHz, 1 us cells), 18.75 for DD; both exact.
    rdata::Divider d = rdata::clockDivider(150000000, 500000);
    CHECK_EQ(d.integer, 9);
    CHECK_EQ(d.frac, 96);
    CHECK(d.exact);
    d = rdata::clockDivider(150000000, 250000);
    CHECK_EQ(d.integer, 18);
    CHECK_EQ(d.frac, 192);
    CHECK(d.exact);
    d = rdata::clockDivider(200000000, 500000);  // overclocked: 12.5
    CHECK_EQ(d.integer, 12);
    CHECK_EQ(d.frac, 128);
    CHECK(d.exact);
    d = rdata::clockDivider(133000000, 500000);  // 8.3125
    CHECK(d.exact);
    d = rdata::clockDivider(125000000, 300000);
    CHECK(!d.exact);
}

}  // namespace

void runTrackTests() {
    RUN(test_crc16_check_values);
    RUN(test_mfm_known_words);
    RUN(test_mfm_round_trip);
    RUN(test_mfm_encoder);
    RUN(test_track_formats_fit);
    RUN(test_track_layout_hd);
    RUN(test_track_parses_back);
    RUN(test_track_crc_catches_a_flipped_bit);
    RUN(test_gen_disk_known_answers);
    RUN(test_gen_disk_track_order);
    RUN(test_interleave);
    RUN(test_build_cylinder);
    RUN(test_clock_divider);
}
