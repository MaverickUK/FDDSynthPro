// Writes the welcome disk (src/welcome_disk.*) out as a 1.44 MB image, for
// host/tests/test_welcome_disk.py to check with mtools and fsck.fat.
//
//   welcome_image <out.img>
#include <cstdio>

#include "welcome_disk.h"
#include "welcome_text.h"

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: welcome_image <out.img>\n");
        return 2;
    }
    FILE* f = std::fopen(argv[1], "wb");
    if (!f) {
        std::perror(argv[1]);
        return 1;
    }
    uint8_t sector[welcome_disk::SECTOR_BYTES];
    for (uint32_t lba = 0; lba < welcome_disk::SECTORS; ++lba) {
        welcome_disk::sector(lba, welcome_text::FILES, welcome_text::FILE_COUNT, welcome_text::FAT_DATE, sector);
        std::fwrite(sector, 1, sizeof sector, f);
    }
    return std::fclose(f) == 0 ? 0 : 1;
}
