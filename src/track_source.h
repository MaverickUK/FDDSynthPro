// Where a disk's sector data comes from: the generated test disk, or an
// image file (sd_image.h on the board). Pure C++, host-tested.
//
// Images are raw sector dumps (IMG/IMA), in the format's file order
// (ibm::Format::trackOffset: by default cylinder, then head, then sector).
// The format comes from IMG.CFG (img_cfg.h) or, without a match there,
// from the file's size (ibm::formatForSize).
#pragma once

#include <cstdint>

#include "gen_disk.h"
#include "ibm_track.h"

class TrackSource {
public:
    virtual ~TrackSource() = default;
    virtual const ibm::Format& format() const = 0;
    // Reads one track's sector data (format().trackDataBytes(cylinder, head)
    // bytes). `cylinder` is the image's (ibm::Format::imageCylinder).
    // Returns false for a track the disk doesn't have, or a read error: the
    // drive then sends a blank track.
    virtual bool readTrack(uint8_t cylinder, uint8_t head, uint8_t* out) = 0;
    // Writes one sector (index from 0) back to the disk. Returns false if it
    // can't keep it: a read-only image, or the generated disk.
    virtual bool writeSector(uint8_t cylinder, uint8_t head, uint8_t sector, const uint8_t* data) {
        (void)cylinder, (void)head, (void)sector, (void)data;
        return false;
    }
    virtual bool writable() const { return false; }
    // A flux image's write: `n` bytes of cells at byte `at` of the track's
    // (ibm::ENCODING_RAW). False if it can't keep them.
    virtual bool writeTrackBytes(uint8_t cylinder, uint8_t head, uint32_t at, const uint8_t* data, uint32_t n) {
        (void)cylinder, (void)head, (void)at, (void)data, (void)n;
        return false;
    }
};

// The disk in the drive: whichever source is inserted. The loader and the
// write path read and write through it, so swapping disks changes what's
// under them without rebuilding either.
class DiskSlot : public TrackSource {
public:
    explicit DiskSlot(TrackSource& disk) : disk_(&disk) {}
    void insert(TrackSource& disk) { disk_ = &disk; }
    TrackSource& disk() { return *disk_; }

    const ibm::Format& format() const override { return disk_->format(); }
    bool readTrack(uint8_t c, uint8_t h, uint8_t* out) override { return disk_->readTrack(c, h, out); }
    bool writeSector(uint8_t c, uint8_t h, uint8_t s, const uint8_t* data) override {
        return disk_->writeSector(c, h, s, data);
    }
    bool writeTrackBytes(uint8_t c, uint8_t h, uint32_t at, const uint8_t* data, uint32_t n) override {
        return disk_->writeTrackBytes(c, h, at, data, n);
    }
    bool writable() const override { return disk_->writable(); }

private:
    TrackSource* disk_;
};

// Whether `fmt` has a track at (cylinder, head): the image's cylinders and heads.
inline bool hasTrack(const ibm::Format& fmt, uint8_t cylinder, uint8_t head) {
    return cylinder < fmt.cyls && head < fmt.heads;
}

class GeneratedSource : public TrackSource {
public:
    explicit GeneratedSource(const ibm::Format& fmt) : fmt_(fmt) {}
    const ibm::Format& format() const override { return fmt_; }
    bool readTrack(uint8_t cylinder, uint8_t head, uint8_t* out) override {
        if (!hasTrack(fmt_, cylinder, head)) return false;
        gen_disk::track(fmt_, cylinder, head, out);
        return true;
    }

private:
    const ibm::Format& fmt_;
};
