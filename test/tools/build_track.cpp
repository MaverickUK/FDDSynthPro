// Writes the drive's MFM tracks to stdout, for the Python tests to compare
// with Greaseweazle and feed to the PIO simulator.
//
//   build_track 1440|720 [image.img] > tracks.bin
//       Every track in IMG order (cylinder 0 head 0, cylinder 0 head 1, ...),
//       each track_bytes cell words, big-endian (the first cell is the top
//       bit of the first byte).
//   build_track 1440|720 --cylinder N [image.img] > cylinder.bin
//       Cylinder N as the RDATA streamer is fed it (rdata_stream.h):
//       track_bytes 32-bit slot words, big-endian.
//
// Without an image, the tracks hold the generated disk.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "gen_disk.h"
#include "ibm_track.h"
#include "rdata_stream.h"

static const unsigned CYLINDERS = 80, HEADS = 2;

static void put16(std::vector<uint8_t>& out, uint16_t w) {
    out.push_back(uint8_t(w >> 8));
    out.push_back(uint8_t(w));
}

int main(int argc, char** argv) {
    const char* image_path = nullptr;
    int cylinder = -1;
    const ibm::Format* fmt = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--cylinder") == 0 && i + 1 < argc)
            cylinder = std::atoi(argv[++i]);
        else if (!fmt)
            fmt = std::strcmp(argv[i], "720") == 0 ? &ibm::DD_720 : &ibm::HD_1440;
        else
            image_path = argv[i];
    }
    if (!fmt || cylinder >= int(CYLINDERS)) {
        std::fprintf(stderr, "usage: build_track 1440|720 [--cylinder N] [image.img] > out.bin\n");
        return 2;
    }

    std::vector<uint8_t> image;
    if (image_path) {
        FILE* f = std::fopen(image_path, "rb");
        if (!f) {
            std::perror(image_path);
            return 1;
        }
        image.resize(size_t(CYLINDERS) * HEADS * fmt->trackDataBytes());
        size_t got = std::fread(image.data(), 1, image.size(), f);
        std::fclose(f);
        if (got != image.size()) {
            std::fprintf(stderr, "%s: expected %zu bytes, read %zu\n", image_path, image.size(), got);
            return 1;
        }
    }

    // One track's sector data, from the image or the generated disk.
    auto trackData = [&](unsigned cyl, unsigned head) {
        std::vector<uint8_t> data(fmt->trackDataBytes());
        if (image.empty())
            gen_disk::track(*fmt, uint8_t(cyl), uint8_t(head), data.data());
        else
            std::memcpy(data.data(), &image[(cyl * HEADS + head) * fmt->trackDataBytes()], data.size());
        return data;
    };

    std::vector<uint16_t> words(fmt->track_bytes);
    std::vector<uint8_t> out;
    if (cylinder >= 0) {
        std::vector<uint32_t> slots(fmt->track_bytes);
        if (!rdata::buildCylinder(*fmt, uint8_t(cylinder), trackData(cylinder, 0).data(),
                                  trackData(cylinder, 1).data(), words.data(), slots.data())) {
            std::fprintf(stderr, "cylinder %d doesn't fit\n", cylinder);
            return 1;
        }
        for (uint32_t s : slots) {
            put16(out, uint16_t(s >> 16));
            put16(out, uint16_t(s));
        }
    } else {
        for (unsigned cyl = 0; cyl < CYLINDERS; ++cyl) {
            for (unsigned head = 0; head < HEADS; ++head) {
                if (!ibm::buildTrack(*fmt, uint8_t(cyl), uint8_t(head), trackData(cyl, head).data(), words.data())) {
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
