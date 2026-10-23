// Decodes a burst of written flux the way the drive will, for the Python
// tests to check against Greaseweazle's encoder (host/tests/test_write_decode.py).
//
//   decode_write 1440|720 <cylinder> <head> <start_byte> < intervals.bin > track.bin
//
// stdin: flux intervals as 16-bit little-endian counts of capture ticks (16
// per cell). stdout: the track's sector data (zeros where nothing was
// written), in IMG order. stderr: one summary line.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "ibm_track.h"
#include "mfm_decode.h"
#include "track_write.h"

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr, "usage: decode_write 1440|720 <cylinder> <head> <start_byte> < in > out\n");
        return 2;
    }
    const ibm::Format& fmt = std::strcmp(argv[1], "720") == 0 ? ibm::DD_720 : ibm::HD_1440;
    uint8_t cyl = uint8_t(std::atoi(argv[2])), head = uint8_t(std::atoi(argv[3]));
    uint32_t start = uint32_t(std::atoi(argv[4]));

    std::vector<uint8_t> store(64 * 1024);
    std::vector<mfm::Field> fields(128);
    mfm::FieldScanner scanner(store.data(), store.size(), fields.data(), fields.size());
    scanner.reset(uint16_t(fmt.sectorBytes()));
    mfm::DataSeparator sep(16);
    uint8_t b[2];
    while (std::fread(b, 1, 2, stdin) == 2) {
        unsigned n = sep.cells(uint32_t(b[0] | (b[1] << 8)));
        for (unsigned i = 1; i < n; ++i) scanner.addCell(false);
        scanner.addCell(true);
    }

    track_write::Update updates[64];
    track_write::Result r = track_write::interpret(fmt, cyl, head, start, scanner, updates, 64);
    std::vector<uint8_t> track(fmt.trackDataBytes());
    for (unsigned i = 0; i < r.updates; ++i)
        std::memcpy(&track[updates[i].sector * fmt.sectorBytes()], &store[updates[i].data_offset], fmt.sectorBytes());
    std::fwrite(track.data(), 1, track.size(), stdout);
    std::fprintf(stderr, "fields=%zu updates=%u bad_crc=%u unplaced=%u\n", scanner.count(), r.updates, r.bad_crc,
                 r.unplaced);
    return 0;
}
