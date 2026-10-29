#include "welcome_disk.h"

#include <cstring>

namespace welcome_disk {

static void put16(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}

static void put32(uint8_t* p, uint32_t v) {
    put16(p, v);
    put16(p + 2, v >> 16);
}

// What a PC that boots this disk runs (at 7C3Eh, after the BPB), as a DOS
// data disk does: a message, a key, then INT 19h to boot again (from the
// hard disk, or this one once a system disk is in).
//     xor ax,ax / mov ds,ax / cld / mov si,MESSAGE
//   next: lodsb / or al,al / jz done
//     mov ah,0Eh / mov bx,0007h / int 10h / jmp next     (teletype out)
//   done: xor ax,ax / int 16h / int 19h                  (a key, then reboot)
static const uint8_t BOOT_CODE[] = {
    0x31, 0xC0, 0x8E, 0xD8, 0xFC, 0xBE, 0x5A, 0x7C,  // 3E: si = 7C5Ah, the message
    0xAC, 0x08, 0xC0, 0x74, 0x09,                    // 46: next
    0xB4, 0x0E, 0xBB, 0x07, 0x00, 0xCD, 0x10, 0xEB, 0xF2,
    0x31, 0xC0, 0xCD, 0x16, 0xCD, 0x19,              // 54: done
};
constexpr uint32_t BOOT_CODE_AT = 0x3E;
constexpr uint32_t MESSAGE_AT = 0x5A;
static_assert(BOOT_CODE_AT + sizeof BOOT_CODE == MESSAGE_AT, "the code's message address");
static const char MESSAGE[] =
    "\r\nThis is the FDD Synth Pro welcome disk, not a system disk.\r\n"
    "Read README.TXT on it from DOS or any computer, or put a disk image\r\n"
    "on the drive's microSD card. Press a key to try again.\r\n";
static_assert(MESSAGE_AT + sizeof MESSAGE <= 510, "the message fits the boot sector");

static void bootSector(uint8_t* out) {
    static const uint8_t JUMP[] = {0xEB, 0x3C, 0x90};  // over the BPB to the code at 3Eh
    std::memcpy(out, JUMP, sizeof JUMP);
    std::memcpy(out + 3, "MSDOS5.0", 8);
    put16(out + 0x0B, SECTOR_BYTES);
    out[0x0D] = 1;  // sectors a cluster
    put16(out + 0x0E, RESERVED_SECTORS);
    out[0x10] = FAT_COUNT;
    put16(out + 0x11, ROOT_ENTRIES);
    put16(out + 0x13, SECTORS);
    out[0x15] = 0xF0;  // a 3.5-inch floppy
    put16(out + 0x16, FAT_SECTORS);
    put16(out + 0x18, SECTORS_PER_TRACK);
    put16(out + 0x1A, HEADS);
    out[0x24] = 0x00;  // BIOS drive number: the first floppy
    out[0x26] = 0x29;  // the extended boot signature: the next three fields are there
    put32(out + 0x27, VOLUME_ID);
    std::memcpy(out + 0x2B, VOLUME_LABEL, 11);
    std::memcpy(out + 0x36, "FAT12   ", 8);
    std::memcpy(out + BOOT_CODE_AT, BOOT_CODE, sizeof BOOT_CODE);
    std::memcpy(out + MESSAGE_AT, MESSAGE, sizeof MESSAGE);  // with its terminating zero
    out[510] = 0x55;
    out[511] = 0xAA;
}

static uint32_t clustersOf(const File& f) { return (f.size + SECTOR_BYTES - 1) / SECTOR_BYTES; }

// FAT entry `n`: each file's clusters chained, the last marked end of chain.
static uint16_t fatEntry(uint32_t n, const File* files, size_t count) {
    if (n == 0) return 0xFF0;  // the media byte, F0h
    if (n == 1) return 0xFFF;
    uint32_t first = 2;
    for (size_t i = 0; i < count; ++i) {
        uint32_t end = first + clustersOf(files[i]);
        if (n < end) return n + 1 == end ? 0xFFF : uint16_t(n + 1);
        first = end;
    }
    return 0;  // free
}

// Byte `b` of a FAT: FAT12 packs each pair of 12-bit entries into three bytes.
static uint8_t fatByte(uint32_t b, const File* files, size_t count) {
    uint32_t pair = b / 3;
    uint16_t even = fatEntry(2 * pair, files, count), odd = fatEntry(2 * pair + 1, files, count);
    switch (b % 3) {
        case 0: return uint8_t(even);
        case 1: return uint8_t((even >> 8 & 0x0F) | (odd & 0x0F) << 4);
        default: return uint8_t(odd >> 4);
    }
}

// A directory entry's name: "README.TXT" as "README  TXT".
static void putName(uint8_t* p, const char* name) {
    std::memset(p, ' ', 11);
    const char* dot = std::strchr(name, '.');
    size_t base = dot ? size_t(dot - name) : std::strlen(name);
    std::memcpy(p, name, base < 8 ? base : 8);
    if (dot) {
        size_t ext = std::strlen(dot + 1);
        std::memcpy(p + 8, dot + 1, ext < 3 ? ext : 3);
    }
}

// Root folder entry `n`: the volume label, then the files.
static void putRootEntry(uint8_t* p, uint32_t n, const File* files, size_t count, uint16_t date) {
    if (n == 0) {
        std::memcpy(p, VOLUME_LABEL, 11);
        p[11] = 0x08;  // volume label
        put16(p + 0x18, date);
        return;
    }
    if (n > count) return;  // unused
    uint32_t first = 2;
    for (size_t i = 0; i + 1 < n; ++i) first += clustersOf(files[i]);
    const File& f = files[n - 1];
    putName(p, f.name);
    p[11] = 0x20;           // archive: an ordinary file
    put16(p + 0x10, date);  // created
    put16(p + 0x12, date);  // last opened
    put16(p + 0x18, date);  // last written (at midnight: time 0)
    put16(p + 0x1A, f.size ? first : 0);
    put32(p + 0x1C, f.size);
}

// The bytes of a data sector (DATA_START or later) that hold a file.
static void dataSector(uint32_t lba, const File* files, size_t count, uint8_t* out) {
    uint32_t cluster = 2 + (lba - DATA_START);
    uint32_t first = 2;
    for (size_t i = 0; i < count; ++i) {
        uint32_t end = first + clustersOf(files[i]);
        if (cluster < end) {
            uint32_t offset = (cluster - first) * SECTOR_BYTES;
            uint32_t left = files[i].size - offset;
            std::memcpy(out, files[i].data + offset, left < SECTOR_BYTES ? left : SECTOR_BYTES);
            return;
        }
        first = end;
    }
}

void sector(uint32_t lba, const File* files, size_t count, uint16_t date, uint8_t out[SECTOR_BYTES]) {
    std::memset(out, 0, SECTOR_BYTES);
    if (lba == 0) {
        bootSector(out);
    } else if (lba >= FAT_START && lba < ROOT_START) {
        uint32_t first = (lba - FAT_START) % FAT_SECTORS * SECTOR_BYTES;
        for (uint32_t i = 0; i < SECTOR_BYTES; ++i) out[i] = fatByte(first + i, files, count);
    } else if (lba >= ROOT_START && lba < DATA_START) {
        uint32_t first = (lba - ROOT_START) * (SECTOR_BYTES / 32);
        for (uint32_t i = 0; i < SECTOR_BYTES / 32; ++i) putRootEntry(out + 32 * i, first + i, files, count, date);
    } else if (lba >= DATA_START && lba < SECTORS) {
        dataSector(lba, files, count, out);
    }
}

}  // namespace welcome_disk
