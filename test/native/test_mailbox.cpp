// SynthMinder's mailbox (mailbox.h) and the drive's answers (duo_commands.h):
// sentences, the sector format, and a virtual PC exchanging requests and
// replies at cylinder 82 through the BIOS's reads and writes, as SYNMIND does.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "duo_commands.h"
#include "machine.h"
#include "mailbox.h"
#include "sentence.h"
#include "virtual_pc.h"

using Status = VirtualPc::Status;

namespace {  // this file's own helpers and tests

// A pretend drive: what the real one keeps in main.cpp.
class FakeDuo : public duo::Duo {
public:
    FakeDuo() {
        for (int i = 0; i < 3; ++i) images.push_back("DISK" + std::to_string(i + 1) + ".IMG");
    }
    const char* firmware() const override { return "9.9.9"; }
    unsigned drives() const override { return two ? 2 : 1; }
    void driveInfo(unsigned drive, duo::DriveInfo& out) const override {
        out = duo::DriveInfo{};
        out.letter = char('A' + drive);
        out.disk_in = disk[drive] >= 0;
        out.disk = out.disk_in ? images[size_t(disk[drive])].c_str() : "";
        out.kb = 1440;
        std::snprintf(out.format, sizeof out.format, "1440 KB MFM 300 rpm");
        out.cylinder = 82;
        out.motor = true;
    }
    int volume() const override { return vol; }
    size_t packCount() const override { return 2; }
    size_t packIndex() const override { return pack; }
    const char* packName(size_t i) const override { return i ? "Placeholder" : "Recorded"; }
    size_t imageCount() const override { return images.size(); }
    const char* imageName(size_t i) const override { return images[i].c_str(); }
    uint32_t imageKB(size_t) const override { return 1440; }
    size_t settingCount() const override { return 1; }
    duo::Setting setting(size_t) const override { return {"drive", "a,b", "a"}; }
    const char* insert(unsigned drive, size_t image, unsigned asked_on) override {
        ++inserts;
        last_asked_on = asked_on;
        disk[drive] = int(image);
        return nullptr;
    }
    const char* eject(unsigned drive, unsigned) override {
        if (disk[drive] < 0) return "empty";
        disk[drive] = -1;
        return nullptr;
    }
    const char* setPack(size_t i) override {
        pack = i;
        return nullptr;
    }
    const char* setVolume(int v) override {
        vol = v;
        return nullptr;
    }
    const char* set(const char* key, const char* value) override {
        last_set = std::string(key) + "=" + value;
        return std::strcmp(key, "drive") == 0 ? nullptr : "key";
    }

