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
//
// Or, with the drive as A and B on one cable, COPY A:*.* B:
//
//   dos_session --copy <a.img> <b.img> <a out.img> <b out.img>
//
// stdout: one "COPY <name> <size>" line per file, then a summary line.
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

// The PC as at power-on: motor on, recalibrated, the disk's rate found.
static bool start(VirtualPc& pc) {
    pc.motorOn();
    return pc.recalibrate() && pc.detectRate();
}

static int copySession(char** argv) {
    std::vector<uint8_t> a_image = load(argv[2]), b_image = load(argv[3]);
    ibm::Format a_format, b_format;
    const ibm::Format* a_fmt = ibm::formatForSize(uint32_t(a_image.size()), a_format) ? &a_format : nullptr;
    const ibm::Format* b_fmt = ibm::formatForSize(uint32_t(b_image.size()), b_format) ? &b_format : nullptr;
    if (!a_fmt || !b_fmt) {
        std::fprintf(stderr, "not an image of a size the drive knows\n");
        return 1;
    }
    Machine m(bus::Setup{bus::Interface::IbmPc, bus::Cable::Twisted, 0b11}, {a_fmt, b_fmt}, Core0Costs{}, 37);
    MemorySource a(*a_fmt, std::move(a_image)), b(*b_fmt, std::move(b_image));
    m.eject(0);
    m.insert(a, 0);
    m.eject(1);
    m.insert(b, 1);
    VirtualPc pc(m);
    if (!start(pc)) {
        std::fprintf(stderr, "the PC couldn't read A:\n");
        return 1;
    }
    pc.useDrive(1);
    if (!pc.recalibrate() || !pc.detectRate()) {
        std::fprintf(stderr, "the PC couldn't read B:\n");
        return 1;
    }
    Dos from(pc, 0), to(pc, 1);
    if (!from.mount() || !to.mount()) {
        std::fprintf(stderr, "couldn't mount both disks\n");
        return 1;
    }
    // COPY reads a file from A, then writes it to B: the drive changes for
    // every file, with both motors running.
    std::vector<Dos::Entry> files = from.dir();
    for (const Dos::Entry& e : files) {
        std::vector<uint8_t> data;
        if (!from.readFile(e, data) || !to.writeFile(e.name.c_str(), data)) {
            std::fprintf(stderr, "couldn't copy %s\n", e.name.c_str());
            return 1;
        }
        std::printf("COPY %s %u\n", e.name.c_str(), unsigned(data.size()));
    }
    // Read B back through a fresh mount, against A: from the drives.
    Dos check_a(pc, 0), check_b(pc, 1);
    if (!check_b.mount() || !check_a.mount()) return 1;
    for (const Dos::Entry& e : files) {
        std::vector<uint8_t> want, got;
        bool found = false;
        for (const Dos::Entry& f : check_b.dir())
            if (f.name == e.name) found = check_b.readFile(f, got);
        if (!found || !check_a.readFile(e, want) || got != want) {
            std::fprintf(stderr, "%s didn't read back from B:\n", e.name.c_str());
            return 1;
        }
    }
    pc.motorOff();
    pc.useDrive(0);
    pc.motorOff();
    pc.run(100000);
    if (!save(argv[4], a.image()) || !save(argv[5], b.image())) return 1;
    std::printf("SESSION files=%u a_writes=%u b_writes=%u retries=%u\n", unsigned(files.size()), a.writes, b.writes,
                pc.retries);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 6 && std::string(argv[1]) == "--copy") return copySession(argv);
    if (argc != 6) {
        std::fprintf(stderr, "usage: dos_session <in.img> <out.img> <extract folder> <new name> <new contents file>\n");
        return 2;
    }
    std::vector<uint8_t> image = load(argv[1]);
    ibm::Format format;
    const ibm::Format* fmt = ibm::formatForSize(uint32_t(image.size()), format) ? &format : nullptr;
    if (!fmt) {
        std::fprintf(stderr, "%s: not an image of a size the drive knows\n", argv[1]);
        return 1;
    }

    // The machine, with this image in, and the PC as at power-on.
    Machine m(*fmt, Core0Costs{}, 37);
    MemorySource disk(*fmt, std::move(image));
    m.eject();
    m.insert(disk);
    VirtualPc pc(m);
    if (!start(pc)) {
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
