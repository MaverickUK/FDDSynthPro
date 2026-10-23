// Writes the drive's MFM tracks for a whole disk to stdout, for the Python
// tests to compare with Greaseweazle (host/tests/test_track_encoder.py).
//
//   build_track 1440|720 [image.img] > tracks.bin
//
// Every track in IMG order (cylinder 0 head 0, cylinder 0 head 1, ...), each
// track_bytes cell words, big-endian (the first cell is the top bit of the
// first byte). Without an image, the tracks hold the generated disk.
#include <cstdio>
#include <cstring>
#include <vector>

#include "gen_disk.h"
#include "ibm_track.h"

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "usage: build_track 1440|720 [image.img] > tracks.bin\n");
        return 2;
    }
    const ibm::Format& fmt = std::strcmp(argv[1], "720") == 0 ? ibm::DD_720 : ibm::HD_1440;
    const unsigned cylinders = 80, heads = 2;

    std::vector<uint8_t> image;
    if (argc == 3) {
        FILE* f = std::fopen(argv[2], "rb");
        if (!f) {
            std::perror(argv[2]);
            return 1;
        }
        image.resize(size_t(cylinders) * heads * fmt.trackDataBytes());
        size_t got = std::fread(image.data(), 1, image.size(), f);
        std::fclose(f);
        if (got != image.size()) {
            std::fprintf(stderr, "%s: expected %zu bytes, read %zu\n", argv[2], image.size(), got);
            return 1;
        }
    }

    std::vector<uint8_t> data(fmt.trackDataBytes());
    std::vector<uint16_t> words(fmt.track_bytes);
    std::vector<uint8_t> out(size_t(fmt.track_bytes) * 2);
    for (unsigned cyl = 0; cyl < cylinders; ++cyl) {
        for (unsigned head = 0; head < heads; ++head) {
            if (image.empty())
                gen_disk::track(fmt, uint8_t(cyl), uint8_t(head), data.data());
            else
                std::memcpy(data.data(), &image[(cyl * heads + head) * fmt.trackDataBytes()], data.size());
            if (!ibm::buildTrack(fmt, uint8_t(cyl), uint8_t(head), data.data(), words.data())) {
                std::fprintf(stderr, "track %u.%u doesn't fit\n", cyl, head);
                return 1;
            }
            for (size_t i = 0; i < words.size(); ++i) {
                out[2 * i] = uint8_t(words[i] >> 8);
                out[2 * i + 1] = uint8_t(words[i]);
            }
            std::fwrite(out.data(), 1, out.size(), stdout);
        }
    }
    return 0;
}
