// Just enough of DOS to use a real FAT12 disk through the virtual PC's BIOS
// calls (virtual_pc.h): read the boot sector's BIOS parameter block, both
// FAT copies and the root directory; read a file by following its cluster
// chain; write a new file (first-fit clusters, both FATs, a root entry).
// Every sector goes through the drive, as DOS's would. Test code only.
#pragma once

#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "virtual_pc.h"

class Dos {
public:
    struct Entry {
        std::string name;  // "NAME.EXT"
        uint8_t attr;
        uint16_t cluster;
        uint32_t size;
    };

    // DOS on drive A (0) or B (1): each call selects it first.
    explicit Dos(VirtualPc& pc, int drive = 0) : pc_(pc), drive_(drive) {}

    // Reads the BIOS parameter block, the FAT and the root directory.
    bool mount() {
        pc_.useDrive(drive_);
        std::vector<uint8_t> boot(512);
        if (pc_.readSectors(0, 0, 1, 1, boot.data()) != VirtualPc::Status::Ok) return false;
        if (boot[510] != 0x55 || boot[511] != 0xAA) return false;
        bps_ = rd16(&boot[11]);
        spc_ = boot[13];
        reserved_ = rd16(&boot[14]);
        fats_ = boot[16];
        root_entries_ = rd16(&boot[17]);
        total_ = rd16(&boot[19]);
        spf_ = rd16(&boot[22]);
        spt_ = rd16(&boot[24]);
        heads_ = rd16(&boot[26]);
        if (bps_ != 512 || !spc_ || !fats_ || !spf_ || !spt_ || heads_ != 2) return false;
        root_sectors_ = (root_entries_ * 32u + 511) / 512;
        data_start_ = reserved_ + fats_ * spf_ + root_sectors_;
        clusters_ = (total_ - data_start_) / spc_;
        fat_.resize(spf_ * 512u);
        if (!read(reserved_, spf_, fat_.data())) return false;
        root_.resize(root_sectors_ * 512u);
        return read(rootStart(), root_sectors_, root_.data());
    }

    // The root directory's files (not volume labels, directories or deleted entries).
    std::vector<Entry> dir() const {
        std::vector<Entry> out;
        for (uint32_t i = 0; i < root_entries_; ++i) {
            const uint8_t* e = &root_[i * 32];
            if (e[0] == 0) break;
            if (e[0] == 0xE5 || (e[11] & 0x18)) continue;  // deleted, volume label or directory
            out.push_back(Entry{name83(e), e[11], rd16(e + 26), rd32(e + 28)});
        }
        return out;
    }

    bool readFile(const Entry& f, std::vector<uint8_t>& out) {
        out.clear();
        std::vector<uint8_t> cluster(spc_ * 512u);
        for (uint16_t c = f.cluster; out.size() < f.size; c = fatEntry(c)) {
            if (c < 2 || c >= 0xFF8) return false;  // the chain ended early
            if (!read(clusterLba(c), spc_, cluster.data())) return false;
            size_t n = std::min<size_t>(cluster.size(), f.size - out.size());
            out.insert(out.end(), cluster.begin(), cluster.begin() + long(n));
        }
        return true;
    }

    // A new file in the root: data first, then both FAT copies, then the
    // directory entry, as DOS orders its writes.
    bool writeFile(const char* name83_, const std::vector<uint8_t>& data) {
        uint32_t per = spc_ * 512u;
        uint32_t need = uint32_t((data.size() + per - 1) / per);
        std::vector<uint16_t> chain;
        for (uint16_t c = 2; c < clusters_ + 2 && chain.size() < need; ++c)
            if (fatEntry(c) == 0) chain.push_back(c);
        if (chain.size() < need) return false;  // disk full
        int slot = -1;
        for (uint32_t i = 0; i < root_entries_ && slot < 0; ++i)
            if (root_[i * 32] == 0 || root_[i * 32] == 0xE5) slot = int(i);
        if (slot < 0) return false;  // root directory full

        std::vector<uint8_t> buf(per);
        for (size_t k = 0; k < chain.size(); ++k) {
            std::fill(buf.begin(), buf.end(), 0);
            size_t from = k * per, n = std::min<size_t>(per, data.size() - from);
            std::memcpy(buf.data(), data.data() + from, n);
            if (!write(clusterLba(chain[k]), spc_, buf.data())) return false;
            setFatEntry(chain[k], k + 1 < chain.size() ? chain[k + 1] : 0xFFF);
        }
        for (uint8_t copy = 0; copy < fats_; ++copy)
            if (!write(reserved_ + copy * spf_, spf_, fat_.data())) return false;

        uint8_t* e = &root_[slot * 32];
        std::memset(e, 0, 32);
        std::memset(e, ' ', 11);
        const char* dot = std::strchr(name83_, '.');
        size_t base = dot ? size_t(dot - name83_) : std::strlen(name83_);
        for (size_t i = 0; i < base && i < 8; ++i) e[i] = uint8_t(std::toupper(name83_[i]));
        if (dot)
            for (size_t i = 0; i < 3 && dot[1 + i]; ++i) e[8 + i] = uint8_t(std::toupper(dot[1 + i]));
        e[11] = 0x20;  // archive
        wr16(e + 24, uint16_t(((2026 - 1980) << 9) | (10 << 5) | 23));  // a date: 2026-10-23
        wr16(e + 26, chain.empty() ? 0 : chain[0]);
        wr32(e + 28, uint32_t(data.size()));
        uint32_t dir_sector = uint32_t(slot) * 32 / 512;
        return write(rootStart() + dir_sector, 1, &root_[dir_sector * 512]);
    }

