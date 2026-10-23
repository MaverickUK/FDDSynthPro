// Tests for slice 5's decoding: the data separator, the field scanner and
// the write interpreter. Tracks are built with ibm::buildTrack (itself
// checked cell for cell against Greaseweazle), turned into flux intervals
// with the kinds of timing error a real host's writes have, and read back.
#include <cstdint>
#include <cstring>
#include <vector>

#include "check.h"
#include "gen_disk.h"
#include "ibm_track.h"
#include "mfm.h"
#include "mfm_decode.h"
#include "track_write.h"
#include "crc16.h"

static const uint32_t TICKS = 16;  // capture ticks per cell, as the WDATA program measures them

// ---- Helpers --------------------------------------------------------------------

// A deterministic generator in [-1, 1).
struct Lcg {
    uint32_t s;
    double next() {
        s = s * 1664525u + 1013904223u;
        return (s >> 8) / double(1u << 24) * 2.0 - 1.0;
    }
};

struct Timing {
    double speed = 1.0;   // the host's clock: 1.03 = 3% slow cells (longer intervals)
    double jitter = 0.0;  // random edge error, in cells (each edge, both ways)
    double precomp = 0.0; // write precompensation, in cells: shifts edges in tight patterns
};

// The flux intervals (in ticks) for a stream of cell words, starting at a
// cell boundary: each interval runs from the previous 1 (or the start) to
// the next.
static std::vector<uint32_t> toFlux(const std::vector<uint16_t>& words, Timing t = {}, uint32_t seed = 1) {
    Lcg rng{seed};
    std::vector<uint32_t> out;
    double last_edge = 0.0;  // in ticks
    uint32_t cell = 0;
    for (size_t w = 0; w < words.size(); ++w) {
        for (int b = 15; b >= 0; --b, ++cell) {
            if (!((words[w] >> b) & 1)) continue;
            double edge = (cell + 1) * t.speed;
            // Precompensation: a 1 right after a 0 0 1 pattern moves early, as hosts do.
            if (t.precomp > 0 && (cell % 3) == 0) edge -= t.precomp;
            edge += t.jitter * rng.next();
            // A capture counts a fixed clock: each edge's time is rounded to
            // a tick, and the intervals are the differences, so rounding
            // never adds up.
            double tick = double(uint64_t(edge * TICKS + 0.5));
            double ticks = tick - last_edge;
            out.push_back(ticks < 1 ? 1u : uint32_t(ticks));
            last_edge = tick;
        }
    }
    return out;
}

struct Decoded {
    std::vector<uint8_t> store = std::vector<uint8_t>(40 * 1024);
    std::vector<mfm::Field> fields = std::vector<mfm::Field>(64);
    mfm::FieldScanner scanner{store.data(), store.size(), fields.data(), fields.size()};
};

static void decode(Decoded& d, const std::vector<uint32_t>& flux, uint16_t default_len = 512) {
    mfm::DataSeparator sep(TICKS);
    d.scanner.reset(default_len);
    for (uint32_t ticks : flux) {
        unsigned n = sep.cells(ticks);
        for (unsigned i = 1; i < n; ++i) d.scanner.addCell(false);
        d.scanner.addCell(true);
    }
}

static std::vector<uint16_t> trackCells(const ibm::Format& fmt, uint8_t cyl, uint8_t head,
                                        std::vector<uint8_t>& data) {
    data.resize(fmt.trackDataBytes());
    gen_disk::track(fmt, cyl, head, data.data());
    std::vector<uint16_t> words(fmt.track_bytes);
    ibm::buildTrack(fmt, cyl, head, data.data(), words.data());
    return words;
}

// True if the decoded fields are the track's 18 (or 9) sectors, in order,
// all good, at the layout's positions, with the right data.
static bool wholeTrack(const ibm::Format& fmt, uint8_t cyl, uint8_t head, const std::vector<uint8_t>& data,
                       const Decoded& d, bool check_positions = true) {
    if (d.scanner.count() != 2u * fmt.sectors) return false;
    for (uint8_t s = 0; s < fmt.sectors; ++s) {
        const mfm::Field& id = d.scanner.field(2 * s);
        const mfm::Field& dam = d.scanner.field(2 * s + 1);
        if (id.mark != mfm::MARK_IDAM || !id.crc_ok || dam.mark != mfm::MARK_DAM || !dam.crc_ok) return false;
        if (id.id[0] != cyl || id.id[1] != head || id.id[2] != s + 1 || id.id[3] != 2) return false;
        if (std::memcmp(&d.store[dam.data_offset], &data[s * 512], 512) != 0) return false;
        if (check_positions) {
            if (id.cell != (fmt.idFieldStart(s) + ibm::PRESYNC_BYTES) * 16) return false;
            if (dam.cell != (fmt.dataFieldStart(s) + ibm::PRESYNC_BYTES) * 16) return false;
        }
    }
    return true;
}

