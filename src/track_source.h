// Where a disk's sector data comes from: the generated test disk, or an
// image file (sd_image.h on the board). Pure C++, host-tested.
//
// Images are raw sector dumps (IMG/IMA), in LBA order: cylinder, then head,
// then sector. The format follows from the file size.
#pragma once

#include <cstdint>

#include "gen_disk.h"
#include "ibm_track.h"

constexpr uint8_t IMAGE_CYLINDERS = 80;
constexpr uint8_t IMAGE_HEADS = 2;

// The format of a raw image of `bytes`, or nullptr if it isn't one we serve
// (1.44 MB and 720 KB for now).
inline const ibm::Format* formatForImageSize(uint32_t bytes) {
    const ibm::Format* formats[] = {&ibm::HD_1440, &ibm::DD_720};
    for (const ibm::Format* f : formats)
        if (bytes == uint32_t(IMAGE_CYLINDERS) * IMAGE_HEADS * f->trackDataBytes()) return f;
    return nullptr;
}

// Where a track's data starts in an image file.
inline uint32_t imageTrackOffset(const ibm::Format& fmt, uint8_t cylinder, uint8_t head) {
    return (uint32_t(cylinder) * IMAGE_HEADS + head) * fmt.trackDataBytes();
}

class TrackSource {
public:
    virtual ~TrackSource() = default;
    virtual const ibm::Format& format() const = 0;
    // Reads one track's sector data (format().trackDataBytes() bytes).
    // Returns false for a track the disk doesn't have (cylinders 80-83, or
    // a read error): the drive then sends a blank track.
    virtual bool readTrack(uint8_t cylinder, uint8_t head, uint8_t* out) = 0;
};

class GeneratedSource : public TrackSource {
public:
    explicit GeneratedSource(const ibm::Format& fmt) : fmt_(fmt) {}
    const ibm::Format& format() const override { return fmt_; }
    bool readTrack(uint8_t cylinder, uint8_t head, uint8_t* out) override {
        if (cylinder >= IMAGE_CYLINDERS || head >= IMAGE_HEADS) return false;
        gen_disk::track(fmt_, cylinder, head, out);
        return true;
    }

private:
    const ibm::Format& fmt_;
};
