// Tests for disk geometry (src/ibm_track.*: formats, layouts, file order)
// and FlashFloppy's IMG.CFG (src/img_cfg.*), using sections from
// FlashFloppy's own example files (examples/Host/*/IMG.CFG). The tracks
// these formats build are checked against Greaseweazle's encoder in
// host/tests/test_track_encoder.py.
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "check.h"
#include "ibm_track.h"
#include "img_cfg.h"
#include "track_source.h"

namespace {  // this file's own helpers and tests

img_cfg::Match find(const std::string& text, const char* name, uint32_t size, ibm::Format& out) {
    return img_cfg::find(text.data(), text.size(), name, size, out);
}

// ---- Geometry --------------------------------------------------------------------------

TEST(test_pc_formats_by_size) {
    struct Want {
        uint32_t bytes;
        uint8_t cyls, heads, sectors;
        uint32_t rate;
        uint16_t rpm, words;
    } wants[] = {
        {163840, 40, 1, 8, 250000, 300, 6250},   {184320, 40, 1, 9, 250000, 300, 6250},
        {327680, 40, 2, 8, 250000, 300, 6250},   {368640, 40, 2, 9, 250000, 300, 6250},
        {737280, 80, 2, 9, 250000, 300, 6250},   {819200, 80, 2, 10, 250000, 300, 6250},
        {1228800, 80, 2, 15, 500000, 360, 10416}, {1474560, 80, 2, 18, 500000, 300, 12500},
        {1720320, 80, 2, 21, 500000, 300, 12500},
    };
    for (const Want& w : wants) {
        ibm::Format f;
        CHECK(ibm::formatForSize(w.bytes, f));
        CHECK_EQ(f.cyls, w.cyls);
        CHECK_EQ(f.heads, w.heads);
        CHECK_EQ(f.kinds[0].sectors, w.sectors);
        CHECK_EQ(f.rate, w.rate);
        CHECK_EQ(f.rpm, w.rpm);
        CHECK_EQ(f.track_bytes, w.words);
        CHECK_EQ(f.imageBytes(), w.bytes);
        CHECK(f.kinds[0].usedBytes() <= f.track_bytes);
    }
    ibm::Format f;
    CHECK(ibm::formatForSize(1228800, f));
    CHECK_EQ(f.periodUs(), 166666u);  // 360 RPM
    CHECK(ibm::formatForSize(1720320, f));  // DMF: interleave 2, skew 3, gap 3 of 12
    CHECK_EQ(f.kinds[0].interleave, 2);
    CHECK_EQ(f.kinds[0].cskew, 3);
    CHECK_EQ(f.kinds[0].gap3, 12);
}

TEST(test_auto_gaps_as_greaseweazle) {
    // ibm.1440 without its gap3: auto gives the standard 84 (it fits).
    ibm::TrackSpec s;
    s.sectors = 18;
    s.size_code = 2;
    ibm::Track t;
    CHECK(ibm::resolve(s, 500000, 300, t));
    CHECK_EQ(t.gap3, 84);
    CHECK_EQ(t.gap4a, 80);
    // 11 x 1024 at 500 kbit/s (Bung MGD2): gap 3 as much as fits, up to 116.
    s.sectors = 11;
    s.size_code = 3;
    CHECK(ibm::resolve(s, 500000, 300, t));
    CHECK(t.gap3 < 116);
    CHECK(t.usedBytes() <= 12500);
    // The auto rate: 9 x 512 is DD, 18 x 512 HD, 36 x 512 ED.
    s.size_code = 2;
    s.sectors = 9;
    CHECK_EQ(ibm::autoRate(s, 300), 250000u);
    s.sectors = 18;
    CHECK_EQ(ibm::autoRate(s, 300), 500000u);
    s.sectors = 36;
    CHECK_EQ(ibm::autoRate(s, 300), 1000000u);
    // ...which the streamer can't send.
    ibm::Format f;
    CHECK(!ibm::uniformFormat(80, 2, s, 0, 300, f));
}

TEST(test_sector_map) {
    // Greaseweazle's sec_map, worked by hand: 9 sectors, interleave 2.
    ibm::Track t;
    t.sectors = 9;
    t.interleave = 2;
    uint16_t m[9];
    t.sectorMap(0, 0, m);
    const uint16_t want[9] = {0, 5, 1, 6, 2, 7, 3, 8, 4};
    CHECK(std::memcmp(m, want, sizeof m) == 0);
    // With skew, every track is still each sector once.
    t.sectors = 21;
    t.cskew = 3;
    t.hskew = 1;
    uint16_t big[21];
    for (uint8_t c = 0; c < 5; ++c) {
        t.sectorMap(c, 1, big);
        std::set<uint16_t> seen(big, big + 21);
        CHECK_EQ(seen.size(), 21u);
    }
    t.sectorMap(1, 0, big);
    CHECK_EQ(big[3], 0);  // cylinder 1 starts 3 slots on
}

TEST(test_file_layouts) {
    ibm::Format f = ibm::DD_720;  // 4,608 bytes a track
    const uint32_t t = 4608;
    CHECK_EQ(f.trackOffset(1, 1), 3 * t);
    f.file_layout = ibm::LAYOUT_SEQUENTIAL;
    CHECK_EQ(f.trackOffset(1, 0), 1 * t);
    CHECK_EQ(f.trackOffset(0, 1), 80 * t);
    f.file_layout = ibm::LAYOUT_SEQUENTIAL | ibm::LAYOUT_REVERSE_SIDE1;
    CHECK_EQ(f.trackOffset(79, 1), 80 * t);
    CHECK_EQ(f.trackOffset(0, 1), 159 * t);
    f.file_layout = ibm::LAYOUT_SIDES_SWAPPED;
    CHECK_EQ(f.trackOffset(0, 1), 0u);
    CHECK_EQ(f.trackOffset(0, 0), t);
    CHECK_EQ(f.imageBytes(), 737280u);
}

TEST(test_double_step_and_one_head) {
    ibm::Format f;
    ibm::TrackSpec s;
    s.sectors = 9;
    s.size_code = 2;
    CHECK(ibm::uniformFormat(40, 1, s, 250000, 300, f));
    f.step = 2;
    CHECK_EQ(f.imageCylinder(0), 0);
    CHECK_EQ(f.imageCylinder(1), -1);  // between double steps
    CHECK_EQ(f.imageCylinder(78), 39);
    CHECK_EQ(f.imageCylinder(80), -1);
    CHECK_EQ(f.lastPhysicalCylinder(), 79u);
    CHECK(hasTrack(f, 39, 0));
    CHECK(!hasTrack(f, 39, 1));  // one head
    CHECK_EQ(f.imageBytes(), 40u * 9 * 512);
}

// ---- IMG.CFG ---------------------------------------------------------------------------

const std::string ROLAND = R"(## IMG.CFG for faster loading of Roland OS and System disks.
# Matches 720kB images named *.os.{img,ima,dsk}.
[os::737280]
cyls = 80
heads = 2
secs = 9
bps = 512
tracks = 0-79 # This line can be adjusted
  interleave = 2
  hskew = 1
  cskew = 2

[mc::737280]
cyls = 80
heads = 2
secs = 9
bps = 512
tracks = 0-79
  interleave = 4

# Roland 1.44MB format may apply sector skew.
[::1474560]
cyls = 80
heads = 2
secs = 18
bps = 512
hskew = 3
cskew = 6
)";