// ---- Layout ----------------------------------------------------------------------

TEST(test_field_positions) {
    const ibm::Format& f = ibm::HD_1440;
    CHECK_EQ(f.idFieldStart(0), 146u);  // gap 4a, IAM, gap 1
    CHECK_EQ(f.dataFieldStart(0), 146u + 44u);
    CHECK_EQ(f.idFieldStart(1), 146u + 44u + 638u);
    CHECK_EQ(f.dataFieldStart(17) + 638u, f.usedBytes());
}

// ---- Data separator and scanner ----------------------------------------------------

TEST(test_exact_flux_reads_back) {
    for (const ibm::Format* fmt : {&ibm::HD_1440, &ibm::DD_720}) {
        std::vector<uint8_t> data;
        auto words = trackCells(*fmt, 12, 1, data);
        Decoded d;
        decode(d, toFlux(words));
        CHECK(wholeTrack(*fmt, 12, 1, data, d));
        CHECK_EQ(d.scanner.overflowed(), 0u);
    }
}

TEST(test_jitter_speed_and_precomp) {
    std::vector<uint8_t> data;
    auto words = trackCells(ibm::HD_1440, 40, 0, data);
    // Jitter of 0.2 cell either way on every edge: several times what a real
    // host's writes have.
    Decoded a;
    decode(a, toFlux(words, Timing{1.0, 0.2, 0.0}, 7));
    CHECK(wholeTrack(ibm::HD_1440, 40, 0, data, a, false));
    // A host whose clock is 8% slow, or 8% fast.
    Decoded b, c;
    decode(b, toFlux(words, Timing{1.08, 0.1, 0.0}, 8));
    decode(c, toFlux(words, Timing{0.92, 0.1, 0.0}, 9));
    CHECK(wholeTrack(ibm::HD_1440, 40, 0, data, b, false));
    CHECK(wholeTrack(ibm::HD_1440, 40, 0, data, c, false));
    // Write precompensation of 1/8 cell on top of jitter.
    Decoded e;
    decode(e, toFlux(words, Timing{1.0, 0.15, 0.125}, 10));
    CHECK(wholeTrack(ibm::HD_1440, 40, 0, data, e, false));
}

TEST(test_too_much_jitter_fails_the_crc) {
    // Over half a cell, edges round to the wrong cell: the CRC must catch it.
    std::vector<uint8_t> data;
    auto words = trackCells(ibm::HD_1440, 3, 0, data);
    Decoded d;
    decode(d, toFlux(words, Timing{1.0, 0.7, 0.0}, 11));
    CHECK(!wholeTrack(ibm::HD_1440, 3, 0, data, d, false));
    bool any_bad = false;
    for (size_t i = 0; i < d.scanner.count(); ++i) any_bad = any_bad || !d.scanner.field(i).crc_ok;
    CHECK(any_bad || d.scanner.count() < 36u);
}

TEST(test_scanner_store_overflow) {
    std::vector<uint8_t> data;
    auto words = trackCells(ibm::HD_1440, 0, 0, data);
    std::vector<uint8_t> store(2000);  // room for three data fields
    std::vector<mfm::Field> fields(64);
    mfm::FieldScanner scanner(store.data(), store.size(), fields.data(), fields.size());
    scanner.reset(512);
    mfm::DataSeparator sep(TICKS);
    for (uint32_t t : toFlux(words)) {
        unsigned n = sep.cells(t);
        for (unsigned i = 1; i < n; ++i) scanner.addCell(false);
        scanner.addCell(true);
    }
    CHECK_EQ(scanner.overflowed(), 15u);
    CHECK_EQ(scanner.count(), 18u + 3u);
}

// ---- The write interpreter ------------------------------------------------------------

