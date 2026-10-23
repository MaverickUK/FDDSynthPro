// A DOS session on a real FAT12 image, through the whole drive: a virtual
// PC (test/native/virtual_pc.h) mounts it, reads every file in the root,
// and writes a new one, every sector going through the drive model, the
// loader, the streamer's buffer and the write path. For
// host/tests/test_dos_disk.py, which checks the results with mtools and
// fsck.fat.
//
//   dos_session <in.img> <out.img> <extract folder> <new file name> <new file contents>
//
// stdout: one "FILE <name> <size>" line per file read, then a summary line.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "dos.h"
#include "machine.h"
#include "track_source.h"
#include "virtual_pc.h"

static std::vector<uint8_t> load(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

static bool save(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    return bool(f);
}

int main(int argc, char** argv) {
    if (argc != 6) {
        std::fprintf(stderr, "usage: dos_session <in.img> <out.img> <extract folder> <new name> <new contents file>\n");
        return 2;
    }
    std::vector<uint8_t> image = load(argv[1]);
    const ibm::Format* fmt = formatForImageSize(uint32_t(image.size()));
    if (!fmt) {
        std::fprintf(stderr, "%s: not a 720 KB or 1.44 MB image\n", argv[1]);
        return 1;
    }

    // The machine, with this image in, and the PC as at power-on.
    Machine m(*fmt, Core0Costs{}, 37);
    MemorySource disk(*fmt, std::move(image));
    m.eject();
    m.insert(disk);
    VirtualPc pc(m);
    pc.motorOn();
    if (!pc.recalibrate() || !pc.detectRate()) {
        std::fprintf(stderr, "the PC couldn't read the disk\n");
        return 1;
    }

    Dos dos(pc);
    if (!dos.mount()) {
        std::fprintf(stderr, "not a FAT12 disk the PC could mount\n");
        return 1;
    }
    for (const Dos::Entry& e : dos.dir()) {
        std::vector<uint8_t> data;
        if (!dos.readFile(e, data)) {
            std::fprintf(stderr, "couldn't read %s\n", e.name.c_str());
            return 1;
        }
        save(std::string(argv[3]) + "/" + e.name, data);
        std::printf("FILE %s %u\n", e.name.c_str(), unsigned(data.size()));
    }

    std::vector<uint8_t> contents = load(argv[5]);
    uint32_t free_before = dos.freeClusters();
    if (!dos.writeFile(argv[4], contents)) {
        std::fprintf(stderr, "couldn't write %s\n", argv[4]);
        return 1;
    }
    // Read it back through a fresh mount: from the drive, not from memory.
    Dos again(pc);
    std::vector<uint8_t> back;
    bool found = false;
    if (again.mount())
        for (const Dos::Entry& e : again.dir())
            if (e.name == argv[4]) found = again.readFile(e, back);
    if (!found || back != contents) {
        std::fprintf(stderr, "%s didn't read back\n", argv[4]);
        return 1;
    }
    pc.motorOff();
    pc.run(100000);
    if (!save(argv[2], disk.image())) return 1;
    std::printf("SESSION read=%u written=%u retries=%u free_before=%u free_after=%u\n", dos.sectorsRead,
                dos.sectorsWritten, pc.retries, free_before, again.freeClusters());
    return 0;
}