TEST(test_img_cfg_tags) {
    char tag[32];
    img_cfg::imageTag("S330.OS.IMG", tag, sizeof tag);
    CHECK_STR(tag, "os");
    img_cfg::imageTag("GAMES.IMG", tag, sizeof tag);
    CHECK_STR(tag, "");
    img_cfg::imageTag("noext", tag, sizeof tag);
    CHECK_STR(tag, "");
}

TEST(test_img_cfg_roland) {
    ibm::Format f;
    // A tagged 720 KB image: the tag's section, interleave on every track.
    img_cfg::Match m = find(ROLAND, "S330.os.img", 737280, f);
    CHECK(m.result == img_cfg::Result::Ok);
    CHECK_EQ(f.rate, 250000u);  // auto: 9 x 512 is DD
    CHECK_EQ(f.track(40, 1).interleave, 2);
    CHECK_EQ(f.track(40, 1).cskew, 2);
    CHECK_EQ(f.track(40, 1).hskew, 1);
    CHECK_EQ(f.track(40, 1).gap3, 84);  // auto
    CHECK(find(ROLAND, "MC500.MC.IMA", 737280, f).result == img_cfg::Result::Ok);
    CHECK_EQ(f.track(0, 0).interleave, 4);
    // The wrong size for its tag's section: none matches.
    CHECK(find(ROLAND, "S330.os.img", 1474560, f).result == img_cfg::Result::NoMatch);
    // Untagged, or a tag no section has: the [::size] section.
    CHECK(find(ROLAND, "MT200.IMG", 1474560, f).result == img_cfg::Result::Ok);
    CHECK_EQ(f.track(0, 0).cskew, 6);
    CHECK_EQ(f.rate, 500000u);
    CHECK(find(ROLAND, "MT200.other.img", 1474560, f).result == img_cfg::Result::Ok);
    CHECK(find(ROLAND, "GAMES.IMG", 737280, f).result == img_cfg::Result::NoMatch);  // by its size, then
}

