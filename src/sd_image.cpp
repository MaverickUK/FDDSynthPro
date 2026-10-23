#include "sd_image.h"

#include <SdFat.h>
#include <strings.h>

#include <cstring>

#include "config.h"

namespace sd_image {

static SdFs s_sd;
static FsFile s_file;
static char s_name[64];
static bool s_writable = false;

class ImageSource : public TrackSource {
public:
    explicit ImageSource(const ibm::Format& fmt) : fmt_(fmt) {}
    const ibm::Format& format() const override { return fmt_; }
    bool readTrack(uint8_t cylinder, uint8_t head, uint8_t* out) override {
        if (cylinder >= IMAGE_CYLINDERS || head >= IMAGE_HEADS) return false;
        uint32_t n = fmt_.trackDataBytes();
        return s_file.seekSet(imageTrackOffset(fmt_, cylinder, head)) && s_file.read(out, n) == int(n);
    }
    // Written through at once, and synced: the card holds every sector the
    // host has been told is written, even if the power goes.
    bool writeSector(uint8_t cylinder, uint8_t head, uint8_t sector, const uint8_t* data) override {
        if (!s_writable || cylinder >= IMAGE_CYLINDERS || head >= IMAGE_HEADS || sector >= fmt_.sectors) return false;
        uint32_t n = fmt_.sectorBytes();
        return s_file.seekSet(imageTrackOffset(fmt_, cylinder, head) + sector * n) && s_file.write(data, n) == n &&
               s_file.sync();
    }
    bool writable() const override { return s_writable; }

private:
    const ibm::Format& fmt_;
};

static bool hasImageExtension(const char* name) {
    size_t n = std::strlen(name);
    return n > 4 && (strcasecmp(name + n - 4, ".img") == 0 || strcasecmp(name + n - 4, ".ima") == 0);
}

TrackSource* open() {
    if (!s_sd.begin(SdioConfig(cfg::PIN_SD_CLK, cfg::PIN_SD_CMD, cfg::PIN_SD_DAT0))) return nullptr;

    // The first image in name order: directory order on FAT is creation
    // order, which a user copying files can't easily control.
    FsFile root, f;
    if (!root.open("/")) return nullptr;
    char best[sizeof s_name] = "";
    while (f.openNext(&root, O_RDONLY)) {
        char name[sizeof s_name];
        if (!f.isDir() && f.getName(name, sizeof name) && hasImageExtension(name) &&
            formatForImageSize(uint32_t(f.fileSize())) && (!best[0] || strcasecmp(name, best) < 0))
            std::strcpy(best, name);
        f.close();
    }
    root.close();
    if (!best[0] || !s_file.open(best, O_RDONLY)) return nullptr;
    // Writable unless the file is marked read-only (the card's way of
    // write-protecting a disk).
    if (!s_file.isReadOnly()) {
        s_file.close();
        s_writable = s_file.open(best, O_RDWR);
        if (!s_writable && !s_file.open(best, O_RDONLY)) return nullptr;
    }

    std::strcpy(s_name, best);
    static ImageSource source(*formatForImageSize(uint32_t(s_file.fileSize())));
    return &source;
}

const char* name() { return s_name; }
bool contiguous() { return s_file.isContiguous(); }

}  // namespace sd_image
