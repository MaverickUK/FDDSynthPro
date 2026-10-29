#include "image_cache.h"

#include <cstring>

void ImageCache::attach(TrackSource& backing) {
    backing_ = &backing;
    for (bool& c : cached_) c = false;
    next_ = fits() ? 0 : MAX_TRACKS;
}

bool ImageCache::fits() const { return backing_ && capacity_ >= backing_->format().imageBytes(); }

unsigned ImageCache::tracks() const { return backing_ ? unsigned(format().cyls) * format().heads : 0; }

bool ImageCache::fill() {
    // The image's tracks, cylinder by cylinder; slots for a head it doesn't
    // have are skipped.
    while (!complete()) {
        uint8_t cylinder = uint8_t(next_ / 2), head = uint8_t(next_ % 2);
        unsigned at = next_++;
        if (cylinder >= format().cyls) {
            next_ = MAX_TRACKS;
            return false;
        }
        if (head >= format().heads) continue;
        // A track the card can't read stays uncached: reads of it go to the
        // card, which fails again, and the drive sends a blank track as it
        // always has.
        cached_[at] = backing_->readTrack(cylinder, head, track(cylinder, head));
        return true;
    }
    return false;
}

unsigned ImageCache::cachedTracks() const {
    unsigned n = 0;
    for (bool c : cached_) n += c;
    return n;
}

bool ImageCache::cached(uint8_t cylinder, uint8_t head) const {
    return backing_ && hasTrack(format(), cylinder, head) && cached_[cylinder * 2 + head];
}

bool ImageCache::readTrack(uint8_t cylinder, uint8_t head, uint8_t* out) {
    if (!cached(cylinder, head)) return backing_->readTrack(cylinder, head, out);
    std::memcpy(out, track(cylinder, head), format().trackDataBytes(cylinder, head));
    return true;
}

bool ImageCache::writeSector(uint8_t cylinder, uint8_t head, uint8_t sector, const uint8_t* data) {
    // The card first: if it refuses, the cache keeps the old data, so a
    // read-back shows what the card really holds.
    if (!backing_->writeSector(cylinder, head, sector, data)) return false;
    uint32_t size = format().track(cylinder, head).sectorBytes();
    if (cached(cylinder, head)) std::memcpy(track(cylinder, head) + sector * size, data, size);
    return true;
}

bool ImageCache::writeTrackBytes(uint8_t cylinder, uint8_t head, uint32_t at, const uint8_t* data, uint32_t n) {
    // As writeSector: the card first.
    if (!backing_->writeTrackBytes(cylinder, head, at, data, n)) return false;
    uint32_t size = format().trackDataBytes(cylinder, head);
    if (cached(cylinder, head) && at < size) std::memcpy(track(cylinder, head) + at, data, at + n <= size ? n : size - at);
    return true;
}
