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
#include "cylinder_loader.h"
#include "rdata_stream.h"
#include "track_source.h"
#include "write_burst.h"
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

// ---- The whole write path: burst in, live track and disk out --------------------------------

// A writable disk in memory, starting as the generated disk.
class MemorySource : public TrackSource {
public:
    explicit MemorySource(const ibm::Format& fmt) : fmt_(fmt), image_(80 * 2 * fmt.trackDataBytes()) {
        for (uint8_t c = 0; c < 80; ++c)
            for (uint8_t h = 0; h < 2; ++h) gen_disk::track(fmt, c, h, &image_[imageTrackOffset(fmt, c, h)]);
    }
    const ibm::Format& format() const override { return fmt_; }
    bool readTrack(uint8_t c, uint8_t h, uint8_t* out) override {
        if (c >= 80) return false;
        std::memcpy(out, &image_[imageTrackOffset(fmt_, c, h)], fmt_.trackDataBytes());
        return true;
    }
    bool writeSector(uint8_t c, uint8_t h, uint8_t s, const uint8_t* data) override {
        std::memcpy(&image_[imageTrackOffset(fmt_, c, h) + s * 512u], data, 512);
        ++writes;
        return true;
    }
    bool writable() const override { return true; }
    const uint8_t* sector(uint8_t c, uint8_t h, uint8_t s) const { return &image_[imageTrackOffset(fmt_, c, h) + s * 512u]; }
    unsigned writes = 0;

private:
    const ibm::Format& fmt_;
    std::vector<uint8_t> image_;
};

// As the WDATA capture delivers them: 8-bit ticks, less the edge ticks.
static std::vector<uint8_t> captured(const std::vector<uint32_t>& flux, uint32_t edge_ticks) {
    std::vector<uint8_t> out;
    for (uint32_t t : flux) out.push_back(uint8_t(t - edge_ticks));
    return out;
}

struct WriteRig {
    MemorySource source{ibm::HD_1440};
    std::vector<uint32_t> slots = std::vector<uint32_t>(12500);
    std::vector<uint16_t> scratch = std::vector<uint16_t>(12500);
    std::vector<uint8_t> d0 = std::vector<uint8_t>(9216), d1 = std::vector<uint8_t>(9216);
    CylinderLoader loader{source, slots.data(), scratch.data(), d0.data(), d1.data()};
    std::vector<uint8_t> store = std::vector<uint8_t>(12 * 1024);
    std::vector<mfm::Field> fields = std::vector<mfm::Field>(64);
    BurstWriter writer{source, loader, store.data(), store.size(), fields.data(), fields.size()};

    void load(uint8_t cyl) {
        loader.request(cyl);
        while (loader.service() != CylinderLoader::Step::Idle) {}
    }
    // The live buffer's head, decoded: what the host would read now.
    std::vector<uint8_t> liveSector(uint8_t head, uint8_t s) {
        std::vector<uint16_t> cells(12500);
        for (size_t i = 0; i < cells.size(); ++i) cells[i] = rdata::headCells(slots[i], head);
        Decoded d;
        decode(d, toFlux(cells));
        for (size_t i = 0; i < d.scanner.count(); ++i) {
            const mfm::Field& f = d.scanner.field(i);
            if (f.mark == mfm::MARK_DAM && f.crc_ok && i > 0 && d.scanner.field(i - 1).id[2] == s + 1)
                return std::vector<uint8_t>(&d.store[f.data_offset], &d.store[f.data_offset] + 512);
        }
        return {};
    }
};

TEST(test_burst_sector_write) {
    WriteRig r;
    r.load(9);
    auto data = pattern(42);
    auto flux = toFlux(sectorWriteCells(data.data()), Timing{1.01, 0.1, 0.0}, 12);
    auto bytes = captured(flux, 2);
    BurstResult res = r.writer.apply(WriteBurst{9, 1, ibm::HD_1440.dataFieldStart(4), bytes.data(), bytes.size()}, 16, 2);
    CHECK_EQ(res.decoded.updates, 1u);
    CHECK_EQ(res.patched, 1u);
    CHECK_EQ(res.persisted, 1u);
    // On the disk, and in what the host reads back at once.
    CHECK(std::memcmp(r.source.sector(9, 1, 4), data.data(), 512) == 0);
    CHECK(r.liveSector(1, 4) == data);
    // The other head, and the other sectors, are untouched.
    std::vector<uint8_t> before(512);
    gen_disk::sector(gen_disk::firstLba(ibm::HD_1440, 9, 1) + 3, before.data());
    CHECK(r.liveSector(1, 3) == before);
    gen_disk::sector(gen_disk::firstLba(ibm::HD_1440, 9, 0) + 4, before.data());
    CHECK(r.liveSector(0, 4) == before);
}

TEST(test_burst_track_write) {
    WriteRig r;
    r.load(30);
    // A whole new track 30.0, as `gw write` sends it.
    std::vector<uint8_t> data(9216);
    for (size_t i = 0; i < data.size(); ++i) data[i] = uint8_t(i * 13 + 1);
    std::vector<uint16_t> cells(12500);
    ibm::buildTrack(ibm::HD_1440, 30, 0, data.data(), cells.data());
    auto bytes = captured(toFlux(cells, Timing{0.99, 0.1, 0.0}, 13), 2);
    BurstResult res = r.writer.apply(WriteBurst{30, 0, 0, bytes.data(), bytes.size()}, 16, 2);
    CHECK_EQ(res.persisted, 18u);
    CHECK_EQ(r.source.writes, 18u);
    bool all = true;
    for (uint8_t s = 0; s < 18; ++s) all = all && r.liveSector(0, s) == std::vector<uint8_t>(&data[s * 512], &data[s * 512] + 512);
    CHECK(all);
}

TEST(test_burst_survives_a_reload) {
    // Written sectors come back from the disk after the head moves away and back.
    WriteRig r;
    r.load(2);
    auto data = pattern(7);
    auto bytes = captured(toFlux(sectorWriteCells(data.data())), 2);
    r.writer.apply(WriteBurst{2, 0, ibm::HD_1440.dataFieldStart(0), bytes.data(), bytes.size()}, 16, 2);
    r.load(3);
    r.load(2);
    CHECK(r.liveSector(0, 0) == data);
}

TEST(test_burst_on_an_unloaded_cylinder_still_persists) {
    // The head moved before core 0 got to the burst: the buffer can't take
    // it, but the disk does, and the next load shows it.
    WriteRig r;
    r.load(5);
    auto data = pattern(3);
    auto bytes = captured(toFlux(sectorWriteCells(data.data())), 2);
    BurstResult res = r.writer.apply(WriteBurst{4, 0, ibm::HD_1440.dataFieldStart(1), bytes.data(), bytes.size()}, 16, 2);
    CHECK_EQ(res.patched, 0u);
    CHECK_EQ(res.persisted, 1u);
    r.load(4);
    CHECK(r.liveSector(0, 1) == data);
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
    RUN(test_burst_sector_write);
    RUN(test_burst_track_write);
    RUN(test_burst_survives_a_reload);
    RUN(test_burst_on_an_unloaded_cylinder_still_persists);
}
