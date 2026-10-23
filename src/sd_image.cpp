#include "sd_image.h"

#include <SdFat.h>

#include <cstring>

#include "config.h"

namespace sd_image {

static SdFs s_sd;
static FsFile s_file;
static ImageList s_images;
static char s_name[ImageList::NAME];
static bool s_writable = false;

class ImageSource : public TrackSource {
public:
    void use(const ibm::Format& fmt) { fmt_ = &fmt; }
    const ibm::Format& format() const override { return *fmt_; }
    bool readTrack(uint8_t cylinder, uint8_t head, uint8_t* out) override {
        if (cylinder >= IMAGE_CYLINDERS || head >= IMAGE_HEADS) return false;
        uint32_t n = fmt_->trackDataBytes();
        return s_file.seekSet(imageTrackOffset(*fmt_, cylinder, head)) && s_file.read(out, n) == int(n);
    }
    // Written through at once, and synced: the card holds every sector the
    // host has been told is written, even if the power goes.
    bool writeSector(uint8_t cylinder, uint8_t head, uint8_t sector, const uint8_t* data) override {
        if (!s_writable || cylinder >= IMAGE_CYLINDERS || head >= IMAGE_HEADS || sector >= fmt_->sectors)
            return false;
        uint32_t n = fmt_->sectorBytes();
        return s_file.seekSet(imageTrackOffset(*fmt_, cylinder, head) + sector * n) && s_file.write(data, n) == n &&
               s_file.sync();
    }
    bool writable() const override { return s_writable; }

private:
    const ibm::Format* fmt_ = &ibm::HD_1440;
};

static ImageSource s_source;

bool mount() {
    if (!s_sd.begin(SdioConfig(cfg::PIN_SD_CLK, cfg::PIN_SD_CMD, cfg::PIN_SD_DAT0))) return false;
    s_images.clear();
    FsFile root, f;
    if (!root.open("/")) return false;
    while (f.openNext(&root, O_RDONLY)) {
        char name[ImageList::NAME];
        if (!f.isDir() && f.getName(name, sizeof name)) s_images.add(name, uint32_t(f.fileSize()));
        f.close();
    }
    root.close();
    return true;
}

const ImageList& images() { return s_images; }

TrackSource* open(size_t index) {
    if (index >= s_images.count()) return nullptr;
    if (s_file.isOpen()) s_file.close();
    const char* name = s_images.name(index);
    if (!s_file.open(name, O_RDONLY)) return nullptr;
    // Writable unless the file is marked read-only (the card's way of
    // write-protecting a disk).
    s_writable = false;
    if (!s_file.isReadOnly()) {
        s_file.close();
        s_writable = s_file.open(name, O_RDWR);
        if (!s_writable && !s_file.open(name, O_RDONLY)) return nullptr;
    }
    std::strcpy(s_name, name);
    s_source.use(*formatForImageSize(s_images.size(index)));
    return &s_source;
}

const char* name() { return s_name; }
bool contiguous() { return s_file.isContiguous(); }

}  // namespace sd_image
