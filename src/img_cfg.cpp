#include "img_cfg.h"

#include <strings.h>

#include <cstring>

namespace img_cfg {

namespace {

// A line's text, without comments and the spaces round it.
struct Line {
    const char* text;
    size_t len;
};

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

Line trim(const char* at, size_t len) {
    const char* hash = static_cast<const char*>(std::memchr(at, '#', len));
    if (hash) len = size_t(hash - at);
    while (len && isSpace(*at)) ++at, --len;
    while (len && isSpace(at[len - 1])) --len;
    return Line{at, len};
}

bool equalsNoCase(const char* a, size_t alen, const char* b) {
    return std::strlen(b) == alen && strncasecmp(a, b, alen) == 0;
}

// A whole number, decimal or 0x hex, in [lo, hi].
bool number(const char* s, size_t len, long lo, long hi, long& out) {
    if (!len) return false;
    int base = 10;
    if (len > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) base = 16, s += 2, len -= 2;
    long v = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = s[i];
        int d = c >= '0' && c <= '9' ? c - '0' : base == 16 && c >= 'a' && c <= 'f' ? c - 'a' + 10
                                               : base == 16 && c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                                                    : -1;
        if (d < 0) return false;
        v = v * base + d;
        if (v > hi) return false;
    }
    if (v < lo) return false;
    out = v;
    return true;
}

// A section header's tag and size.
struct Header {
    const char* tag;
    size_t tag_len;
    uint32_t size;  // 0: any
    bool valid;
};

Header header(Line l) {
    Header h{l.text + 1, 0, 0, false};
    if (l.len < 2 || l.text[0] != '[' || l.text[l.len - 1] != ']') return h;
    const char* inside = l.text + 1;
    size_t n = l.len - 2;
    const char* sep = nullptr;
    for (size_t i = 0; i + 1 < n; ++i)
        if (inside[i] == ':' && inside[i + 1] == ':') sep = inside + i;
    h.tag_len = sep ? size_t(sep - inside) : n;
    while (h.tag_len && isSpace(inside[h.tag_len - 1])) --h.tag_len;
    if (sep) {
        long size;
        Line s = trim(sep + 2, size_t(inside + n - (sep + 2)));
        if (!number(s.text, s.len, 1, 0x7FFFFFFF, size)) return h;
        h.size = uint32_t(size);
    }
    h.valid = true;
    return h;
}

// Calls `each(line, number)` for every non-blank line; stops if it returns false.
template <typename Each>
void lines(const char* text, size_t len, Each each) {
    unsigned number = 0;
    for (size_t at = 0; at < len;) {
        size_t end = at;
        while (end < len && text[end] != '\n') ++end;
        ++number;
        Line l = trim(text + at, end - at);
        if (l.len && !each(l, number)) return;
        at = end + 1;
    }
}

// One `tracks =` sub-section: which tracks, and their parameters.
struct Override {
    ibm::TrackSpec spec;
    char list[64];
};

struct Parse {
    Match m;
    unsigned cyls = 0, heads = 0, step = 1;
    uint8_t layout = 0;
    long rpm = -1;  // -1: not given
    ibm::TrackSpec base;
    Override overrides[ibm::MAX_KINDS - 1];
    unsigned override_count = 0;

