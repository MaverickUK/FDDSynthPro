// The welcome disk: what the drive serves when it starts with no microSD
// card, or with no image on it it can use. A 1.44 MB DOS floppy, volume
// FDDSYNTHPRO, holding README.TXT (from welcome/README.TXT, embedded at
// build time by scripts/embed_welcome.py), so any computer that reads it
// shows how to set the drive up. As HDDSynthPro's welcome disk, on FAT12.
//
// Like the generated disk (gen_disk.h), every sector is computed when it's
// read, so the disk costs no storage: the boot sector, the FATs and the root
// folder from the layout below, the files' clusters from their text in
// flash, and zeros everywhere else. The firmware copies it into the image
// cache in PSRAM like any image, and writes stay there until the drive
// restarts. A PC that boots it is told it's not a system disk. Pure C++,
// host-tested.
#pragma once

#include <cstddef>
#include <cstdint>

#include "track_source.h"

namespace welcome_disk {

struct File {
    const char* name;  // 8.3, e.g. "README.TXT"
    const char* data;
    uint32_t size;     // bytes
};

// A 1.44 MB disk as DOS formats it: 512-byte sectors, one a cluster, two
// FATs of 9 sectors, 224 root entries.
constexpr uint32_t SECTOR_BYTES = 512;
constexpr uint32_t SECTORS = 2880;
constexpr uint32_t SECTORS_PER_TRACK = 18;
constexpr uint32_t HEADS = 2;
constexpr uint32_t RESERVED_SECTORS = 1;  // the boot sector
constexpr uint32_t FAT_COUNT = 2;
constexpr uint32_t FAT_SECTORS = 9;
constexpr uint32_t ROOT_ENTRIES = 224;
constexpr uint32_t ROOT_SECTORS = ROOT_ENTRIES * 32 / SECTOR_BYTES;  // 14
constexpr uint32_t FAT_START = RESERVED_SECTORS;
constexpr uint32_t ROOT_START = FAT_START + FAT_COUNT * FAT_SECTORS;  // 19
constexpr uint32_t DATA_START = ROOT_START + ROOT_SECTORS;            // 33: cluster 2
constexpr uint32_t CLUSTERS = SECTORS - DATA_START;                   // 2847
static_assert(CLUSTERS < 4085, "a FAT12 volume by its cluster count");
static_assert(FAT_SECTORS * SECTOR_BYTES * 2 / 3 >= CLUSTERS + 2, "the FAT has an entry for every cluster");

constexpr const char* VOLUME_LABEL = "FDDSYNTHPRO";  // 11 characters, the most FAT allows
constexpr uint32_t VOLUME_ID = 0x46445350;            // "FDSP"

// Sector `lba` of the disk holding `files` (one after another from cluster
// 2, each dated `date` in FAT's format) into `out`. Zeros past the end.
void sector(uint32_t lba, const File* files, size_t count, uint16_t date, uint8_t out[SECTOR_BYTES]);

}  // namespace welcome_disk

// The welcome disk as the drive serves it: a 1.44 MB disk whose sectors are
// computed. It takes writes (`keeps_writes`) only where something in front
// of it keeps them: the image cache in PSRAM. It forgets them itself.
class WelcomeSource : public TrackSource {
public:
    WelcomeSource(const welcome_disk::File* files, size_t count, uint16_t date)
        : files_(files), count_(count), date_(date) {}
    void keepsWrites(bool keeps) { keeps_writes_ = keeps; }

    const ibm::Format& format() const override { return ibm::HD_1440; }
    bool readTrack(uint8_t cylinder, uint8_t head, uint8_t* out) override {
        if (!hasTrack(format(), cylinder, head)) return false;
        uint32_t lba = (uint32_t(cylinder) * welcome_disk::HEADS + head) * welcome_disk::SECTORS_PER_TRACK;
        for (uint32_t s = 0; s < welcome_disk::SECTORS_PER_TRACK; ++s)
            welcome_disk::sector(lba + s, files_, count_, date_, out + s * welcome_disk::SECTOR_BYTES);
        return true;
    }
    bool writeSector(uint8_t, uint8_t, uint8_t, const uint8_t*) override { return keeps_writes_; }
    bool writable() const override { return keeps_writes_; }

private:
    const welcome_disk::File* files_;
    size_t count_;
    uint16_t date_;
    bool keeps_writes_ = false;
};