    std::vector<std::string> images;
    bool two = false;
    int disk[2] = {0, -1};
    int vol = 75;
    size_t pack = 0;
    unsigned inserts = 0;
    unsigned last_asked_on = 9;
    std::string last_set;
};

std::string ask(duo::Duo& d, const char* body, unsigned drive = 0) {
    char line[256], out[mailbox::REPLY_TEXT_MAX];
    size_t n = sentence::make(body, std::strlen(body), line, sizeof line);
    return std::string(out, duo::answer(d, drive, line, uint32_t(n), out, sizeof out));
}

// The sentence `$body*CS` and its line feed.
std::string line(const char* body) {
    char out[256];
    return std::string(out, sentence::make(body, std::strlen(body), out, sizeof out));
}

std::vector<uint8_t> request(uint16_t seq, const char* body) {
    std::vector<uint8_t> s(mailbox::SECTOR, 0);
    std::memcpy(s.data(), "SMND", 4);
    s[4] = mailbox::VERSION;
    s[6] = uint8_t(seq);
    s[7] = uint8_t(seq >> 8);
    std::string text = line(body);
    s[8] = uint8_t(text.size());
    s[9] = uint8_t(text.size() >> 8);
    std::memcpy(&s[mailbox::HEADER], text.data(), text.size());
    return s;
}

// The text of a reply's sectors, as SynthMinder reads it; `seq` gets the
// first sector's sequence number (-1 if it isn't an SMND sector).
std::string replyText(const uint8_t* sectors, int& seq) {
    seq = -1;
    if (std::memcmp(sectors, "SMND", 4) != 0) return "";
    seq = sectors[6] | sectors[7] << 8;
    std::string text;
    for (unsigned s = 0; s < mailbox::REPLY_SECTORS; ++s) {
        const uint8_t* p = sectors + s * mailbox::SECTOR;
        text.append(reinterpret_cast<const char*>(p + mailbox::HEADER), p[8] | p[9] << 8);
        if (!(p[5] & mailbox::FLAG_MORE)) break;
    }
    return text;
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// ---- Sentences and sectors --------------------------------------------------------------

TEST(test_sentence_round_trip) {
    std::string s = line("FDC,1,VOL,60");
    CHECK_STR(s, "$FDC,1,VOL,60*" + std::string(s.substr(s.size() - 3, 2)) + "\n");
    sentence::Parsed p;
    CHECK(sentence::parse(s.data(), s.size(), p));
    CHECK_EQ(p.count, 4u);
    CHECK(p.is(0, "FDC") && p.is(2, "VOL") && p.is(3, "60"));
    std::string bad = s;
    bad[5] = '2';  // the checksum no longer matches
    CHECK(!sentence::parse(bad.data(), bad.size(), p));
    std::string crlf = s.substr(0, s.size() - 1) + "\r\n";
    CHECK(sentence::parse(crlf.data(), crlf.size(), p));
    char clean[16];
    sentence::clean("a,b*c$d", clean, sizeof clean);
    CHECK_STR(clean, "a_b_c_d");
}

TEST(test_mailbox_eligible_disks) {
    CHECK(mailbox::eligible(ibm::HD_1440));
    CHECK(mailbox::eligible(ibm::DD_720));
    CHECK(!mailbox::eligible(ibm::amigaDD()));
    ibm::Format forty = ibm::DD_720;
    forty.step = 2;  // a 40-track disk, double-stepped
    CHECK(!mailbox::eligible(forty));
    ibm::Format eight = ibm::DD_720;
    eight.kinds[0].sectors = 8;  // too few sectors for the reply's 5-9
    CHECK(!mailbox::eligible(eight));
    ibm::Format big = ibm::HD_1440;
    big.cyls = 84;  // reaches cylinder 82 itself
    CHECK(!mailbox::eligible(big));
}

TEST(test_mailbox_request_sector) {
    mailbox::Request r;
    std::vector<uint8_t> s = request(513, "FDQ,1,STATUS");
    CHECK(mailbox::parseRequest(s.data(), r));
    CHECK_EQ(r.seq, 513);
    CHECK_STR(std::string(r.text, r.len), line("FDQ,1,STATUS"));
    std::vector<uint8_t> bad = s;
    bad[0] = 'X';
    CHECK(!mailbox::parseRequest(bad.data(), r));
    bad = s;
    bad[4] = 2;  // a later version
    CHECK(!mailbox::parseRequest(bad.data(), r));
    bad = s;
    bad[8] = 0xF1, bad[9] = 1;  // longer than a sector holds
    CHECK(!mailbox::parseRequest(bad.data(), r));
}

TEST(test_mailbox_reply_packs_whole_sentences) {
    std::string text;
    for (int i = 0; i < 70; ++i) text += line(("FDF,1," + std::to_string(i) + ",70,SOMEGAME.IMG,1440,I").c_str());
    std::vector<uint8_t> out(mailbox::REPLY_SECTORS * mailbox::SECTOR);
    uint32_t placed = mailbox::buildReply(7, text.data(), uint32_t(text.size()), out.data());
    CHECK(placed < text.size());  // 70 don't fit in five sectors
    CHECK(placed > 4 * mailbox::TEXT_MAX);
    int seq;
    std::string got = replyText(out.data(), seq);
    CHECK_EQ(seq, 7);
    CHECK_EQ(got.size(), placed);
    CHECK_STR(got, text.substr(0, placed));
    CHECK(got.back() == '\n');  // never split mid-sentence
    for (unsigned s = 0; s < mailbox::REPLY_SECTORS; ++s) {
        const uint8_t* p = &out[s * mailbox::SECTOR];
        CHECK(std::memcmp(p, "SMND", 4) == 0);
        CHECK_EQ(p[5], s + 1 < mailbox::REPLY_SECTORS ? mailbox::FLAG_MORE : 0);
        CHECK(p[8] | p[9] << 8);
    }
    // A short reply: one sector, no more flag; an empty one still carries the sequence.
    mailbox::buildReply(8, "", 0, out.data());
    CHECK(std::memcmp(out.data(), "SMND", 4) == 0);
    CHECK_EQ(out[6], 8);
    CHECK_EQ(out[5], 0);
}

// ---- The answers ------------------------------------------------------------------------

TEST(test_duo_status) {
    FakeDuo d;
    d.two = true;
    d.disk[1] = 2;
    std::string r = ask(d, "FDQ,1,STATUS", 1);
    CHECK(contains(r, line("FDV,1,firmware,9.9.9")));
    CHECK(contains(r, line("FDV,1,drives,AB")));
    CHECK(contains(r, line("FDV,1,mailbox,B")));
    CHECK(contains(r, line("FDV,1,volume,75")));
    CHECK(contains(r, line("FDV,1,pack,Recorded")));
    CHECK(contains(r, line("FDV,1,a_disk,DISK1.IMG")));
    CHECK(contains(r, line("FDV,1,b_disk,DISK3.IMG")));
    CHECK(contains(r, line("FDV,1,a_format,1440 KB MFM 300 rpm")));
    d.disk[1] = -1;
    r = ask(d, "FDQ,1,STATUS");
    CHECK(contains(r, line("FDV,1,b_disk,-")));
    CHECK(!contains(r, "b_kb"));
}

TEST(test_duo_files_in_pages) {
    FakeDuo d;
    d.images.clear();
    for (int i = 0; i < 70; ++i) d.images.push_back("GAMEDISK" + std::to_string(i) + ".IMG");
    std::vector<bool> seen(70, false);
    unsigned from = 0, asks = 0;
    while (from < 70 && asks < 10) {
        ++asks;
        std::string r = ask(d, ("FDQ,1,FILES," + std::to_string(from)).c_str());
        CHECK(r.size() <= mailbox::REPLY_TEXT_MAX);
        unsigned got = 0;
        for (size_t at = 0; (at = r.find("$FDF,1,", at)) != std::string::npos; ++at) {
            unsigned i = unsigned(std::stoul(r.substr(at + 7)));
            CHECK(i < 70 && !seen[i]);
            if (i < 70) seen[i] = true;
            from = i + 1;
            ++got;
        }
        CHECK(got > 0);
    }
    for (bool s : seen) CHECK(s);
    CHECK(asks > 1);
    CHECK(contains(ask(d, "FDQ,1,FILES,70"), "FDF,1,70,70,-,0,-"));
    CHECK(contains(ask(d, "FDQ,1,FILES"), line("FDF,1,0,70,GAMEDISK0.IMG,1440,I")));
}

TEST(test_duo_changes) {
    FakeDuo d;
    d.two = true;
    CHECK_STR(ask(d, "FDC,1,INSERT,B,disk2.img"), line("FDK,1,INSERT,OK,"));  // any case
    CHECK_EQ(d.disk[1], 1);
    CHECK_EQ(d.last_asked_on, 0u);
    CHECK_STR(ask(d, "FDC,1,INSERT,B,NOPE.IMG"), line("FDK,1,INSERT,ERR,notfound"));
    CHECK_STR(ask(d, "FDC,1,INSERT,C,DISK1.IMG"), line("FDK,1,INSERT,ERR,nodrive"));
    CHECK_STR(ask(d, "FDC,1,EJECT,B"), line("FDK,1,EJECT,OK,"));
    CHECK_STR(ask(d, "FDC,1,EJECT,B"), line("FDK,1,EJECT,ERR,empty"));
    CHECK_STR(ask(d, "FDC,1,VOL,60"), line("FDK,1,VOL,OK,"));
    CHECK_EQ(d.vol, 60);
    CHECK_STR(ask(d, "FDC,1,VOL,101"), line("FDK,1,VOL,ERR,range"));
    CHECK_STR(ask(d, "FDC,1,VOL,6x"), line("FDK,1,VOL,ERR,range"));
    CHECK_STR(ask(d, "FDC,1,PACK,1"), line("FDK,1,PACK,OK,"));
    CHECK_EQ(d.pack, 1u);
    CHECK_STR(ask(d, "FDC,1,PACK,2"), line("FDK,1,PACK,ERR,range"));
    CHECK_STR(ask(d, "FDC,1,SET,drive,a"), line("FDK,1,SET,OK,"));
    CHECK_STR(d.last_set, "drive=a");
    CHECK_STR(ask(d, "FDC,1,SET,drive,a,b"), line("FDK,1,SET,OK,"));
    CHECK_STR(d.last_set, "drive=a,b");  // the comma's fields joined again
    CHECK_STR(ask(d, "FDC,1,SET,volume,5"), line("FDK,1,SET,ERR,key"));
    CHECK_STR(ask(d, "FDQ,1,SETTINGS"), line("FDS,1,drive,a_b,a"));  // a comma can't be in a value
    CHECK_STR(ask(d, "FDQ,1,PACKS"), line("FDP,1,0,2,Recorded") + line("FDP,1,1,2,Placeholder"));
}

TEST(test_duo_refuses_what_it_doesnt_know) {
    FakeDuo d;
    CHECK_STR(ask(d, "FDQ,1,MODEL"), line("FDK,1,MODEL,ERR,unknown"));
    CHECK_STR(ask(d, "FDC,1,FLY"), line("FDK,1,FLY,ERR,unknown"));
    CHECK_STR(ask(d, "SBQ,1,STATUS"), line("FDK,1,SBQ,ERR,unknown"));
    CHECK_STR(ask(d, "FDQ,2,STATUS"), line("FDK,1,REQUEST,ERR,invalid"));
    char out[256];
    uint32_t n = duo::answer(d, 0, "$FDQ,1,STATUS*00", 16, out, sizeof out);
    CHECK_STR(std::string(out, n), line("FDK,1,REQUEST,ERR,invalid"));  // a bad checksum
}

// ---- Through the cable: a virtual PC at cylinder 82 -------------------------------------

struct Rig {
    explicit Rig(const ibm::Format& fmt = ibm::HD_1440, Core0Costs costs = {}, bool writable = true)
        : m(fmt, costs, 0, writable), pc(m), handler(duo) {
        m.unit(0).mailbox.setHandler(&handler);
        pc.motorOn();
        pc.recalibrate();
        pc.detectRate();  // as the BIOS finds a 720 KB disk's 250 kbit/s
    }
    // One exchange as SYNMIND makes it: the request to sector 1, the reply
    // from sectors 5-9, read again until it carries the request's sequence.
    // Returns the reply's text; `reads` the reads it took.
    std::string exchange(uint16_t seq, const char* body, unsigned& reads) {
        std::vector<uint8_t> req = request(seq, body), reply(mailbox::REPLY_SECTORS * mailbox::SECTOR);
        reads = 0;
        if (pc.writeSectors(mailbox::CYLINDER, 0, 1, 1, req.data()) != Status::Ok) return "write failed";
        for (int tries = 0; tries < 5; ++tries) {
            ++reads;
            if (pc.readSectors(mailbox::CYLINDER, 0, 5, 5, reply.data()) != Status::Ok) return "read failed";
            int got;
            std::string text = replyText(reply.data(), got);
            if (got == seq) return text;
        }
        return "no reply";
    }
    std::string exchange(uint16_t seq, const char* body) {
        unsigned reads;
        return exchange(seq, body, reads);
    }

    Machine m;
    VirtualPc pc;
    FakeDuo duo;
    duo::MailboxHandler handler;
};

TEST(test_mailbox_says_hello_before_any_request) {
    // SynthMinder finds the drive by reading alone.
    Rig r;
    std::vector<uint8_t> reply(mailbox::REPLY_SECTORS * mailbox::SECTOR);
    CHECK(r.pc.readSectors(mailbox::CYLINDER, 0, 5, 5, reply.data()) == Status::Ok);
    int seq;
    CHECK_STR(replyText(reply.data(), seq), line("FDK,1,HELLO,OK,"));
    CHECK_EQ(seq, 0);
    // Elsewhere past the disk's end there's nothing, as on a real drive's disk.
    CHECK(r.pc.readSectors(80, 0, 5, 1, reply.data()) == Status::SectorNotFound);
}

TEST(test_mailbox_pc_exchange) {
    Rig r;
    std::vector<uint8_t> before(512), after(512);
    CHECK(r.pc.readSectors(0, 0, 1, 1, before.data()) == Status::Ok);
    CHECK(contains(r.exchange(1, "FDQ,1,STATUS"), line("FDV,1,firmware,9.9.9")));
    CHECK_STR(r.exchange(2, "FDC,1,VOL,40"), line("FDK,1,VOL,OK,"));
    CHECK_EQ(r.duo.vol, 40);
    CHECK_EQ(r.m.unit(0).mailbox.exchanges(), 2u);
    // The user's disk is never written, nor its data moved.
    CHECK_EQ(r.m.source.writes, 0u);
    CHECK(r.pc.readSectors(0, 0, 1, 1, after.data()) == Status::Ok);
    CHECK(before == after);
    CHECK(!r.m.d.writeProtected() || r.m.d.head().cylinder() != mailbox::CYLINDER);
}

TEST(test_mailbox_reply_on_the_first_revolution) {
    // The board's costs: a burst decoded at 1,700 KB/s, its finish 4 ms.
    Core0Costs costs;
    costs.decode_kbps = 1700;
    costs.finish_us = 4000;
    for (const ibm::Format* fmt : {&ibm::HD_1440, &ibm::DD_720}) {
        Rig r(*fmt, costs);
        unsigned reads = 0;
        for (uint16_t seq = 1; seq <= 4; ++seq) {
            CHECK(contains(r.exchange(seq, "FDQ,1,STATUS", reads), "firmware"));
            CHECK_EQ(reads, 1u);
        }
    }
}

TEST(test_mailbox_on_a_write_protected_disk) {
    Rig r(ibm::HD_1440, {}, false);
    CHECK(contains(r.exchange(1, "FDQ,1,STATUS"), "firmware"));
    std::vector<uint8_t> data(512, 0x55);
    CHECK(r.pc.writeSectors(3, 0, 1, 1, data.data()) == Status::WriteProtected);  // the disk itself still is
    CHECK_EQ(r.m.source.writes, 0u);
}

TEST(test_mailbox_runs_a_request_once) {
    Rig r;
    r.duo.two = true;
    std::vector<uint8_t> req = request(5, "FDC,1,INSERT,B,DISK2.IMG");
    CHECK(r.pc.writeSectors(mailbox::CYLINDER, 0, 1, 1, req.data()) == Status::Ok);
    CHECK(r.pc.writeSectors(mailbox::CYLINDER, 0, 1, 1, req.data()) == Status::Ok);  // a retry
    CHECK_EQ(r.duo.inserts, 1u);
    CHECK_EQ(r.m.unit(0).mailbox.repeats(), 1u);
    // The next request, even if it says the same, runs.
    CHECK_STR(r.exchange(6, "FDC,1,INSERT,B,DISK2.IMG"), line("FDK,1,INSERT,OK,"));
    CHECK_EQ(r.duo.inserts, 2u);
}

TEST(test_mailbox_with_no_disk_in) {
    Rig r;
    r.m.eject();
    r.pc.run(10000);
    // On the disk's cylinders the drive is empty: no index, DISK CHANGE.
    std::vector<uint8_t> buf(512);
    CHECK(r.pc.diskChanged());
    CHECK(r.pc.readSectors(0, 0, 1, 1, buf.data()) != Status::Ok);
    // At cylinder 82 the mailbox still answers, and DOS's disk change stays unseen there.
    CHECK(r.pc.seekTo(mailbox::CYLINDER));
    CHECK(!r.pc.diskChanged());
    CHECK(contains(r.exchange(1, "FDQ,1,STATUS"), line("FDV,1,firmware,9.9.9")));
    CHECK(!r.m.d.diskPresent());
    // Back on the disk's cylinders it's empty again: no disk to read. The
    // steps cleared DISK CHANGE, as a BIOS needs before it seeks (the Evesham's
    // steps to cylinder 1 and back, and gives up if the line stays on).
    CHECK(r.pc.seekTo(0));
    CHECK(!r.pc.diskChanged());
    CHECK(r.pc.readSectors(0, 0, 1, 1, buf.data()) != Status::Ok);
    CHECK_EQ(r.m.source.writes, 0u);
    // A disk going in sets it again, so DOS sees the new disk.
    r.m.insert(r.m.source);
    r.pc.run(10000);
    CHECK(r.pc.diskChanged());
    r.m.eject();
    r.pc.run(10000);
    CHECK(r.pc.diskChanged());  // and taking it out
    // Not while a swap fills the cache: then it's empty at 82 too.
    r.m.d.setMailboxWhenEmpty(false);
    CHECK(r.pc.seekTo(mailbox::CYLINDER));
    std::vector<uint8_t> reply(mailbox::REPLY_SECTORS * mailbox::SECTOR);
    CHECK(r.pc.readSectors(mailbox::CYLINDER, 0, 5, 5, reply.data()) != Status::Ok);
}

TEST(test_mailbox_ignores_stray_writes) {
    Rig r;
    std::vector<uint8_t> junk(512, 0xE5);  // a disk tool writing every cylinder
    CHECK(r.pc.writeSectors(mailbox::CYLINDER, 0, 1, 1, junk.data()) == Status::Ok);
    CHECK(r.pc.writeSectors(mailbox::CYLINDER, 0, 6, 1, junk.data()) == Status::Ok);
    CHECK_EQ(r.m.unit(0).mailbox.exchanges(), 0u);
    CHECK_EQ(r.m.unit(0).mailbox.rejected(), 2u);
    CHECK_EQ(r.duo.inserts, 0u);
    for (const BurstResult& w : r.m.writes) CHECK(!lostWrite(w));  // the disk isn't write-protected for it
    CHECK(!r.m.d.writeProtected());
}

}  // namespace

void runMailboxTests() {
    RUN(test_sentence_round_trip);
    RUN(test_mailbox_eligible_disks);
    RUN(test_mailbox_request_sector);
    RUN(test_mailbox_reply_packs_whole_sentences);
    RUN(test_duo_status);
    RUN(test_duo_files_in_pages);
    RUN(test_duo_changes);
    RUN(test_duo_refuses_what_it_doesnt_know);
    RUN(test_mailbox_says_hello_before_any_request);
    RUN(test_mailbox_pc_exchange);
    RUN(test_mailbox_reply_on_the_first_revolution);
    RUN(test_mailbox_on_a_write_protected_disk);
    RUN(test_mailbox_runs_a_request_once);
    RUN(test_mailbox_ignores_stray_writes);
    RUN(test_mailbox_with_no_disk_in);
}