    bool fail(Result r, const char* why, unsigned line) {
        m.result = r;
        m.why = why;
        m.line = line;
        return false;
    }
};

// One track-scope `key = value` into `spec`. False with the reason set.
bool trackParam(Parse& p, ibm::TrackSpec& spec, bool sub, const char* key, size_t klen, Line v, unsigned line) {
    long n;
    auto is = [&](const char* k) { return equalsNoCase(key, klen, k); };
    auto autoOr = [&](int& field, long lo, long hi) {
        if (equalsNoCase(v.text, v.len, "auto")) {
            field = -1;
            return true;
        }
        if (!number(v.text, v.len, lo, hi, n)) return p.fail(Result::Bad, "bad value", line);
        field = int(n);
        return true;
    };
    if (is("secs")) {
        if (!number(v.text, v.len, 0, ibm::MAX_SECTORS, n)) return p.fail(Result::Bad, "bad secs", line);
        spec.sectors = int(n);
    } else if (is("bps")) {
        // One size, or a list (one per sector) of a single size.
        int code = -1;
        for (size_t at = 0; at <= v.len;) {
            size_t end = at;
            while (end < v.len && v.text[end] != ',') ++end;
            Line item = trim(v.text + at, end - at);
            int c = 0;
            if (!number(item.text, item.len, 128, 8192, n)) return p.fail(Result::Bad, "bad bps", line);
            while ((128L << c) != n && c < 7) ++c;
            if ((128L << c) != n) return p.fail(Result::Bad, "bps is not a power of two", line);
            if (code >= 0 && c != code) return p.fail(Result::Unsupported, "mixed sector sizes", line);
            code = c;
            at = end + 1;
        }
        spec.size_code = code;
    } else if (is("id")) {
        if (!number(v.text, v.len, 0, 255, n)) return p.fail(Result::Bad, "bad id", line);
        spec.first_id = int(n);
    } else if (is("h")) {
        return autoOr(spec.head_id, 0, 1);
    } else if (is("interleave")) {
        if (!number(v.text, v.len, 1, 255, n)) return p.fail(Result::Bad, "bad interleave", line);
        spec.interleave = int(n);
    } else if (is("cskew") || is("hskew")) {
        if (!number(v.text, v.len, 0, 255, n)) return p.fail(Result::Bad, "bad skew", line);
        (is("cskew") ? spec.cskew : spec.hskew) = int(n);
    } else if (is("gap1") || is("gap2") || is("gap3") || is("gap4a")) {
        int& gap = is("gap1") ? spec.gap1 : is("gap2") ? spec.gap2 : is("gap3") ? spec.gap3 : spec.gap4a;
        return autoOr(gap, 0, 255);
    } else if (is("iam")) {
        if (equalsNoCase(v.text, v.len, "yes")) spec.iam = true;
        else if (equalsNoCase(v.text, v.len, "no")) spec.iam = false;
        else return p.fail(Result::Bad, "iam is yes or no", line);
    } else if (is("mode")) {
        if (equalsNoCase(v.text, v.len, "fm")) spec.fm = true;
        else if (equalsNoCase(v.text, v.len, "mfm")) spec.fm = false;
        else return p.fail(Result::Bad, "mode is fm or mfm", line);
    } else if (is("rate")) {
        // kbit/s, 0 for auto. A track may run at half the disk's rate.
        if (!number(v.text, v.len, 0, 2000, n)) return p.fail(Result::Bad, "bad rate", line);
        spec.rate = int(n) * 1000;
    } else if (is("rpm")) {
        if (!number(v.text, v.len, 1, 2000, n)) return p.fail(Result::Bad, "bad rpm", line);
        // One speed for the whole disk: the drive spins at one.
        if (sub && p.rpm >= 0 && p.rpm != n) return p.fail(Result::Unsupported, "an rpm for some tracks", line);
        p.rpm = n;
    } else if (is("img_bps")) {
        if (!number(v.text, v.len, 0, 8192, n)) return p.fail(Result::Bad, "bad img_bps", line);
        if (n && (spec.size_code < 0 || n != (128L << spec.size_code)))
            return p.fail(Result::Unsupported, "img_bps padding", line);
    } else {
        return p.fail(Result::Bad, "unknown parameter", line);
    }
    return true;
}

bool diskParam(Parse& p, const char* key, size_t klen, Line v, unsigned line) {
    long n;
    if (equalsNoCase(key, klen, "cyls")) {
        if (!number(v.text, v.len, 1, ibm::MAX_CYLINDERS, n)) return p.fail(Result::Bad, "bad cyls", line);
        p.cyls = unsigned(n);
    } else if (equalsNoCase(key, klen, "heads")) {
        if (!number(v.text, v.len, 1, 2, n)) return p.fail(Result::Bad, "bad heads", line);
        p.heads = unsigned(n);
    } else if (equalsNoCase(key, klen, "step")) {
        if (!number(v.text, v.len, 1, 4, n)) return p.fail(Result::Bad, "bad step", line);
        p.step = unsigned(n);
    } else if (equalsNoCase(key, klen, "file-layout")) {
        for (size_t at = 0; at <= v.len;) {
            size_t end = at;
            while (end < v.len && v.text[end] != ',') ++end;
            Line item = trim(v.text + at, end - at);
            if (equalsNoCase(item.text, item.len, "interleaved")) {}
            else if (equalsNoCase(item.text, item.len, "sequential")) p.layout |= ibm::LAYOUT_SEQUENTIAL;
            else if (equalsNoCase(item.text, item.len, "sides-swapped")) p.layout |= ibm::LAYOUT_SIDES_SWAPPED;
            else if (equalsNoCase(item.text, item.len, "reverse-side0")) p.layout |= ibm::LAYOUT_REVERSE_SIDE0;
            else if (equalsNoCase(item.text, item.len, "reverse-side1")) p.layout |= ibm::LAYOUT_REVERSE_SIDE1;
            else return p.fail(Result::Bad, "bad file-layout", line);
            at = end + 1;
        }
    } else {
        return false;  // not a disk parameter
    }
    return true;
}

// Marks the tracks a `tracks =` list names with `kind`. False if it's malformed.
bool applyList(const char* list, const Parse& p, uint8_t kind, ibm::Format& f) {
    size_t len = std::strlen(list);
    for (size_t at = 0; at <= len;) {
        size_t end = at;
        while (end < len && list[end] != ',') ++end;
        Line item = trim(list + at, end - at);
        const char* dot = static_cast<const char*>(std::memchr(item.text, '.', item.len));
        size_t cyl_len = dot ? size_t(dot - item.text) : item.len;
        int head = -1;
        long n, lo, hi;
        if (dot) {
            if (!number(dot + 1, item.len - cyl_len - 1, 0, 1, n)) return false;
            head = int(n);
        }
        const char* dash = static_cast<const char*>(std::memchr(item.text, '-', cyl_len));
        if (dash) {
            if (!number(item.text, size_t(dash - item.text), 0, 254, lo) ||
                !number(dash + 1, cyl_len - size_t(dash - item.text) - 1, 0, 254, hi) || hi < lo)
                return false;
        } else {
            if (!number(item.text, cyl_len, 0, 254, lo)) return false;
            hi = lo;
        }
        for (long c = lo; c <= hi && c < long(p.cyls); ++c)
            for (int h = 0; h < 2; ++h)
                if (head < 0 || head == h) f.kind_of[c][h] = kind;
        at = end + 1;
    }
    return true;
}

}  // namespace

