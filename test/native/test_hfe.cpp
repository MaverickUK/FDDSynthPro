// Tests for HFE flux images (src/hfe.*) beyond what Greaseweazle's own files
// cover (host/tests/test_hfe.py): v3's opcodes where they're awkward (an
// index part-way round, bits skipped, opcodes across blocks), files the
// drive refuses, and weak bits and written cells in the loader.
#include <cstdint>
#include <cstring>
#include <vector>

#include "check.h"
#include "cylinder_loader.h"
#include "hfe.h"
#include "rdata_stream.h"

namespace {  // this file's own helpers and tests

uint8_t rev(uint8_t b) {
    uint8_t r = 0;
    for (int i = 0; i < 8; ++i) r = uint8_t(r << 1 | ((b >> i) & 1));
    return r;
}

// An HFE file of `cyls` cylinders, two sides, each side's stored bytes
// (before HFE's bit reversal) given per track.
std::vector<uint8_t> makeHfe(unsigned version, uint8_t cyls, uint16_t kbps,
                             const std::vector<std::vector<uint8_t>>& sides, bool double_step = false) {
    std::vector<uint8_t> f(1024, 0xFF);
    std::memcpy(f.data(), version == 3 ? "HXCHFEV3" : "HXCPICFE", 8);
    f[8] = 0;
    f[9] = cyls;
    f[10] = 2;
    f[11] = 0;
    f[12] = uint8_t(kbps), f[13] = uint8_t(kbps >> 8);
    f[14] = f[15] = 0;
    f[16] = 0;
    f[17] = 1;
    f[18] = 1, f[19] = 0;  // the track table in block 1
    f[20] = 0xFF;
    f[21] = double_step ? 0 : 0xFF;
    for (unsigned c = 0; c < cyls; ++c) {
        const auto& s0 = sides[c * 2];
        const auto& s1 = sides[c * 2 + 1];
        size_t n = s0.size() > s1.size() ? s0.size() : s1.size();
        size_t blocks = (n + 255) / 256;
        uint16_t first = uint16_t(f.size() / 512);
        f[512 + 4 * c] = uint8_t(first), f[513 + 4 * c] = uint8_t(first >> 8);
        f[514 + 4 * c] = uint8_t(2 * n), f[515 + 4 * c] = uint8_t((2 * n) >> 8);
        for (size_t b = 0; b < blocks; ++b)
            for (const auto* s : {&s0, &s1})
                for (size_t i = 0; i < 256; ++i) {
                    size_t k = b * 256 + i;
                    f.push_back(k < s->size() ? rev((*s)[k]) : 0x88);
                }
    }
    return f;
}

std::vector<uint8_t> repeat(uint8_t b, size_t n) { return std::vector<uint8_t>(n, b); }

TEST(test_hfe_header) {
    hfe::Header h;
    std::vector<uint8_t> f = makeHfe(1, 2, 250, {repeat(0xAA, 64), repeat(0xAA, 64), repeat(0xAA, 64), repeat(0xAA, 64)});
    CHECK(hfe::parseHeader(f.data(), f.size(), h) == nullptr);
    CHECK_EQ(h.version, 1);
    CHECK_EQ(h.cyls, 2);
    CHECK_EQ(h.bitrate, 250);
    CHECK(h.write_allowed && !h.double_step);
    f[0] = 'X';
    CHECK(hfe::parseHeader(f.data(), f.size(), h) != nullptr);  // not an HFE
    f = makeHfe(1, 2, 2000, {repeat(0xAA, 64), repeat(0xAA, 64), repeat(0xAA, 64), repeat(0xAA, 64)});
    CHECK(hfe::parseHeader(f.data(), f.size(), h) != nullptr);  // a rate the drive can't stream
    f = makeHfe(1, 1, 250, {repeat(0xAA, 64), repeat(0xAA, 64)}, true);
    CHECK(hfe::parseHeader(f.data(), f.size(), h) == nullptr && h.double_step);
}

TEST(test_hfe_v1_cells_and_length) {
    // Cylinder lengths differ (a cylinder's sides are one length in the
    // file): every track is the longest's, a shorter one carrying on from
    // its start.
    std::vector<uint8_t> long_side = repeat(0x12, 600), short_side = repeat(0x34, 500);
    long_side[0] = 0x81;
    short_side[0] = 0x99;
    std::vector<uint8_t> f = makeHfe(1, 2, 250, {long_side, long_side, short_side, short_side});
    hfe::MemoryFile file(f.data(), uint32_t(f.size()));
    hfe::Disk d;
    CHECK(d.open(file, true) == nullptr);
    CHECK_EQ(d.format().track_bytes, 300);  // 600 bytes: 4,800 cells
    CHECK_EQ(d.format().periodUs(), 300u * 8000000u / 250000u);
    std::vector<uint8_t> t(600);
    CHECK(d.readTrack(0, 0, t.data()));
    CHECK(t == long_side);
    CHECK(d.readTrack(1, 1, t.data()));
    CHECK_EQ(t[499], 0x34);
    CHECK_EQ(t[500], 0x99);  // round to the start again
    CHECK_EQ(t[501], 0x34);
    CHECK(d.writable());
}

TEST(test_hfe_v3_opcodes) {
    // F2 n (rate), F0, F3 n b (the last 8 - n cells of b), F4 (weak), F1 (the
    // index) part-way round: the drive's track starts at the index.
    std::vector<uint8_t> s = {hfe::OP_BITRATE, 72, 0xA5, hfe::OP_NOP, 0x3C, hfe::OP_SKIPBITS, 4, 0x0B,
                              0x5A, hfe::OP_INDEX, 0xC3, hfe::OP_RAND, 0x99};
    // Cells, before the index: A5 3C then 1011 (B's low 4) then 5A; after: C3, 00 (weak), 99.
    std::vector<uint8_t> f = makeHfe(3, 1, 250, {s, s});
    hfe::MemoryFile file(f.data(), uint32_t(f.size()));
    hfe::Disk d;
    CHECK(d.open(file, true) == nullptr);
    CHECK(!d.writable());  // v3: read-only
    CHECK_EQ(d.variableRateTracks(), 0u);
    // 8 + 8 + 4 + 8 + 8 + 8 + 8 cells = 52: four words. Then the 0x88 padding
    // (stored bytes, after the track's own) would be cells too.
    std::vector<uint8_t> t(d.format().track_bytes * 2u);
    CHECK(d.readTrack(0, 0, t.data()));
    // From the index: C3 00 99, then A5 3C B 5A (the cells before it), as one loop.
    uint32_t cells = 0;
    auto put = [&](std::vector<bool>& v, uint8_t b, int n) {
        for (int i = 7; i >= 8 - n; --i) v.push_back((b >> i) & 1);
    };
    std::vector<bool> want;
    put(want, 0xC3, 8);
    put(want, 0x00, 8);
    put(want, 0x99, 8);
    put(want, 0xA5, 8);
    put(want, 0x3C, 8);
    put(want, 0xB0, 4);
    put(want, 0x5A, 8);
    bool same = true;
    for (bool w : want) same = same && (((t[cells / 8] >> (7 - cells % 8)) & 1) == w), ++cells;
    CHECK(same);
}

TEST(test_hfe_v3_variable_rate_counted) {
    std::vector<uint8_t> s = {hfe::OP_BITRATE, 72, 0xAA, 0xAA, hfe::OP_BITRATE, 66, 0xAA, 0xAA};
    std::vector<uint8_t> plain = {hfe::OP_BITRATE, 72, 0xAA, 0xAA, hfe::OP_BITRATE, 73, 0xAA, 0xAA};
    std::vector<uint8_t> f = makeHfe(3, 1, 250, {s, plain});
    hfe::MemoryFile file(f.data(), uint32_t(f.size()));
    hfe::Disk d;
    CHECK(d.open(file, false) == nullptr);
    CHECK_EQ(d.variableRateTracks(), 1u);  // 66 is 8% fast; 73 a nudge
}

// ---- In the loader ---------------------------------------------------------------------

struct Loaded {
    explicit Loaded(std::vector<uint8_t> file) : bytes(std::move(file)), file(bytes.data(), uint32_t(bytes.size())) {
        CHECK(disk.open(this->file, true) == nullptr);
    }
    std::vector<uint8_t> bytes;
    hfe::MemoryFile file;
    hfe::Disk disk;
    std::vector<uint32_t> slots = std::vector<uint32_t>(ibm::MAX_TRACK_WORDS);
    std::vector<uint16_t> scratch = std::vector<uint16_t>(ibm::MAX_TRACK_WORDS);
    std::vector<uint8_t> d0 = std::vector<uint8_t>(ibm::MAX_TRACK_DATA), d1 = std::vector<uint8_t>(ibm::MAX_TRACK_DATA);
    CylinderLoader loader{disk, slots.data(), scratch.data(), d0.data(), d1.data()};

