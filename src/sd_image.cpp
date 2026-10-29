#include "sd_image.h"

#include <Arduino.h>
#include <SPI.h>
#include <SdFat.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "amiga_track.h"
#include "img_cfg.h"
#include "settings.h"
#include "wav.h"

namespace sd_image {

static SdFs s_sd;
static ImageList s_images;
static PackList s_packs;
static int16_t* s_pool = nullptr;  // the loaded pack's samples
// IMG.CFG's text, kept from mount() for each open(): community files are a
// few KB.
constexpr size_t IMG_CFG_MAX = 8192;
static char* s_img_cfg = nullptr;
static size_t s_img_cfg_len = 0;

// An image's format: its IMG.CFG section if one matches, else its size.
// `report` prints why a matching section can't be used.
static bool formatOf(const char* name, uint32_t size, ibm::Format& out, bool report) {
    if (ImageList::adfName(name)) {  // an Amiga disk: DD (HD ADFs need a 150 RPM drive)
        if (size != amiga::ADF_BYTES) return false;
        out = ibm::amigaDD();
        return true;
    }
    if (s_img_cfg) {
        img_cfg::Match m = img_cfg::find(s_img_cfg, s_img_cfg_len, name, size, out);
        if (m.result == img_cfg::Result::Ok) return true;
        if (m.result != img_cfg::Result::NoMatch && report)
            Serial.printf("# IMG.CFG line %u, for %s: %s%s; going by its size\n", m.line, name,
                          m.result == img_cfg::Result::Unsupported ? "not supported yet: " : "", m.why);
    }
    return ibm::formatForSize(size, out);
}

// A drive's open image.
class ImageSource : public TrackSource {
public:
    const ibm::Format& format() const override { return fmt; }
    bool readTrack(uint8_t cylinder, uint8_t head, uint8_t* out) override {
        if (!hasTrack(fmt, cylinder, head)) return false;
        uint32_t n = fmt.trackDataBytes(cylinder, head);
        return file.seekSet(fmt.trackOffset(cylinder, head)) && file.read(out, n) == int(n);
    }
    // Written through at once, and synced: the card holds every sector the
    // host has been told is written, even if the power goes.
    bool writeSector(uint8_t cylinder, uint8_t head, uint8_t sector, const uint8_t* data) override {
        if (!writable_ || !hasTrack(fmt, cylinder, head) || sector >= fmt.track(cylinder, head).sectors) return false;
        uint32_t n = fmt.track(cylinder, head).sectorBytes();
        return file.seekSet(fmt.trackOffset(cylinder, head) + sector * n) && file.write(data, n) == n && file.sync();
    }
    bool writable() const override { return writable_; }

    FsFile file;
    char name[ImageList::NAME] = "";
    bool writable_ = false;
    ibm::Format fmt = ibm::HD_1440;
};

static ImageSource s_sources[bus::MAX_DRIVES];

bool mount() {
    // The BB48R's slot, in SPI mode on SPI1. SdFat drives the chip select.
    SPI1.setRX(cfg::PIN_SD_MISO);
    SPI1.setTX(cfg::PIN_SD_MOSI);
    SPI1.setSCK(cfg::PIN_SD_SCK);
    if (!s_sd.begin(SdSpiConfig(cfg::PIN_SD_CS, DEDICATED_SPI, SD_SCK_MHZ(cfg::SD_SPI_MHZ), &SPI1))) return false;
    // IMG.CFG, for images that aren't PC ones.
    std::free(s_img_cfg);
    s_img_cfg = nullptr;
    s_img_cfg_len = 0;
    FsFile cfg;
    if (cfg.open("/IMG.CFG", O_RDONLY) || cfg.open("/FF/IMG.CFG", O_RDONLY)) {
        size_t size = size_t(cfg.fileSize());
        if (size > IMG_CFG_MAX) Serial.printf("# IMG.CFG is over %u bytes: only the start is read\n", unsigned(IMG_CFG_MAX));
        size_t n = size < IMG_CFG_MAX ? size : IMG_CFG_MAX;
        s_img_cfg = static_cast<char*>(std::malloc(n ? n : 1));
        int got = s_img_cfg ? cfg.read(s_img_cfg, n) : -1;
        s_img_cfg_len = got > 0 ? size_t(got) : 0;
        cfg.close();
    }
    s_images.clear();
    FsFile root, f;
    if (!root.open("/")) return false;
    while (f.openNext(&root, O_RDONLY)) {
        char name[ImageList::NAME];
        ibm::Format fmt;
        if (!f.isDir() && f.getName(name, sizeof name) && ImageList::imageName(name) &&
            formatOf(name, uint32_t(f.fileSize()), fmt, true))
            s_images.add(name, uint32_t(f.fileSize()));
        f.close();
    }
    root.close();
    s_packs.clear();
    FsFile dir;
    if (dir.open("/samples")) {
        while (f.openNext(&dir, O_RDONLY)) {
            char name[PackList::NAME];
            if (f.isDir() && f.getName(name, sizeof name)) s_packs.add(name);
            f.close();
        }
        dir.close();
    }
    return true;
}

const PackList& packs() { return s_packs; }

size_t readSettings(char* out, size_t cap) {
    FsFile f;
    if (!f.open(settings::PATH, O_RDONLY)) return 0;
    int n = f.read(out, cap);
    f.close();
    return n > 0 ? size_t(n) : 0;
}

bool writeSettings(const char* text, size_t len) {
    FsFile f;
    if (!f.open(settings::PATH, O_WRONLY | O_CREAT | O_TRUNC)) return false;
    bool ok = f.write(text, len) == len;
    return f.close() && ok;
}

const ImageList& images() { return s_images; }

TrackSource* open(size_t index, unsigned drive) {
    if (index >= s_images.count() || drive >= bus::MAX_DRIVES) return nullptr;
    ImageSource& src = s_sources[drive];
    if (src.file.isOpen()) src.file.close();
    src.name[0] = 0;
    const char* name = s_images.name(index);
    if (!src.file.open(name, O_RDONLY)) return nullptr;
    // Writable unless the file is marked read-only (the card's way of
    // write-protecting a disk), or the other drive has it open.
    bool shared = false;
    for (unsigned d = 0; d < bus::MAX_DRIVES; ++d)
        shared = shared || (d != drive && s_sources[d].file.isOpen() && std::strcmp(s_sources[d].name, name) == 0);
    src.writable_ = false;
    if (!src.file.isReadOnly() && !shared) {
        src.file.close();
        src.writable_ = src.file.open(name, O_RDWR);
        if (!src.writable_ && !src.file.open(name, O_RDONLY)) return nullptr;
    }
    std::strcpy(src.name, name);
    if (!formatOf(name, s_images.size(index), src.fmt, false)) return nullptr;
    return &src;
}

bool loadPack(synth::Pack& pack, size_t index, size_t max_bytes) {
    std::free(s_pool);  // the last pack's samples
    s_pool = nullptr;
    pack = synth::Pack{};
    if (index >= s_packs.count()) return false;
    const char* best = s_packs.name(index);

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
    if (!pool) {
        pack = synth::Pack{};
        return false;
    }
    s_pool = pool;

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

const char* name(unsigned drive) { return drive < bus::MAX_DRIVES ? s_sources[drive].name : ""; }
bool contiguous(unsigned drive) { return drive < bus::MAX_DRIVES && s_sources[drive].file.isContiguous(); }

}  // namespace sd_image