TEST(test_img_cfg_per_head_ids) {
    // Kaypro and GRiD: head 1's sectors carry on from head 0's IDs.
    const std::string KAYPRO = R"(# 400kB: DSDD, 40 cyl
[::409600]
cyls = 40
heads = 2
secs = 10
bps = 512
interleave = 3
h = 0
tracks = 0-39.0
  id = 0
tracks = 0-39.1
  id = 10
)";
    ibm::Format f;
    CHECK(find(KAYPRO, "CPM.IMG", 409600, f).result == img_cfg::Result::Ok);
    CHECK_EQ(f.cyls, 40);
    CHECK_EQ(f.track(5, 0).first_id, 0);
    CHECK_EQ(f.track(5, 1).first_id, 10);
    CHECK_EQ(f.track(5, 1).head_id, 0);  // h = 0 on both heads
    CHECK_EQ(f.track(5, 1).interleave, 3);
    CHECK_EQ(f.imageBytes(), 409600u);
}

TEST(test_img_cfg_track_sizes_differ) {
    // Bung MGD2: cylinder 0 is 18 x 512, the rest 11 x 1024.
    const std::string BUNG = R"(# Images are 1756kB exactly.
[::1798144]
cyls = 80
heads = 2
tracks = 0
  secs = 18
  bps = 512
tracks = 1-79
  secs = 11
  bps = 1024
  gap3 = 30
)";
    ibm::Format f;
    img_cfg::Match m = find(BUNG, "GAME.IMG", 1798144, f);
    CHECK(m.result == img_cfg::Result::Ok);
    CHECK_EQ(f.imageBytes(), 1798144u);
    CHECK_EQ(f.trackDataBytes(0, 1), 9216u);
    CHECK_EQ(f.trackDataBytes(1, 0), 11264u);
    CHECK_EQ(f.trackOffset(1, 0), 2u * 9216);
    CHECK_EQ(f.trackOffset(2, 1), 2u * 9216 + 3 * 11264);
    CHECK_EQ(f.track(50, 1).gap3, 30);
    CHECK_EQ(f.rate, 500000u);
    CHECK_EQ(f.maxTrackDataBytes(), 11264u);
}

TEST(test_img_cfg_ensoniq) {
    const std::string ENSONIQ = R"(# Ensoniq 1.6MB DSHD
[::1638400]
cyls = 80
heads = 2
secs = 20
bps = 512
gap3 = 40
id = 0
rate = 500

# Ensoniq 5.1MB DSHD "SuperDisk"
[::5222400]
cyls = 255
heads = 2
secs = 20
bps = 512
gap3 = 40
id = 0
rate = 500

# Ensoniq Mirage 440kB SSDD
[::450560]
cyls = 80
heads = 1
secs = 6
bps = 1024,1024,1024,1024,1024,512
id = 0
rate = 250
)";
    ibm::Format f;
    CHECK(find(ENSONIQ, "EPS.IMG", 1638400, f).result == img_cfg::Result::Ok);
    CHECK_EQ(f.track(3, 0).first_id, 0);
    CHECK_EQ(f.imageBytes(), 1638400u);
    CHECK(find(ENSONIQ, "SUPER.IMG", 5222400, f).result == img_cfg::Result::Ok);
    CHECK_EQ(f.cyls, 255);
    CHECK_EQ(f.lastPhysicalCylinder(), 254u);
    img_cfg::Match m = find(ENSONIQ, "MIRAGE.IMG", 450560, f);
    CHECK(m.result == img_cfg::Result::Unsupported);  // mixed sector sizes
    CHECK_EQ(m.line, 26u);
}

