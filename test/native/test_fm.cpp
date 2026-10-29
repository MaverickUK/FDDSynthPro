// Tests for FM (single density): writes decoded and placed, on an all-FM
// disk and on an FM boot track at half an MFM disk's rate, through the
// whole write path (BurstWriter, the loader, the disk). Greaseweazle checks
// the tracks themselves (test_track_encoder.py).
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "img_cfg.h"
#include "machine.h"
#include "rdata_stream.h"

namespace {  // this file's own helpers and tests

constexpr uint32_t TICKS = 16;  // capture ticks a cell at the disk's rate

// Flux intervals for cell words from a cell boundary, the host's clock
// `speed` (1.02: 2% slow) and each edge `jitter` cells off, rounded to ticks.
std::vector<uint8_t> flux(const uint16_t* words, size_t n, double speed, double jitter, uint32_t seed) {
    std::vector<uint8_t> out;
    double last = 0;
    uint32_t cell = 0;
    for (size_t w = 0; w < n; ++w)
        for (int b = 15; b >= 0; --b, ++cell) {
            if (!((words[w] >> b) & 1)) continue;
            seed = seed * 1664525u + 1013904223u;
            double edge = (cell + 1) * speed + jitter * ((seed >> 8) / double(1u << 24) * 2 - 1);
            double tick = double(uint64_t(edge * TICKS + 0.5));
            uint32_t ticks = uint32_t(tick - last);
            out.push_back(uint8_t(ticks - 2));  // as captured: less the edge ticks
            last = tick;
        }
    return out;
}

struct Rig {
    explicit Rig(const ibm::Format& fmt) : source(fmt) {}
    MemorySource source;
    std::vector<uint32_t> slots = std::vector<uint32_t>(ibm::MAX_TRACK_WORDS);
    std::vector<uint16_t> scratch = std::vector<uint16_t>(ibm::MAX_TRACK_WORDS);
    std::vector<uint8_t> d0 = std::vector<uint8_t>(ibm::MAX_TRACK_DATA), d1 = std::vector<uint8_t>(ibm::MAX_TRACK_DATA);
    CylinderLoader loader{source, slots.data(), scratch.data(), d0.data(), d1.data()};
    std::vector<uint8_t> store = std::vector<uint8_t>(12 * 1024);
    std::vector<mfm::Field> fields = std::vector<mfm::Field>(144);
    BurstWriter writer{source, loader, store.data(), store.size(), fields.data(), fields.size()};

    void load(uint8_t cyl) {
        loader.request(cyl);
        while (loader.service() != CylinderLoader::Step::Idle) {}
    }
    const ibm::Format& fmt() const { return source.format(); }

    // A track of `data` for (cyl, head), as the drive would send it.
    std::vector<uint16_t> track(uint8_t cyl, uint8_t head, const std::vector<uint8_t>& data) {
        std::vector<uint16_t> words(fmt().track_bytes);
        CHECK(ibm::buildTrack(fmt(), cyl, head, data.data(), words.data()));
        return words;
    }
};

std::vector<uint8_t> pattern(size_t n, uint32_t seed) {
    std::vector<uint8_t> out(n);
    for (uint8_t& b : out) b = uint8_t((seed = seed * 1103515245 + 12345) >> 16);
    return out;
}

ibm::Format fromCfg(const std::string& text, const char* name, uint32_t size) {
    ibm::Format f;
    CHECK(img_cfg::find(text.data(), text.size(), name, size, f).result == img_cfg::Result::Ok);
    return f;
}

const std::string OSBORNE = "[::102400]\ncyls = 40\nheads = 1\nsecs = 10\nbps = 256\ncskew = 2\nmode = fm\niam = no\n";

const std::string FLEX = R"([::733184]
cyls = 80
heads = 2
bps = 256
tracks = 0.0
  secs = 10
  mode = fm
  interleave = 4
  id = 1
tracks = 0.1
  secs = 10
  mode = fm
  interleave = 4
  hskew = 1
  id = 11
tracks = 1-79.0
  secs = 18
  mode = mfm
  interleave = 6
  id = 1
tracks = 1-79.1
  secs = 18
  mode = mfm
  interleave = 6
  hskew = 5
  id = 19
)";

