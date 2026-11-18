#include "mailbox.h"

#include <cstring>

#include "cylinder_loader.h"
#include "sentence.h"

namespace mailbox {

bool eligible(const ibm::Format& fmt) {
    if (fmt.encoding != ibm::ENCODING_IBM || fmt.step != 1 || fmt.cyls > CYLINDER) return false;
    const ibm::Track& t = fmt.kinds[0];
    return !t.fm && t.scale == 1 && t.size_code == 2 && t.first_id == 1 && t.sectors >= FIRST_REPLY + REPLY_SECTORS;
}

static uint16_t get16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
static void put16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}

bool parseRequest(const uint8_t* sector, Request& out) {
    if (std::memcmp(sector, "SMND", 4) != 0 || sector[4] != VERSION) return false;
    uint16_t len = get16(sector + 8);
    if (len == 0 || len > TEXT_MAX) return false;
    out.seq = get16(sector + 6);
    out.len = len;
    std::memcpy(out.text, sector + HEADER, len);
    out.text[len] = 0;
    return true;
}

uint32_t buildReply(uint16_t seq, const char* text, uint32_t len, uint8_t* out) {
    std::memset(out, 0, REPLY_SECTORS * SECTOR);
    uint32_t at = 0, placed = 0;
    unsigned used = 0;
    for (unsigned s = 0; s < REPLY_SECTORS && at < len; ++s) {
        uint8_t* sector = out + s * SECTOR;
        uint32_t n = 0;
        // Whole sentences, up to TEXT_MAX bytes.
        while (at + n < len) {
            const void* nl = std::memchr(text + at + n, '\n', len - at - n);
            uint32_t line = nl ? uint32_t(static_cast<const char*>(nl) - (text + at + n)) + 1 : len - at - n;
            if (n + line > TEXT_MAX) break;
            n += line;
        }
        if (n == 0) break;  // a sentence longer than a sector: left out
        std::memcpy(sector + HEADER, text + at, n);
        put16(sector + 8, uint16_t(n));
        at += n;
        placed += n;
        used = s + 1;
    }
    if (used == 0) used = 1;  // an empty reply still says which request it answers
    for (unsigned s = 0; s < used; ++s) {
        uint8_t* sector = out + s * SECTOR;
        std::memcpy(sector, "SMND", 4);
        sector[4] = VERSION;
        sector[5] = s + 1 < used ? FLAG_MORE : 0;
        put16(sector + 6, seq);
    }
    return placed;
}

static const char HELLO[] = "$FDK,1,HELLO,OK,*";

MailboxSource::MailboxSource(TrackSource& disk, unsigned drive) : disk_(disk), drive_(drive) {
    char line[32];
    size_t body = sizeof HELLO - 3;  // without `$` and `*`
    size_t n = sentence::make(HELLO + 1, body, line, sizeof line);
    buildReply(0, line, uint32_t(n), reply_);
}

const ibm::Format& MailboxSource::format() const {
    const ibm::Format& disk = disk_.format();
    if (!eligible(disk)) return disk;
    extended_ = disk;
    extended_.cyls = CYLINDER + 1;
    return extended_;
}

void MailboxSource::fillTrack(uint8_t head, uint8_t* out) const {
    const ibm::Track& t = disk_.format().kinds[0];
    std::memset(out, 0, t.dataBytes());
    if (head != HEAD) return;
    std::memcpy(out + REQUEST * SECTOR, request_, SECTOR);
    std::memcpy(out + FIRST_REPLY * SECTOR, reply_, sizeof reply_);
}

bool MailboxSource::readTrack(uint8_t cylinder, uint8_t head, uint8_t* out) {
    if (mailboxCylinder(cylinder)) {
        fillTrack(head, out);
        return true;
    }
    if (active() && cylinder >= disk_.format().cyls) return false;  // past the disk's end: blank, as before
    return disk_.readTrack(cylinder, head, out);
}

void MailboxSource::readCylinder(uint8_t cylinder, uint8_t* out0, uint8_t* out1, bool ok[2]) {
    if (mailboxCylinder(cylinder)) {
        fillTrack(0, out0);
        fillTrack(1, out1);
        ok[0] = ok[1] = true;
        return;
    }
    if (active() && cylinder >= disk_.format().cyls) {
        ok[0] = ok[1] = false;
        return;
    }
    disk_.readCylinder(cylinder, out0, out1, ok);
}

bool MailboxSource::writeSector(uint8_t cylinder, uint8_t head, uint8_t sector, const uint8_t* data) {
    if (!active() || cylinder < disk_.format().cyls) return disk_.writeSector(cylinder, head, sector, data);
    // Past the disk's end. Taken, so the write isn't counted as lost (which
    // would write-protect the disk), and kept nowhere: before the mailbox a
    // write there matched no track and was dropped, too.
    if (cylinder == CYLINDER && head == HEAD && sector == REQUEST) take(data);
    else if (cylinder == CYLINDER) ++rejected_;
    return true;
}

void MailboxSource::take(const uint8_t* sector) {
    Request req;
    if (!parseRequest(sector, req)) {
        ++rejected_;
        return;
    }
    // The PC may write the same request again (a retry after an error):
    // answered already, so not run twice.
    if (answered_ && req.seq == last_seq_ && std::memcmp(sector, request_, SECTOR) == 0) {
        ++repeats_;
        return;
    }
    std::memcpy(request_, sector, SECTOR);
    uint32_t n = handler_ ? handler_->answer(drive_, req.text, req.len, text_, sizeof text_) : 0;
    buildReply(req.seq, text_, n, reply_);
    last_seq_ = req.seq;
    answered_ = true;
    ++exchanges_;
    // Into the loaded track at once: the writer re-encodes the head after
    // this write, so the reply goes out with it.
    if (loader_)
        for (uint8_t s = 0; s < REPLY_SECTORS; ++s)
            loader_->storeSector(CYLINDER, HEAD, uint8_t(FIRST_REPLY + s), reply_ + s * SECTOR);
}

}  // namespace mailbox
