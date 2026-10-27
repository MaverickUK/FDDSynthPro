// Tests for the image cache (image_cache.h): filling it from the card, reads
// served from memory, and writes that reach the card before the cache.
#include <cstdint>
#include <cstring>
#include <vector>

#include "check.h"
#include "image_cache.h"

namespace {  // this file's own helpers and tests

// The card's image: the generated disk in memory, counting reads, with a
// track that can fail to read and writes that can be refused.
class Card : public TrackSource {
public:
    explicit Card(const ibm::Format& fmt) : fmt_(fmt), image_(ImageCache::TRACKS * fmt.trackDataBytes()) {
        for (uint8_t c = 0; c < IMAGE_CYLINDERS; ++c)
            for (uint8_t h = 0; h < IMAGE_HEADS; ++h) gen_disk::track(fmt, c, h, at(c, h));
    }
    const ibm::Format& format() const override { return fmt_; }
    bool readTrack(uint8_t c, uint8_t h, uint8_t* out) override {
        ++reads;
        if (c >= IMAGE_CYLINDERS || (c == bad_cylinder && h == bad_head)) return false;
        std::memcpy(out, at(c, h), fmt_.trackDataBytes());
        return true;
    }
    bool writeSector(uint8_t c, uint8_t h, uint8_t s, const uint8_t* data) override {
        if (refuse_writes) return false;
        std::memcpy(at(c, h) + s * fmt_.sectorBytes(), data, fmt_.sectorBytes());
        return true;
    }
    bool writable() const override { return true; }
    uint8_t* at(uint8_t c, uint8_t h) { return &image_[imageTrackOffset(fmt_, c, h)]; }

