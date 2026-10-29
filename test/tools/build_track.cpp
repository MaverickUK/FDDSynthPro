// Writes the drive's MFM tracks to stdout, for the Python tests to compare
// with Greaseweazle and feed to the PIO simulator.
//
//   build_track FORMAT [image.img] > tracks.bin
//       Every track in order (cylinder 0 head 0, cylinder 0 head 1, ...;
//       one head's only, for a single-sided format), each track_bytes cell
//       words, big-endian (the first cell is the top bit of the first byte).
//   build_track FORMAT --cylinder N [image.img] > cylinder.bin
//       Cylinder N as the RDATA streamer is fed it (rdata_stream.h):
//       track_bytes 32-bit slot words, big-endian.
//
//   build_track hfe:FILE > tracks.bin
//       An HFE flux image's tracks as the drive serves them (hfe.h): every
//       track the same length, its cells as recorded (weak bits empty).
//
// FORMAT is 1440 or 720 (the PC formats), adf (an Amiga disk), size:BYTES (a raw image's format
// by its size: ibm::formatForSize), or cfg:IMG.CFG:NAME:BYTES (the IMG.CFG
// section for an image of that name and size). Without an image, the
// tracks hold the generated disk.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "gen_disk.h"
#include "hfe.h"
#include "ibm_track.h"
#include "img_cfg.h"
#include "rdata_stream.h"

static void put16(std::vector<uint8_t>& out, uint16_t w) {
    out.push_back(uint8_t(w >> 8));
    out.push_back(uint8_t(w));
}

static std::vector<uint8_t> readFile(const char* path) {
    std::vector<uint8_t> data;
    FILE* f = std::fopen(path, "rb");
    if (!f) return data;
    uint8_t buf[4096];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) data.insert(data.end(), buf, buf + n);
    std::fclose(f);
    return data;
}

static bool parseFormat(const char* arg, ibm::Format& fmt) {
    if (std::strcmp(arg, "1440") == 0) return fmt = ibm::HD_1440, true;
    if (std::strcmp(arg, "720") == 0) return fmt = ibm::DD_720, true;
    if (std::strcmp(arg, "adf") == 0) return fmt = ibm::amigaDD(), true;
    if (std::strncmp(arg, "size:", 5) == 0) return ibm::formatForSize(uint32_t(std::atol(arg + 5)), fmt);
    if (std::strncmp(arg, "cfg:", 4) == 0) {
        // cfg:PATH:NAME:BYTES
        std::string rest(arg + 4);
        size_t a = rest.find(':'), b = rest.rfind(':');
        if (a == std::string::npos || b == a) return false;
        std::vector<uint8_t> text = readFile(rest.substr(0, a).c_str());
        std::string name = rest.substr(a + 1, b - a - 1);
        img_cfg::Match m = img_cfg::find(reinterpret_cast<const char*>(text.data()), text.size(), name.c_str(),
                                         uint32_t(std::atol(rest.c_str() + b + 1)), fmt);
        if (m.result != img_cfg::Result::Ok) std::fprintf(stderr, "IMG.CFG: %s (line %u)\n", m.why, m.line);
        return m.result == img_cfg::Result::Ok;
    }
    return false;
}

static int hfeTracks(const char* path) {
    std::vector<uint8_t> bytes = readFile(path);
    hfe::MemoryFile file(bytes.data(), uint32_t(bytes.size()));
    hfe::Disk disk;
    if (const char* why = disk.open(file, false)) {
        std::fprintf(stderr, "%s: %s\n", path, why);
        return 1;
    }
    const ibm::Format& fmt = disk.format();
    std::vector<uint8_t> track(fmt.track_bytes * 2u);
    for (uint8_t c = 0; c < fmt.cyls; ++c)
        for (uint8_t h = 0; h < fmt.heads; ++h) {
            if (!disk.readTrack(c, h, track.data())) {
                std::fprintf(stderr, "track %u.%u unreadable\n", c, h);
                return 1;
            }
            std::fwrite(track.data(), 1, track.size(), stdout);
        }
    std::fprintf(stderr, "HFE v%u cyls=%u heads=%u rate=%u words=%u period_us=%u variable=%u\n", disk.header().version,
                 fmt.cyls, fmt.heads, unsigned(fmt.rate), unsigned(fmt.track_bytes), unsigned(fmt.periodUs()),
                 disk.variableRateTracks());
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 2 && std::strncmp(argv[1], "hfe:", 4) == 0) return hfeTracks(argv[1] + 4);
    const char* image_path = nullptr;
    int cylinder = -1;
    ibm::Format fmt;
    bool have_format = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--cylinder") == 0 && i + 1 < argc)
            cylinder = std::atoi(argv[++i]);
        else if (!have_format) {
            if (!parseFormat(argv[i], fmt)) {
                std::fprintf(stderr, "%s: not a format the drive knows\n", argv[i]);
                return 2;
            }
            have_format = true;
        } else
            image_path = argv[i];
    }
    if (!have_format || cylinder >= int(fmt.cyls)) {
        std::fprintf(stderr, "usage: build_track 1440|720|size:BYTES|cfg:IMG.CFG:NAME:BYTES [--cylinder N] "
                             "[image.img] > out.bin\n");
        return 2;
    }

    std::vector<uint8_t> image;
    if (image_path) {
        image = readFile(image_path);
        if (image.size() != fmt.imageBytes()) {
            std::fprintf(stderr, "%s: expected %u bytes, read %zu\n", image_path, unsigned(fmt.imageBytes()),
                         image.size());
            return 1;
        }
    }

    // One track's sector data, from the image or the generated disk.
    auto trackData = [&](unsigned cyl, unsigned head) {
        std::vector<uint8_t> data(ibm::MAX_TRACK_DATA);
        if (head >= fmt.heads) return data;
        if (image.empty())
            gen_disk::track(fmt, uint8_t(cyl), uint8_t(head), data.data());
        else
            std::memcpy(data.data(), &image[fmt.trackOffset(uint8_t(cyl), uint8_t(head))],
                        fmt.trackDataBytes(uint8_t(cyl), uint8_t(head)));
        return data;
    };

    std::vector<uint16_t> words(fmt.track_bytes);
    std::vector<uint8_t> out;
    if (cylinder >= 0) {
        std::vector<uint32_t> slots(fmt.track_bytes);
        if (!rdata::buildCylinder(fmt, uint8_t(cylinder), trackData(cylinder, 0).data(),
                                  trackData(cylinder, 1).data(), words.data(), slots.data())) {
            std::fprintf(stderr, "cylinder %d doesn't fit\n", cylinder);
            return 1;
        }
        for (uint32_t s : slots) {
            put16(out, uint16_t(s >> 16));
            put16(out, uint16_t(s));
        }
    } else {
        for (unsigned cyl = 0; cyl < fmt.cyls; ++cyl) {
            for (unsigned head = 0; head < fmt.heads; ++head) {
                if (!ibm::buildTrack(fmt, uint8_t(cyl), uint8_t(head), trackData(cyl, head).data(), words.data())) {
                    std::fprintf(stderr, "track %u.%u doesn't fit\n", cyl, head);
                    return 1;
                }
                for (uint16_t w : words) put16(out, w);
            }
        }
    }
    std::fwrite(out.data(), 1, out.size(), stdout);
    return 0;
}
