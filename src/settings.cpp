#include "settings.h"

#include <cstdio>
#include <cstring>

namespace settings {

bool Settings::operator==(const Settings& o) const {
    for (unsigned i = 0; i < bus::MAX_DRIVES; ++i)
        if (std::strcmp(image[i], o.image[i]) != 0 || std::strcmp(last[i], o.last[i]) != 0) return false;
    return std::strcmp(pack, o.pack) == 0 && volume == o.volume && bus == o.bus;
}

// Copies a value, refusing one too long rather than cutting it short (a cut
// name would name another file, or none).
static void setName(char (&to)[NAME], const char* value, size_t len) {
    if (len >= NAME) return;
    std::memcpy(to, value, len);
    to[len] = 0;
}

// A key, matched whole.
static bool is(const char* line, size_t key, const char* name) {
    return std::strlen(name) == key && std::strncmp(line, name, key) == 0;
}

// One `key=value` line into `s`: true if the key is known and its value
// taken. The names before 0.20.0 (image, image2, last, last2) go into
// `old_image` and `old_last`, so that parse() can let the new names win.
static bool applyLine(Settings& s, const char* line, size_t n, char (*old_image)[NAME], char (*old_last)[NAME]) {
    const char* eq = static_cast<const char*>(std::memchr(line, '=', n));
    if (!eq) return false;
    size_t key = size_t(eq - line);
    const char* value = eq + 1;
    size_t vlen = n - key - 1;
    auto name = [&](char(&to)[NAME]) {
        if (vlen >= NAME) return false;
        setName(to, value, vlen);
        return true;
    };
    if (is(line, key, "device0")) return name(s.image[0]);
    if (is(line, key, "device1")) return name(s.image[1]);
    if (is(line, key, "last0")) return name(s.last[0]);
    if (is(line, key, "last1")) return name(s.last[1]);
    if (is(line, key, "image")) return name(old_image[0]);
    if (is(line, key, "image2")) return name(old_image[1]);
    if (is(line, key, "last")) return name(old_last[0]);
    if (is(line, key, "last2")) return name(old_last[1]);
    if (is(line, key, "pack")) return name(s.pack);
    // The bus: a word each, so a copy that ends at the line's end.
    char word[12] = "";
    if (vlen < sizeof word) {
        std::memcpy(word, value, vlen);
        word[vlen] = 0;
    }
    if (is(line, key, "interface")) return bus::parseInterface(word, s.bus.interface);
    if (is(line, key, "cable")) return bus::parseCable(word, s.bus.cable);
    if (is(line, key, "drive")) return bus::parseUnits(word, s.bus.units);
    if (is(line, key, "pin02")) {
        if (std::strcmp(word, "chg") == 0) s.bus.pin2_chng = true;
        else if (std::strcmp(word, "nc") == 0) s.bus.pin2_chng = false;
        else return false;
        return true;
    }
    if (is(line, key, "volume")) {
        if (vlen == 0 || vlen > 3) return false;
        int v = 0;
        for (size_t i = 0; i < vlen; ++i) {
            if (value[i] < '0' || value[i] > '9') return false;
            v = v * 10 + (value[i] - '0');
        }
        if (v > 100) return false;
        s.volume = v;
        return true;
    }
    return false;
}

Settings parse(const char* text, size_t len) {
    Settings s;
    // The names before 0.20.0: read when the new name isn't there, and
    // written back under the new one.
    char old_image[bus::MAX_DRIVES][NAME] = {};
    char old_last[bus::MAX_DRIVES][NAME] = {};
    size_t at = 0;
    while (at < len) {
        size_t end = at;
        while (end < len && text[end] != '\n') ++end;
        size_t stop = end;
        while (stop > at && (text[stop - 1] == '\r' || text[stop - 1] == ' ')) --stop;  // CRLF, trailing spaces
        applyLine(s, text + at, stop - at, old_image, old_last);
        at = end + 1;
    }
    for (unsigned i = 0; i < bus::MAX_DRIVES; ++i) {
        if (!s.image[i][0]) std::memcpy(s.image[i], old_image[i], NAME);
        if (!s.last[i][0]) std::memcpy(s.last[i], old_last[i], NAME);
    }
    s.bus = bus::normalised(s.bus);
    return s;
}

bool set(Settings& s, const char* line, size_t len) {
    // "key = value" too: the spaces round the key and the value go.
    while (len && (line[len - 1] == ' ' || line[len - 1] == '\r' || line[len - 1] == '\n')) --len;
    while (len && line[0] == ' ') ++line, --len;
    const char* eq = static_cast<const char*>(std::memchr(line, '=', len));
    if (!eq) return false;
    size_t key = size_t(eq - line), v = key + 1;
    while (key && line[key - 1] == ' ') --key;
    while (v < len && line[v] == ' ') ++v;
    char clean[NAME + 16];
    if (key + 1 + (len - v) >= sizeof clean) return false;
    std::memcpy(clean, line, key);
    clean[key] = '=';
    std::memcpy(clean + key + 1, line + v, len - v);
    Settings next = s;
    // An old name sets the new one directly.
    if (!applyLine(next, clean, key + 1 + (len - v), next.image, next.last)) return false;
    next.bus = bus::normalised(next.bus);
    s = next;
    return true;
}

size_t format(const Settings& s, char* out, size_t cap) {
    // `device` lines only where they're set (by hand); last1 only for a
    // second drive; pin02 only when it's set. A simple file stays simple.
    char fixed[bus::MAX_DRIVES][NAME + 12] = {};
    for (unsigned i = 0; i < bus::MAX_DRIVES; ++i)
        if (s.image[i][0]) std::snprintf(fixed[i], sizeof fixed[i], "device%u=%s\n", i, s.image[i]);
    char second[NAME + 8] = "";
    if (s.bus.drives() > 1) std::snprintf(second, sizeof second, "last1=%s\n", s.last[1]);
    char units[12];
    int n = std::snprintf(out, cap, "%s%slast0=%s\n%spack=%s\nvolume=%d\ninterface=%s\ncable=%s\ndrive=%s\n%s",
                          fixed[0], fixed[1], s.last[0], second, s.pack, s.volume,
                          bus::interfaceName(s.bus.interface), bus::cableName(s.bus.cable),
                          bus::unitsName(s.bus, units), s.bus.pin2_chng ? "pin02=chg\n" : "");
    return n < 0 ? 0 : size_t(n) < cap ? size_t(n) : cap - 1;
}

}  // namespace settings
