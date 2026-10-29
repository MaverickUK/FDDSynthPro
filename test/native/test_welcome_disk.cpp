// Tests for the welcome disk (src/welcome_disk.*): its FAT12 layout, its
// boot sector, and a virtual PC mounting it with DOS, reading README.TXT and
// writing to it, through the whole drive. mtools and fsck.fat check the
// disk itself in host/tests/test_welcome_disk.py.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "dos.h"
#include "machine.h"
#include "settings.h"
#include "virtual_pc.h"
#include "welcome_disk.h"
#include "welcome_text.h"

namespace {  // this file's own helpers and tests

using welcome_text::FILE_COUNT;
using welcome_text::FILES;

std::vector<uint8_t> sector(uint32_t lba) {
    std::vector<uint8_t> s(512);
    welcome_disk::sector(lba, FILES, FILE_COUNT, welcome_text::FAT_DATE, s.data());
    return s;
}

uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

TEST(test_welcome_boot_sector) {
    std::vector<uint8_t> b = sector(0);
    CHECK_EQ(b[0], 0xEB);  // a jump over the BPB: DOS checks for it
    CHECK_EQ(rd16(&b[0x0B]), 512);
    CHECK_EQ(b[0x0D], 1);
    CHECK_EQ(rd16(&b[0x11]), 224);
    CHECK_EQ(rd16(&b[0x13]), 2880);
    CHECK_EQ(b[0x15], 0xF0);
    CHECK_EQ(rd16(&b[0x16]), 9);
    CHECK_EQ(rd16(&b[0x18]), 18);
    CHECK_EQ(rd16(&b[0x1A]), 2);
    CHECK(std::memcmp(&b[0x2B], "FDDSYNTHPRO", 11) == 0);
    CHECK(std::memcmp(&b[0x36], "FAT12   ", 8) == 0);
    CHECK(b[510] == 0x55 && b[511] == 0xAA);
    // The code a PC runs if it boots this disk prints the message after it.
    CHECK(std::string(reinterpret_cast<const char*>(&b[0x5A])).find("not a system disk") != std::string::npos);
}

TEST(test_welcome_fat) {
    // FAT12: F0 FF FF, then README's chain, cluster 2 onwards, then the
    // next file's straight after.
    std::vector<uint8_t> f = sector(welcome_disk::FAT_START);
    CHECK(f[0] == 0xF0 && f[1] == 0xFF && f[2] == 0xFF);
    CHECK_STR(FILES[0].name, "README.TXT");
    uint32_t clusters = (FILES[0].size + 511) / 512;
    auto entry = [&](uint32_t n) {
        uint32_t at = n * 3 / 2;
        uint16_t v = uint16_t(f[at] | f[at + 1] << 8);
        return n & 1 ? v >> 4 : v & 0xFFF;
    };
    bool chained = true;
    for (uint32_t n = 2; n + 1 < 2 + clusters; ++n) chained = chained && entry(n) == n + 1;
    CHECK(chained);
    CHECK_EQ(entry(2 + clusters - 1), 0xFFFu);
    uint32_t all = 0;
    for (size_t i = 0; i < FILE_COUNT; ++i) all += (FILES[i].size + 511) / 512;
    CHECK_EQ(entry(2 + all - 1), 0xFFFu);
    CHECK_EQ(entry(2 + all), 0u);  // free from there
    CHECK(sector(welcome_disk::FAT_START + welcome_disk::FAT_SECTORS) == f);  // the second copy
}

TEST(test_welcome_disk_through_dos) {
    // The drive as it starts with no card: the welcome disk, cached in PSRAM.
    WelcomeSource welcome(FILES, FILE_COUNT, welcome_text::FAT_DATE);
    welcome.keepsWrites(true);
    Machine m;
    m.eject();
    m.insert(welcome);
    VirtualPc pc(m);
    pc.motorOn();
    CHECK(pc.recalibrate());
    pc.clearDiskChange();
    Dos dos(pc);
    CHECK(dos.mount());
    std::vector<Dos::Entry> files = dos.dir();
    CHECK_EQ(files.size(), FILE_COUNT);
    for (size_t i = 0; i < files.size() && i < FILE_COUNT; ++i) {
        std::vector<uint8_t> text;
        CHECK(files[i].name == FILES[i].name && dos.readFile(files[i], text));
        CHECK(std::string(text.begin(), text.end()) == std::string(FILES[i].data, FILES[i].size));
    }
    // A file saved to it stays, until the drive restarts.
    std::vector<uint8_t> note(3000, 'x');
    CHECK(dos.writeFile("NOTES.TXT", note));
    Dos again(pc);
    CHECK(again.mount());
    std::vector<uint8_t> back;
    for (const Dos::Entry& e : again.dir())
        if (e.name == "NOTES.TXT") again.readFile(e, back);
    CHECK(back == note);
    CHECK_EQ(pc.retries, 0u);
}

TEST(test_welcome_sample_settings) {
    // The sample FDDSYNTH.CFG reads as the defaults it describes, and its
    // commented-out lines (#image=...) are only notes.
    const welcome_disk::File* cfg = nullptr;
    for (const auto& f : FILES)
        if (std::string(f.name) == "FDDSYNTH.CFG") cfg = &f;
    CHECK(cfg != nullptr);
    if (!cfg) return;
    settings::Settings s = settings::parse(cfg->data, cfg->size);
    CHECK(s == settings::Settings{});
}

TEST(test_welcome_disk_read_only_without_psram) {
    // Nowhere to keep writes: it says so (write-protected), rather than
    // losing them.
    WelcomeSource welcome(FILES, FILE_COUNT, welcome_text::FAT_DATE);
    CHECK(!welcome.writable());
    std::vector<uint8_t> data(512);
    CHECK(!welcome.writeSector(0, 0, 0, data.data()));
}

}  // namespace

void runWelcomeDiskTests() {
    RUN(test_welcome_boot_sector);
    RUN(test_welcome_fat);
    RUN(test_welcome_disk_through_dos);
    RUN(test_welcome_sample_settings);
    RUN(test_welcome_disk_read_only_without_psram);
}