    void load(uint8_t cyl) {
        loader.request(cyl);
        while (loader.service() != CylinderLoader::Step::Idle) {}
    }
    std::vector<uint16_t> head(uint8_t h) {
        std::vector<uint16_t> out(disk.format().track_bytes);
        for (size_t i = 0; i < out.size(); ++i) out[i] = rdata::headCells(slots[i], h);
        return out;
    }
};

TEST(test_hfe_weak_bits_are_noise_every_revolution) {
    // MFM-like cells with 40 bytes of no flux (weak bits) in the middle.
    std::vector<uint8_t> side = repeat(0x92, 400);  // 1001 0010...: never 8 empty cells
    for (int i = 200; i < 240; ++i) side[i] = 0x00;
    Loaded l(makeHfe(1, 2, 250, {side, repeat(0x92, 400), side, repeat(0x92, 400)}));
    l.load(1);
    CHECK_EQ(l.loader.weakRuns(), 1u);
    std::vector<uint16_t> a = l.head(0);
    l.loader.refreshWeak();
    std::vector<uint16_t> b = l.head(0);
    // Outside the weak run: as recorded. Inside: flux, and different each time.
    CHECK(a[0] == 0x9292 && b[0] == 0x9292 && a[90] == 0x9292 && b[150] == 0x9292);
    bool flux = false, differs = false;
    for (int w = 101; w < 119; ++w) flux = flux || a[w], differs = differs || a[w] != b[w];
    CHECK(flux);
    CHECK(differs);
    // The other head is untouched.
    CHECK(l.head(1)[110] == 0x9292);
}

TEST(test_hfe_write_splices_cells) {
    std::vector<uint8_t> side = repeat(0x92, 400);
    for (int i = 200; i < 240; ++i) side[i] = 0x00;  // weak
    Loaded l(makeHfe(1, 2, 250, {side, side, side, repeat(0x92, 400)}));
    l.load(1);
    // A write over head 0's weak run, round the end of the track and back to the start.
    std::vector<uint16_t> cells(110, 0x4A4A);
    CHECK(l.loader.writeCells(1, 0, 95, cells.data(), uint32_t(cells.size())));
    CHECK_EQ(l.loader.weakRuns(), 0u);  // written: real flux now
    std::vector<uint16_t> h = l.head(0);
    CHECK(h[95] == 0x4A4A && h[199] == 0x4A4A && h[4] == 0x4A4A && h[5] == 0x9292);
    // And into the file, through the disk (as BurstWriter does: the disk first).
    std::vector<uint8_t> bytes(4);
    for (int i = 0; i < 4; ++i) bytes[i] = 0x4A;
    CHECK(l.disk.writeTrackBytes(1, 0, 398, bytes.data(), 4));  // round the end of the side's 400 bytes
    std::vector<uint8_t> t(400);
    CHECK(l.disk.readTrack(1, 0, t.data()));
    CHECK(t[398] == 0x4A && t[399] == 0x4A && t[0] == 0x4A && t[1] == 0x4A && t[2] == 0x92);
    // A v3 file can't be written.
    Loaded v3(makeHfe(3, 1, 250, {repeat(0x92, 64), repeat(0x92, 64)}));
    CHECK(!v3.disk.writeTrackBytes(0, 0, 0, bytes.data(), 4));
}

}  // namespace

void runHfeTests() {
    RUN(test_hfe_header);
    RUN(test_hfe_v1_cells_and_length);
    RUN(test_hfe_v3_opcodes);
    RUN(test_hfe_v3_variable_rate_counted);
    RUN(test_hfe_weak_bits_are_noise_every_revolution);
    RUN(test_hfe_write_splices_cells);
}
