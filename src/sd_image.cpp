#include "sd_image.h"

#include <SdFat.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "wav.h"

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

bool loadPack(synth::Pack& pack, size_t max_bytes, char* pack_name, size_t name_len) {
    FsFile dir, entry;
    if (!dir.open("/samples")) return false;
    char best[ImageList::NAME] = "";
    while (entry.openNext(&dir, O_RDONLY)) {
        char name[ImageList::NAME];
        if (entry.isDir() && entry.getName(name, sizeof name) && (!best[0] || strcasecmp(name, best) < 0))
            std::strcpy(best, name);
        entry.close();
    }
    dir.close();
    if (!best[0]) return false;
    std::strncpy(pack_name, best, name_len - 1);
    pack_name[name_len - 1] = '\0';

    struct Sound {
        const char* file;
        synth::Sample* slot;
        uint32_t offset = 0;  // of the samples in the file
        size_t count = 0;     // samples to load (0: missing or unplayable)
    } sounds[] = {
        {"spinup", &pack.spinup}, {"spin", &pack.spin}, {"spindown", &pack.spindown}, {"step", &pack.step},
        {"seek", &pack.seek},     {"bump", &pack.bump}, {"insert", &pack.insert},     {"eject", &pack.eject},
    };
    auto path = [&](const Sound& s, char* out, size_t len) { std::snprintf(out, len, "/samples/%s/%s.wav", best, s.file); };

    // First pass: the headers, so the pool can be sized to the pack.
    static uint8_t header[cfg::WAV_HEADER_BYTES];
    pack = synth::Pack{};
    size_t total = 0;
    for (Sound& s : sounds) {
        char p[ImageList::NAME * 2 + 24];
        path(s, p, sizeof p);
        FsFile f;
        if (!f.open(p, O_RDONLY)) continue;
        int n = f.read(header, sizeof header);
        WavInfo info;
        if (n > 0 && parseWavHeader(header, size_t(n), uint32_t(f.fileSize()), info) == WavError::Ok &&
            wavIsPlayable(info) && (!pack.rate || info.sampleRate == pack.rate)) {
            s.offset = info.dataOffset;
            s.count = info.dataBytes / 2;
            if ((total + s.count) * 2 > max_bytes) s.count = max_bytes / 2 - total;  // cut short to fit
            total += s.count;
            pack.rate = info.sampleRate;
        }
        f.close();
    }
    if (!total) return false;
    int16_t* pool = static_cast<int16_t*>(std::malloc(total * 2));
    if (!pool) return false;

    // Second pass: the samples.
    size_t used = 0;
    for (Sound& s : sounds) {
        if (!s.count) continue;
        char p[ImageList::NAME * 2 + 24];
        path(s, p, sizeof p);
        FsFile f;
        if (f.open(p, O_RDONLY) && f.seekSet(s.offset) && f.read(pool + used, s.count * 2) == int(s.count * 2)) {
            *s.slot = synth::Sample{pool + used, uint32_t(s.count)};
            used += s.count;
        }
        f.close();
    }
    return pack.complete();
}

const char* name() { return s_name; }
bool contiguous() { return s_file.isContiguous(); }

}  // namespace sd_image
