#include "image_cache.h"

#include <cstring>

void ImageCache::attach(TrackSource& backing) {
    backing_ = &backing;
    for (bool& c : cached_) c = false;
    next_ = fits() ? 0 : TRACKS;
}

bool ImageCache::fits() const {
    return backing_ && capacity_ >= size_t(TRACKS) * backing_->format().trackDataBytes();
}

bool ImageCache::fill() {
    if (complete()) return false;
    uint8_t cylinder = uint8_t(next_ / IMAGE_HEADS), head = uint8_t(next_ % IMAGE_HEADS);
    // A track the card can't read stays uncached: reads of it go to the card,
    // which fails again, and the drive sends a blank track as it always has.
    cached_[next_] = backing_->readTrack(cylinder, head, track(cylinder, head));
    ++next_;
    return true;
}

unsigned ImageCache::cachedTracks() const {
    unsigned n = 0;
    for (bool c : cached_) n += c;
    return n;
}

bool ImageCache::cached(uint8_t cylinder, uint8_t head) const {
    return cylinder < IMAGE_CYLINDERS && head < IMAGE_HEADS && cached_[cylinder * IMAGE_HEADS + head];
}

bool ImageCache::readTrack(uint8_t cylinder, uint8_t head, uint8_t* out) {
    if (!cached(cylinder, head)) return backing_->readTrack(cylinder, head, out);
    std::memcpy(out, track(cylinder, head), format().trackDataBytes());
    return true;
}

bool ImageCache::writeSector(uint8_t cylinder, uint8_t head, uint8_t sector, const uint8_t* data) {
    // The card first: if it refuses, the cache keeps the old data, so a
    // read-back shows what the card really holds.
    if (!backing_->writeSector(cylinder, head, sector, data)) return false;
    if (cached(cylinder, head))
        std::memcpy(track(cylinder, head) + sector * format().sectorBytes(), data, format().sectorBytes());
    return true;
}