    unsigned reads = 0;
    int bad_cylinder = -1, bad_head = -1;
    bool refuse_writes = false;

private:
    const ibm::Format& fmt_;
    std::vector<uint8_t> image_;
};

std::vector<uint8_t> readBack(TrackSource& s, uint8_t c, uint8_t h) {
    std::vector<uint8_t> out(s.format().trackDataBytes());
    if (!s.readTrack(c, h, out.data())) out.clear();
    return out;
}

void fillAll(ImageCache& cache) {
    unsigned steps = 0;
    while (cache.fill()) ++steps;
    CHECK(steps <= ImageCache::TRACKS);
}

TEST(test_cache_fills_a_track_at_a_time) {
    Card card(ibm::HD_1440);
    std::vector<uint8_t> ram(1474560);
    ImageCache cache(ram.data(), ram.size());
    cache.attach(card);
    CHECK(cache.fits());
    CHECK(!cache.complete());
    CHECK(cache.fill());
    CHECK_EQ(card.reads, 1u);  // one track per step: core 0 stays responsive
    CHECK(cache.cached(0, 0));
    CHECK(!cache.cached(0, 1));
    fillAll(cache);
    CHECK(cache.complete());
    CHECK_EQ(cache.cachedTracks(), ImageCache::TRACKS);
    CHECK_EQ(card.reads, ImageCache::TRACKS);
}

TEST(test_cache_reads_from_memory) {
    Card card(ibm::HD_1440);
    std::vector<uint8_t> ram(1474560);
    ImageCache cache(ram.data(), ram.size());
    cache.attach(card);
    fillAll(cache);
    std::vector<uint8_t> original(card.at(40, 1), card.at(40, 1) + 9216);
    unsigned reads = card.reads;
    card.at(40, 1)[0] ^= 0xFF;  // the card changes behind the cache's back
    CHECK(readBack(cache, 40, 1) == original);
    CHECK_EQ(card.reads, reads);  // never touched the card
    CHECK(readBack(cache, 82, 0).empty());  // beyond the image: still a blank track
}

TEST(test_cache_reads_the_card_until_filled) {
    Card card(ibm::HD_1440);
    std::vector<uint8_t> ram(1474560);
    ImageCache cache(ram.data(), ram.size());
    cache.attach(card);
    std::vector<uint8_t> want(card.at(79, 1), card.at(79, 1) + 9216);
    CHECK(readBack(cache, 79, 1) == want);
    CHECK_EQ(card.reads, 1u);
}

TEST(test_cache_writes_reach_the_card_first) {
    Card card(ibm::HD_1440);
    std::vector<uint8_t> ram(1474560);
    ImageCache cache(ram.data(), ram.size());
    cache.attach(card);
    fillAll(cache);
    std::vector<uint8_t> data(512, 0x5A);
    CHECK(cache.writeSector(12, 0, 3, data.data()));
    CHECK(std::memcmp(card.at(12, 0) + 3 * 512, data.data(), 512) == 0);
    CHECK(std::memcmp(readBack(cache, 12, 0).data() + 3 * 512, data.data(), 512) == 0);

    // A card that refuses: the write fails and the cache keeps the old data,
    // so a read-back shows what the card really holds.
    std::vector<uint8_t> before = readBack(cache, 12, 1);
    card.refuse_writes = true;
    std::vector<uint8_t> other(512, 0xA5);
    CHECK(!cache.writeSector(12, 1, 0, other.data()));
    CHECK(readBack(cache, 12, 1) == before);
}

TEST(test_cache_write_before_fill_is_kept) {
    // A write to a track not yet cached goes to the card; the fill then
    // copies the new data.
    Card card(ibm::HD_1440);
    std::vector<uint8_t> ram(1474560);
    ImageCache cache(ram.data(), ram.size());
    cache.attach(card);
    std::vector<uint8_t> data(512, 0x33);
    CHECK(cache.writeSector(70, 1, 17, data.data()));
    fillAll(cache);
    CHECK(std::memcmp(readBack(cache, 70, 1).data() + 17 * 512, data.data(), 512) == 0);
}

TEST(test_cache_skips_an_unreadable_track) {
    Card card(ibm::HD_1440);
    card.bad_cylinder = 7;
    card.bad_head = 1;
    std::vector<uint8_t> ram(1474560);
    ImageCache cache(ram.data(), ram.size());
    cache.attach(card);
    fillAll(cache);
    CHECK(cache.complete());
    CHECK_EQ(cache.cachedTracks(), ImageCache::TRACKS - 1);
    CHECK(!cache.cached(7, 1));
    CHECK(readBack(cache, 7, 1).empty());  // the card fails again: a blank track, as before
    CHECK(!readBack(cache, 7, 0).empty());
}

TEST(test_cache_without_room_reads_the_card) {
    // No PSRAM (a plain BB48): nothing cached, every read from the card.
    Card card(ibm::HD_1440);
    ImageCache none(nullptr, 0);
    none.attach(card);
    CHECK(!none.fits());
    CHECK(none.complete());
    CHECK(!none.fill());
    std::vector<uint8_t> want(card.at(3, 0), card.at(3, 0) + 9216);
    CHECK(readBack(none, 3, 0) == want);
    CHECK_EQ(card.reads, 1u);
    std::vector<uint8_t> data(512, 1);
    CHECK(none.writeSector(3, 0, 0, data.data()));
    CHECK(std::memcmp(card.at(3, 0), data.data(), 512) == 0);

    // Room for a 720 KB image but not a 1.44 MB one.
    std::vector<uint8_t> ram(737280);
    ImageCache small(ram.data(), ram.size());
    small.attach(card);
    CHECK(!small.fits());
    Card dd(ibm::DD_720);
    small.attach(dd);
    CHECK(small.fits());
    fillAll(small);
    CHECK_EQ(small.cachedTracks(), ImageCache::TRACKS);
}

TEST(test_cache_attach_starts_again) {
    Card a(ibm::HD_1440), b(ibm::DD_720);
    b.at(0, 0)[0] ^= 0xFF;
    std::vector<uint8_t> ram(1474560);
    ImageCache cache(ram.data(), ram.size());
    cache.attach(a);
    fillAll(cache);
    cache.attach(b);  // a swap: the new image's format, nothing cached yet
    CHECK(&cache.format() == &ibm::DD_720);
    CHECK_EQ(cache.cachedTracks(), 0u);
    fillAll(cache);
    std::vector<uint8_t> want(b.at(0, 0), b.at(0, 0) + 4608);
    CHECK(readBack(cache, 0, 0) == want);
}

}  // namespace

void runImageCacheTests() {
    RUN(test_cache_fills_a_track_at_a_time);
    RUN(test_cache_reads_from_memory);
    RUN(test_cache_reads_the_card_until_filled);
    RUN(test_cache_writes_reach_the_card_first);
    RUN(test_cache_write_before_fill_is_kept);
    RUN(test_cache_skips_an_unreadable_track);
    RUN(test_cache_without_room_reads_the_card);
    RUN(test_cache_attach_starts_again);
}