TEST(test_img_cfg_refuses_what_it_cant_do) {
    ibm::Format f;
    // FM recording (CompuPro's boot track, the Sinclair QL...).
    CHECK(find("[::204800]\ncyls=40\nheads=2\nsecs=10\nbps=256\nmode = fm\n", "A.IMG", 204800, f).result ==
          img_cfg::Result::Unsupported);
    // A different rate on some tracks.
    CHECK(find("[]\ncyls=80\nheads=2\nsecs=9\nbps=512\nrate=250\ntracks=0\n rate=500\n", "A.IMG", 1, f).result ==
          img_cfg::Result::Unsupported);
    // Mistakes: no cyls, no bps, a disk parameter in a tracks section, a bad
    // list, a typo, and a layout too long for a track.
    CHECK(find("[]\nheads=2\nsecs=9\nbps=512\n", "A.IMG", 1, f).result == img_cfg::Result::Bad);
    CHECK(find("[]\ncyls=80\nheads=2\nsecs=9\n", "A.IMG", 1, f).result == img_cfg::Result::Bad);
    CHECK(find("[]\ncyls=80\nheads=2\nsecs=9\nbps=512\ntracks=0\n cyls=40\n", "A.IMG", 1, f).result ==
          img_cfg::Result::Bad);
    CHECK(find("[]\ncyls=80\nheads=2\nsecs=9\nbps=512\ntracks=3-1\n id=0\n", "A.IMG", 1, f).result ==
          img_cfg::Result::Bad);
    CHECK(find("[]\ncyls=80\nheads=2\nsecz=9\n", "A.IMG", 1, f).result == img_cfg::Result::Bad);
    CHECK(find("[]\ncyls=80\nheads=2\nsecs=30\nbps=512\nrate=500\n", "A.IMG", 1, f).result == img_cfg::Result::Bad);
    // An unmatched section's mistakes don't matter.
    CHECK(find("[other]\nnonsense\n[]\ncyls=80\nheads=2\nsecs=9\nbps=512\n", "A.IMG", 1, f).result ==
          img_cfg::Result::Ok);
}

TEST(test_img_cfg_garbage_never_crashes) {
    uint32_t seed = 11;
    for (int round = 0; round < 300; ++round) {
        std::string text;
        for (int i = 0; i < 200; ++i) {
            seed = seed * 1103515245 + 12345;
            const char pick[] = "[]:=\n# .-,0123456789cylsheadsecbpstrackidratemfmauto";
            text += (seed >> 16) % 4 ? pick[(seed >> 8) % (sizeof pick - 1)] : char(seed >> 24);
        }
        ibm::Format f;
        img_cfg::Match m = find(text, "X.IMG", 737280, f);
        if (m.result == img_cfg::Result::Ok) CHECK(f.finish());
    }
}

}  // namespace

void runImgCfgTests() {
    RUN(test_pc_formats_by_size);
    RUN(test_auto_gaps_as_greaseweazle);
    RUN(test_sector_map);
    RUN(test_file_layouts);
    RUN(test_double_step_and_one_head);
    RUN(test_img_cfg_tags);
    RUN(test_img_cfg_roland);
    RUN(test_img_cfg_per_head_ids);
    RUN(test_img_cfg_track_sizes_differ);
    RUN(test_img_cfg_ensoniq);
    RUN(test_img_cfg_refuses_what_it_cant_do);
    RUN(test_img_cfg_garbage_never_crashes);
}
