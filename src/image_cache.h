// A whole disk image held in memory (the BB48R's PSRAM on the board), in
// front of the image on the SD card. Pure C++, host-tested.
//
// fill() copies the card's image in a track at a time, so core 0 stays
// responsive while it runs; the firmware fills it before the disk goes in
// (at power-on, and during a swap's ejected pause), so the drive only ever
// serves a cached disk and every track read is a copy from memory. Writes go
// to the card first and then into the cache, so the cache never holds data
// the card refused.
//
// Without room for the image (no PSRAM: capacity 0), it caches nothing and
// every read goes to the card, as before.
#pragma once

#include <cstddef>
#include <cstdint>

#include "track_source.h"

class ImageCache : public TrackSource {
public:
    static constexpr unsigned MAX_TRACKS = ibm::MAX_CYLINDERS * 2;

    // `buffer` holds `capacity` bytes; a 1.44 MB image needs 1,474,560.
    ImageCache(uint8_t* buffer, size_t capacity) : buffer_(buffer), capacity_(buffer ? capacity : 0) {}

    // Starts caching `backing`, forgetting whatever was cached before.
    void attach(TrackSource& backing);

    // Copies the next track in. Returns false once there's nothing left to
    // do: every track is cached, or skipped (a read error, or no room).
    bool fill();

    bool complete() const { return next_ >= MAX_TRACKS; }
    bool fits() const;  // the image fits: false means every read goes to the card
    unsigned cachedTracks() const;
    unsigned tracks() const;  // the image's
    bool cached(uint8_t cylinder, uint8_t head) const;

    const ibm::Format& format() const override { return backing_->format(); }
    bool readTrack(uint8_t cylinder, uint8_t head, uint8_t* out) override;
    bool writeSector(uint8_t cylinder, uint8_t head, uint8_t sector, const uint8_t* data) override;
    bool writeTrackBytes(uint8_t cylinder, uint8_t head, uint32_t at, const uint8_t* data, uint32_t n) override;
    bool writable() const override { return backing_->writable(); }

private:
    uint8_t* track(uint8_t cylinder, uint8_t head) { return buffer_ + format().trackOffset(cylinder, head); }

    uint8_t* buffer_;
    size_t capacity_;
    TrackSource* backing_ = nullptr;
    unsigned next_ = MAX_TRACKS;  // the next track fill() copies (cylinder * 2 + head)
    bool cached_[MAX_TRACKS] = {};
};