// A whole track written (FORMAT, `gw write`): every sector to the disk and
// the live track.
void checkTrackWrite(Rig& r, uint8_t cyl, uint8_t head, uint32_t seed) {
    r.load(cyl);
    const ibm::Track& t = r.fmt().track(cyl, head);
    std::vector<uint8_t> data = pattern(t.dataBytes(), seed);
    std::vector<uint16_t> words = r.track(cyl, head, data);
    std::vector<uint8_t> bytes = flux(words.data(), words.size(), 1.02, 0.15, seed);
    unsigned before = r.source.writes;
    BurstResult res = r.writer.apply(WriteBurst{cyl, head, 0, bytes.data(), bytes.size()}, TICKS, 2);
    CHECK_EQ(res.decoded.bad_crc, 0u);
    CHECK_EQ(res.decoded.unplaced, 0u);
    CHECK_EQ(res.persisted, unsigned(t.sectors));
    CHECK_EQ(r.source.writes - before, unsigned(t.sectors));
    CHECK(std::memcmp(r.source.sector(cyl, head, 0), data.data(), data.size()) == 0);
}

// One sector written where the layout puts it (a controller's sector
// write: the data field only), found by its position.
void checkSectorWrite(Rig& r, uint8_t cyl, uint8_t head, uint8_t sector, uint32_t seed) {
    r.load(cyl);
    const ibm::Track& t = r.fmt().track(cyl, head);
    std::vector<uint8_t> data(t.dataBytes());
    std::memcpy(data.data(), r.source.sector(cyl, head, 0), data.size());
    std::vector<uint8_t> fresh = pattern(t.sectorBytes(), seed);
    std::memcpy(&data[sector * t.sectorBytes()], fresh.data(), fresh.size());
    std::vector<uint16_t> words = r.track(cyl, head, data);
    // Its slot, and the data field's words: zeros, mark, data, CRC, a gap byte.
    uint16_t order[ibm::MAX_SECTORS];
    t.sectorMap(cyl, head, order);
    uint16_t slot = 0;
    while (order[slot] != sector) ++slot;
    uint32_t start = t.dataFieldStart(slot);
    uint32_t len = (t.presyncBytes() + t.syncBytes() + t.sectorBytes() + 2 + 1) * t.scale;
    std::vector<uint8_t> bytes = flux(&words[start], len, 0.99, 0.1, seed);
    BurstResult res = r.writer.apply(WriteBurst{cyl, head, start, bytes.data(), bytes.size()}, TICKS, 2);
    CHECK_EQ(res.decoded.updates, 1u);
    CHECK_EQ(res.persisted, 1u);
    CHECK_EQ(res.patched, 1u);
    CHECK(std::memcmp(r.source.sector(cyl, head, sector), fresh.data(), fresh.size()) == 0);
}

TEST(test_fm_disk_writes) {
    // The Osborne 1: FM throughout, 125 kbit/s, no index mark, skewed.
    Rig r(fromCfg(OSBORNE, "OS1.IMG", 102400));
    checkTrackWrite(r, 0, 0, 1);
    checkTrackWrite(r, 39, 0, 2);
    checkSectorWrite(r, 12, 0, 7, 3);
    checkSectorWrite(r, 12, 0, 0, 4);
}

TEST(test_half_rate_fm_boot_track_writes) {
    // TSC Flex: an FM boot track at 125 kbit/s on a 250 kbit/s MFM disk,
    // each cell sent doubled. Writes come in at its own rate.
    Rig r(fromCfg(FLEX, "FLEX.IMG", 733184));
    CHECK_EQ(r.fmt().track(0, 1).scale, 2);
    checkTrackWrite(r, 0, 1, 5);
    checkSectorWrite(r, 0, 0, 9, 6);
    // ...and the MFM tracks around it as before.
    checkTrackWrite(r, 1, 1, 7);
    checkSectorWrite(r, 40, 0, 17, 8);
}

TEST(test_fm_scanner_ignores_mfm_syncs) {
    // An MFM track decoded as FM finds nothing (and the other way round):
    // the marks can't be mistaken.
    Rig r(fromCfg(FLEX, "FLEX.IMG", 733184));
    std::vector<uint8_t> data = pattern(r.fmt().track(5, 0).dataBytes(), 9);
    std::vector<uint16_t> words = r.track(5, 0, data);
    std::vector<uint8_t> store(12 * 1024);
    std::vector<mfm::Field> fields(144);
    mfm::FieldScanner scan(store.data(), store.size(), fields.data(), fields.size());
    scan.reset(256, true);
    for (uint16_t w : words)
        for (int b = 15; b >= 0; --b) scan.addCell((w >> b) & 1);
    CHECK_EQ(scan.count(), 0u);
}

}  // namespace

void runFmTests() {
    RUN(test_fm_disk_writes);
    RUN(test_half_rate_fm_boot_track_writes);
    RUN(test_fm_scanner_ignores_mfm_syncs);
}