    uint32_t freeClusters() const {
        uint32_t n = 0;
        for (uint16_t c = 2; c < clusters_ + 2; ++c) n += fatEntry(c) == 0;
        return n;
    }
    unsigned sectorsRead = 0, sectorsWritten = 0;

private:
    static uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
    static uint32_t rd32(const uint8_t* p) { return uint32_t(p[0] | (p[1] << 8) | (p[2] << 16)) | (uint32_t(p[3]) << 24); }
    static void wr16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v), p[1] = uint8_t(v >> 8); }
    static void wr32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }

    static std::string name83(const uint8_t* e) {
        std::string base(reinterpret_cast<const char*>(e), 8), ext(reinterpret_cast<const char*>(e + 8), 3);
        while (!base.empty() && base.back() == ' ') base.pop_back();
        while (!ext.empty() && ext.back() == ' ') ext.pop_back();
        return ext.empty() ? base : base + "." + ext;
    }

    // FAT12: two 12-bit entries in every three bytes.
    uint16_t fatEntry(uint16_t c) const {
        uint32_t at = c + c / 2;
        uint16_t v = uint16_t(fat_[at] | (fat_[at + 1] << 8));
        return c & 1 ? uint16_t(v >> 4) : uint16_t(v & 0xFFF);
    }
    void setFatEntry(uint16_t c, uint16_t value) {
        uint32_t at = c + c / 2;
        if (c & 1) {
            fat_[at] = uint8_t((fat_[at] & 0x0F) | ((value << 4) & 0xF0));
            fat_[at + 1] = uint8_t(value >> 4);
        } else {
            fat_[at] = uint8_t(value);
            fat_[at + 1] = uint8_t((fat_[at + 1] & 0xF0) | ((value >> 8) & 0x0F));
        }
    }

    uint32_t rootStart() const { return reserved_ + fats_ * spf_; }
    uint32_t clusterLba(uint16_t c) const { return data_start_ + (c - 2u) * spc_; }

    // Sectors by LBA, split into whole-track BIOS calls as DOS does.
    bool read(uint32_t lba, uint32_t count, uint8_t* out) { return transfer(lba, count, out, nullptr); }
    bool write(uint32_t lba, uint32_t count, const uint8_t* data) { return transfer(lba, count, nullptr, data); }
    bool transfer(uint32_t lba, uint32_t count, uint8_t* out, const uint8_t* data) {
        pc_.useDrive(drive_);
        while (count) {
            uint8_t cyl = uint8_t(lba / (spt_ * heads_)), head = uint8_t((lba / spt_) % heads_);
            uint8_t sector = uint8_t(lba % spt_ + 1);
            uint32_t n = std::min<uint32_t>(count, spt_ - (sector - 1u));  // to the end of this track
            VirtualPc::Status s = data ? pc_.writeSectors(cyl, head, sector, uint8_t(n), data)
                                       : pc_.readSectors(cyl, head, sector, uint8_t(n), out);
            if (s != VirtualPc::Status::Ok) return false;
            (data ? sectorsWritten : sectorsRead) += n;
            if (out) out += n * 512;
            if (data) data += n * 512;
            lba += n;
            count -= n;
        }
        return true;
    }

    VirtualPc& pc_;
    int drive_;
    uint16_t bps_ = 0, reserved_ = 0, root_entries_ = 0, total_ = 0, spf_ = 0, spt_ = 0, heads_ = 0;
    uint8_t spc_ = 0, fats_ = 0;
    uint32_t root_sectors_ = 0, data_start_ = 0, clusters_ = 0;
    std::vector<uint8_t> fat_, root_;
};