void imageTag(const char* name, char* out, size_t cap) {
    out[0] = 0;
    const char* ext = std::strrchr(name, '.');
    if (!ext) return;
    const char* start = ext;
    while (start > name && start[-1] != '.') --start;
    if (start == name) return;  // one dot: no tag
    size_t n = size_t(ext - start);
    if (n >= cap) n = cap - 1;
    for (size_t i = 0; i < n; ++i) out[i] = char(start[i] >= 'A' && start[i] <= 'Z' ? start[i] - 'A' + 'a' : start[i]);
    out[n] = 0;
}

Match find(const char* text, size_t len, const char* name, uint32_t size, ibm::Format& out) {
    char tag[32];
    imageTag(name, tag, sizeof tag);
    // Is the image's tag one a section names? If not, it counts as untagged.
    bool tag_known = false;
    if (tag[0])
        lines(text, len, [&](Line l, unsigned) {
            Header h = header(l);
            if (h.valid && h.tag_len && equalsNoCase(h.tag, h.tag_len, tag)) tag_known = true;
            return !tag_known;
        });

    // The first section that matches.
    Parse p;
    bool in_section = false;
    unsigned section_line = 0;
    ibm::TrackSpec* spec = &p.base;
    lines(text, len, [&](Line l, unsigned number) {
        if (l.text[0] == '[') {
            if (in_section) return false;  // the end of the one that matched
            Header h = header(l);
            bool tag_ok = h.tag_len ? tag[0] && equalsNoCase(h.tag, h.tag_len, tag) : !tag[0] || !tag_known;
            in_section = h.valid && tag_ok && (!h.size || h.size == size);
            section_line = number;
            return true;
        }
        if (!in_section) return true;
        const char* eq = static_cast<const char*>(std::memchr(l.text, '=', l.len));
        if (!eq) return p.fail(Result::Bad, "expected key = value", number);
        Line key = trim(l.text, size_t(eq - l.text));
        Line value = trim(eq + 1, size_t(l.text + l.len - eq - 1));
        if (equalsNoCase(key.text, key.len, "tracks")) {
            if (p.override_count == ibm::MAX_KINDS - 1)
                return p.fail(Result::Unsupported, "too many tracks sections", number);
            Override& o = p.overrides[p.override_count++];
            o.spec = p.base;  // what the whole disk has, then this sub-section's own
            if (value.len >= sizeof o.list) return p.fail(Result::Bad, "tracks list too long", number);
            std::memcpy(o.list, value.text, value.len);
            o.list[value.len] = 0;
            spec = &o.spec;
            return true;
        }
        if (spec == &p.base && diskParam(p, key.text, key.len, value, number)) return true;
        if (p.m.result != Result::NoMatch) return false;  // diskParam failed
        return trackParam(p, *spec, spec != &p.base, key.text, key.len, value, number);
    });
    if (p.m.result != Result::NoMatch) return p.m;  // a failure inside the section
    if (!in_section) return p.m;                     // no section matched

    const unsigned first_line = section_line;  // later errors are the section's
    if (!p.cyls || !p.heads) {
        p.fail(Result::Bad, "cyls and heads are required", first_line);
        return p.m;
    }
    auto needsSize = [](const ibm::TrackSpec& s) { return s.sectors > 0 && s.size_code < 0; };
    if (needsSize(p.base)) {
        p.fail(Result::Bad, "bps is required", first_line);
        return p.m;
    }
    for (unsigned i = 0; i < p.override_count; ++i)
        if (needsSize(p.overrides[i].spec)) {
            p.fail(Result::Bad, "bps is required", first_line);
            return p.m;
        }

    // Each layout's own rate (as given, or auto); the disk's is the fastest,
    // and the streamer sends a track at half of it with each cell doubled.
    uint32_t rpm = p.rpm > 0 ? uint32_t(p.rpm) : 300;
    ibm::TrackSpec* specs[ibm::MAX_KINDS];
    uint32_t rates[ibm::MAX_KINDS];
    unsigned kinds = 1 + p.override_count;
    specs[0] = &p.base;
    for (unsigned i = 0; i < p.override_count; ++i) specs[i + 1] = &p.overrides[i].spec;
    uint32_t rate = 0;
    for (unsigned k = 0; k < kinds; ++k) {
        rates[k] = specs[k]->rate > 0 ? uint32_t(specs[k]->rate) : ibm::autoRate(*specs[k], rpm);
        if (rates[k] > rate) rate = rates[k];
    }

    ibm::Format f{};
    f.cyls = uint8_t(p.cyls);
    f.heads = uint8_t(p.heads);
    f.step = uint8_t(p.step);
    f.file_layout = p.layout;
    f.rpm = uint16_t(rpm);
    f.rate = rate;
    f.kind_count = uint8_t(kinds);
    if (!ibm::resolve(p.base, rates[0], rpm, f.kinds[0])) {
        p.fail(Result::Bad, "the layout doesn't fit a track", first_line);
        return p.m;
    }
    for (unsigned i = 0; i < p.override_count; ++i) {
        if (!ibm::resolve(p.overrides[i].spec, rates[i + 1], rpm, f.kinds[i + 1]) ||
            !applyList(p.overrides[i].list, p, uint8_t(i + 1), f)) {
            p.fail(Result::Bad, "bad tracks section", first_line);
            return p.m;
        }
    }
    // Each layout in use runs at the disk's rate or half of it.
    bool used[ibm::MAX_KINDS] = {};
    for (unsigned c = 0; c < p.cyls; ++c)
        for (unsigned h = 0; h < p.heads; ++h) used[f.kind_of[c][h]] = true;
    for (unsigned k = 0; k < kinds; ++k) {
        if (rates[k] == rate || !used[k]) continue;
        if (rates[k] * 2 != rate) {
            p.fail(Result::Unsupported, "tracks at rates other than the disk's or half of it", first_line);
            return p.m;
        }
        f.kinds[k].scale = 2;
    }
    if (!f.finish()) {
        p.fail(Result::Bad, "the layout doesn't fit a track", first_line);
        return p.m;
    }
    out = f;
    p.m.result = Result::Ok;
    return p.m;
}

}  // namespace img_cfg
