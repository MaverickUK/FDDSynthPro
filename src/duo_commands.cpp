#include "duo_commands.h"

#include <strings.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "image_list.h"
#include "sentence.h"

namespace duo {

namespace {

// Builds the reply, a sentence at a time; a sentence that doesn't fit is
// left out (full() then says so).
class Reply {
public:
    Reply(char* out, uint32_t cap) : out_(out), cap_(cap) {}
    // `body` without `$` and checksum: "FDV,1,firmware,0.25.0".
    bool add(const char* body) {
        size_t n = sentence::make(body, std::strlen(body), out_ + len_, cap_ - len_);
        if (!n) {
            full_ = true;
            return false;
        }
        len_ += uint32_t(n);
        return true;
    }
    template <typename... Args>
    bool addf(const char* fmt, Args... args) {
        char body[160];
        std::snprintf(body, sizeof body, fmt, args...);
        return add(body);
    }
    uint32_t len() const { return len_; }
    bool full() const { return full_; }

private:
    char* out_;
    uint32_t cap_;
    uint32_t len_ = 0;
    bool full_ = false;
};

struct Clean {
    explicit Clean(const char* s) { sentence::clean(s ? s : "", text, sizeof text); }
    char text[72];
};

// A field as a NUL-terminated string.
struct Field {
    Field(const sentence::Parsed& p, size_t i) {
        size_t n = i < p.count ? p.len[i] : 0;
        if (n >= sizeof text) n = sizeof text - 1;
        if (n) std::memcpy(text, p.field[i], n);
        text[n] = 0;
    }
    char text[72];
};

void ack(Reply& r, const char* what, const char* reason) {
    if (reason)
        r.addf("FDK,1,%s,ERR,%s", what, reason);
    else
        r.addf("FDK,1,%s,OK,", what);
}

// "A" or "B" (any case) to the drive with that letter, or -1.
int driveByLetter(const Duo& duo, const sentence::Parsed& p, size_t i) {
    if (i >= p.count || p.len[i] != 1) return -1;
    char want = p.field[i][0] & ~0x20;
    for (unsigned d = 0; d < duo.drives(); ++d) {
        DriveInfo info;
        duo.driveInfo(d, info);
        if (info.letter == want) return int(d);
    }
    return -1;
}

void status(Duo& duo, unsigned drive, Reply& r) {
    r.addf("FDV,1,firmware,%s", Clean(duo.firmware()).text);
    char letters[4] = {};
    DriveInfo asked;
    duo.driveInfo(drive, asked);
    for (unsigned d = 0; d < duo.drives() && d < 3; ++d) {
        DriveInfo info;
        duo.driveInfo(d, info);
        letters[d] = info.letter;
    }
    r.addf("FDV,1,drives,%s", letters);
    r.addf("FDV,1,mailbox,%c", asked.letter);
    r.addf("FDV,1,volume,%d", duo.volume());
    if (duo.packCount()) {
        r.addf("FDV,1,pack,%s", Clean(duo.packName(duo.packIndex())).text);
        r.addf("FDV,1,pack_index,%u", unsigned(duo.packIndex()));
    }
    for (unsigned d = 0; d < duo.drives(); ++d) {
        DriveInfo info;
        duo.driveInfo(d, info);
        char k = char(info.letter | 0x20);
        r.addf("FDV,1,%c_disk,%s", k, info.disk_in ? Clean(info.disk).text : "-");
        if (!info.disk_in) continue;
        r.addf("FDV,1,%c_kb,%u", k, unsigned(info.kb));
        r.addf("FDV,1,%c_format,%s", k, Clean(info.format).text);
        r.addf("FDV,1,%c_cylinder,%u", k, info.cylinder);
        r.addf("FDV,1,%c_motor,%s", k, info.motor ? "on" : "off");
        r.addf("FDV,1,%c_protect,%s", k, info.write_protected ? "on" : "off");
    }
}

void files(Duo& duo, const sentence::Parsed& p, Reply& r) {
    size_t from = p.count > 3 ? size_t(std::strtoul(Field(p, 3).text, nullptr, 10)) : 0;
    size_t count = duo.imageCount();
    if (from >= count) {
        r.addf("FDF,1,%u,%u,-,0,-", unsigned(count), unsigned(count));  // none from there
        return;
    }
    for (size_t i = from; i < count; ++i) {
        Clean name(duo.imageName(i));
        if (!r.addf("FDF,1,%u,%u,%s,%u,%c", unsigned(i), unsigned(count), name.text, unsigned(duo.imageKB(i)),
                    imageKind(name.text)))
            break;
    }
}

void packs(Duo& duo, Reply& r) {
    size_t count = duo.packCount();
    for (size_t i = 0; i < count; ++i)
        if (!r.addf("FDP,1,%u,%u,%s", unsigned(i), unsigned(count), Clean(duo.packName(i)).text)) break;
}

void settingsList(Duo& duo, Reply& r) {
    for (size_t i = 0; i < duo.settingCount(); ++i) {
        Setting s = duo.setting(i);
        Clean next(s.next), now(s.now);
        if (!r.addf("FDS,1,%s,%s,%s", s.key, next.text[0] ? next.text : "-", now.text[0] ? now.text : "-")) break;
    }
}

bool number(const sentence::Parsed& p, size_t i, long& out) {
    if (i >= p.count || p.len[i] == 0) return false;
    Field f(p, i);
    char* end = nullptr;
    out = std::strtol(f.text, &end, 10);
    return end && *end == 0;
}

size_t findImage(const Duo& duo, const char* name) {
    for (size_t i = 0; i < duo.imageCount(); ++i)
        if (strcasecmp(duo.imageName(i), name) == 0) return i;
    return duo.imageCount();
}

void change(Duo& duo, unsigned drive, const sentence::Parsed& p, Reply& r) {
    Field what(p, 2);
    if (p.is(2, "INSERT")) {
        int d = driveByLetter(duo, p, 3);
        if (d < 0) return ack(r, what.text, "nodrive");
        size_t image = findImage(duo, Field(p, 4).text);
        if (image == duo.imageCount()) return ack(r, what.text, "notfound");
        return ack(r, what.text, duo.insert(unsigned(d), image, drive));
    }
    if (p.is(2, "EJECT")) {
        int d = driveByLetter(duo, p, 3);
        if (d < 0) return ack(r, what.text, "nodrive");
        return ack(r, what.text, duo.eject(unsigned(d), drive));
    }
    long v;
    if (p.is(2, "PACK")) {
        if (!number(p, 3, v) || v < 0 || size_t(v) >= duo.packCount()) return ack(r, what.text, "range");
        return ack(r, what.text, duo.setPack(size_t(v)));
    }
    if (p.is(2, "VOL")) {
        if (!number(p, 3, v) || v < 0 || v > 100) return ack(r, what.text, "range");
        return ack(r, what.text, duo.setVolume(int(v)));
    }
    if (p.is(2, "SET")) {
        if (p.count < 5) return ack(r, what.text, "value");
        // A value with commas in (`drive` = `a,b`) arrives as several fields: joined again.
        char value[72] = "";
        for (size_t i = 4; i < p.count; ++i) {
            if (i > 4) std::strncat(value, ",", sizeof value - std::strlen(value) - 1);
            std::strncat(value, Field(p, i).text, sizeof value - std::strlen(value) - 1);
        }
        return ack(r, what.text, duo.set(Field(p, 3).text, value));
    }
    ack(r, what.text, "unknown");
}

}  // namespace

uint32_t answer(Duo& duo, unsigned drive, const char* request, uint32_t n, char* out, uint32_t cap) {
    Reply r(out, cap);
    sentence::Parsed p;
    // The request may carry more than one line (version 1 sends one): the first is taken.
    const void* nl = std::memchr(request, '\n', n);
    uint32_t line = nl ? uint32_t(static_cast<const char*>(nl) - request) : n;
    if (!sentence::parse(request, line, p) || p.count < 3 || !p.is(1, "1")) {
        ack(r, "REQUEST", "invalid");
        return r.len();
    }
    if (p.is(0, "FDQ")) {
        if (p.is(2, "STATUS"))
            status(duo, drive, r);
        else if (p.is(2, "FILES"))
            files(duo, p, r);
        else if (p.is(2, "PACKS"))
            packs(duo, r);
        else if (p.is(2, "SETTINGS"))
            settingsList(duo, r);
        else
            ack(r, Field(p, 2).text, "unknown");
    } else if (p.is(0, "FDC")) {
        change(duo, drive, p, r);
    } else {
        ack(r, Field(p, 0).text, "unknown");
    }
    return r.len();
}

void describe(const ibm::Format& fmt, char* out, size_t cap) {
    if (fmt.encoding == ibm::ENCODING_RAW) {
        std::snprintf(out, cap, "flux %u rpm", unsigned(fmt.rpm));
        return;
    }
    if (fmt.encoding == ibm::ENCODING_AMIGA) {
        std::snprintf(out, cap, "Amiga %s", fmt.rate >= 500000 ? "HD" : "DD");
        return;
    }
    uint32_t bytes = 0;
    for (uint8_t c = 0; c < fmt.cyls; ++c)
        for (uint8_t h = 0; h < fmt.heads; ++h) bytes += fmt.track(c, h).dataBytes();
    std::snprintf(out, cap, "%u KB %s %u rpm", unsigned(bytes / 1024), fmt.kinds[0].fm ? "FM" : "MFM",
                  unsigned(fmt.rpm));
}

char imageKind(const char* name) {
    if (ImageList::hfeName(name)) return 'H';
    if (ImageList::adfName(name)) return 'A';
    return 'I';
}

}  // namespace duo