// A sector write as a PC's controller makes it: the data field only.
static std::vector<uint16_t> sectorWriteCells(const uint8_t* data) {
    std::vector<uint16_t> words(ibm::PRESYNC_BYTES + 4 + 512 + 2 + 4);
    mfm::Encoder e(words.data(), words.size());
    e.fill(0x00, ibm::PRESYNC_BYTES);
    for (int i = 0; i < 3; ++i) e.sync(mfm::SYNC_A1);
    e.byte(mfm::MARK_DAM);
    e.bytes(data, 512);
    uint16_t crc = crc16::INITIAL;
    for (int i = 0; i < 3; ++i) crc = crc16::update(crc, 0xA1);
    crc = crc16::update(crc, mfm::MARK_DAM);
    crc = crc16::compute(data, 512, crc);
    e.byte(uint8_t(crc >> 8));
    e.byte(uint8_t(crc));
    e.fill(0x4E, 4);
    return words;
}

static std::vector<uint8_t> pattern(uint8_t seed) {
    std::vector<uint8_t> d(512);
    for (int i = 0; i < 512; ++i) d[i] = uint8_t(i * 7 + seed);
    return d;
}

TEST(test_sector_write_placed_by_position) {
    const ibm::Format& fmt = ibm::HD_1440;
    auto data = pattern(5);
    Decoded d;
    decode(d, toFlux(sectorWriteCells(data.data()), Timing{1.0, 0.1, 0.0}, 3));
    CHECK_EQ(d.scanner.count(), 1u);
    track_write::Update u[4];
    // WGATE on where sector 7's data field begins: exact, a little early, a little late.
    for (int shift : {0, -10, 10}) {
        track_write::Result r = track_write::interpret(fmt, 4, 1, uint32_t(int(fmt.dataFieldStart(6)) + shift),
                                                       d.scanner, u, 4);
        CHECK_EQ(r.updates, 1u);
        CHECK_EQ(u[0].sector, 6);
        CHECK(std::memcmp(&d.store[u[0].data_offset], data.data(), 512) == 0);
    }
    // Far from any data field: not placed.
    track_write::Result r = track_write::interpret(fmt, 4, 1, fmt.dataFieldStart(6) + 100, d.scanner, u, 4);
    CHECK_EQ(r.updates, 0u);
    CHECK_EQ(r.unplaced, 1u);
}

TEST(test_track_write_placed_by_ids) {
    const ibm::Format& fmt = ibm::HD_1440;
    std::vector<uint8_t> data;
    auto words = trackCells(fmt, 22, 0, data);
    Decoded d;
    decode(d, toFlux(words, Timing{1.01, 0.15, 0.0}, 4));
    track_write::Update u[32];
    track_write::Result r = track_write::interpret(fmt, 22, 0, 0, d.scanner, u, 32);
    CHECK_EQ(r.updates, 18u);
    bool all = true;
    for (unsigned i = 0; i < r.updates; ++i)
        all = all && u[i].sector == i && std::memcmp(&d.store[u[i].data_offset], &data[i * 512], 512) == 0;
    CHECK(all);
    // The same track written while the head is on another cylinder: refused.
    r = track_write::interpret(fmt, 23, 0, 0, d.scanner, u, 32);
    CHECK_EQ(r.updates, 0u);
    CHECK_EQ(r.unplaced, 18u);
}

TEST(test_bad_crc_is_not_written) {
    auto data = pattern(9);
    auto words = sectorWriteCells(data.data());
    words[100] ^= 0x0004;  // a data cell inside the field
    Decoded d;
    decode(d, toFlux(words));
    track_write::Update u[4];
    track_write::Result r =
        track_write::interpret(ibm::HD_1440, 0, 0, ibm::HD_1440.dataFieldStart(0), d.scanner, u, 4);
    CHECK_EQ(r.updates, 0u);
    CHECK_EQ(r.bad_crc, 1u);
}

TEST(test_dd_track_write) {
    const ibm::Format& fmt = ibm::DD_720;
    std::vector<uint8_t> data;
    auto words = trackCells(fmt, 79, 1, data);
    Decoded d;
    decode(d, toFlux(words, Timing{0.98, 0.2, 0.0}, 5));
    track_write::Update u[16];
    CHECK_EQ(track_write::interpret(fmt, 79, 1, 0, d.scanner, u, 16).updates, 9u);
}

void runWriteDecodeTests() {
    RUN(test_field_positions);
    RUN(test_exact_flux_reads_back);
    RUN(test_jitter_speed_and_precomp);
    RUN(test_too_much_jitter_fails_the_crc);
    RUN(test_scanner_store_overflow);
    RUN(test_sector_write_placed_by_position);
    RUN(test_track_write_placed_by_ids);
    RUN(test_bad_crc_is_not_written);
    RUN(test_dd_track_write);
}
