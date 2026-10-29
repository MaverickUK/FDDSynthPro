#include "settings.h"

#include <cstdio>
#include <cstring>

namespace settings {

bool Settings::operator==(const Settings& o) const {
    for (unsigned i = 0; i < bus::MAX_DRIVES; ++i)
        if (std::strcmp(image[i], o.image[i]) != 0) return false;
    return std::strcmp(pack, o.pack) == 0 && volume == o.volume && bus == o.bus;
}

// Copies a value, refusing one too long rather than cutting it short (a cut
// name would name another file, or none).
static void setName(char (&to)[NAME], const char* value, size_t len) {
    if (len >= NAME) return;
    std::memcpy(to, value, len);
    to[len] = 0;
}

Settings parse(const char* text, size_t len) {
    Settings s;
    size_t at = 0;
    while (at < len) {
        size_t end = at;
        while (end < len && text[end] != '\n') ++end;
        size_t stop = end;
        while (stop > at && (text[stop - 1] == '\r' || text[stop - 1] == ' ')) --stop;  // CRLF, trailing spaces
        const char* line = text + at;
        size_t n = stop - at;
        const char* eq = static_cast<const char*>(std::memchr(line, '=', n));
        if (eq) {
            size_t key = size_t(eq - line);
            const char* value = eq + 1;
            size_t vlen = n - key - 1;
            if (key == 5 && std::strncmp(line, "image", 5) == 0) setName(s.image[0], value, vlen);
            if (key == 6 && std::strncmp(line, "image2", 6) == 0) setName(s.image[1], value, vlen);
            if (key == 4 && std::strncmp(line, "pack", 4) == 0) setName(s.pack, value, vlen);
            // The bus: a word each, so a copy that ends at the line's end.
            char word[12] = "";
            if (vlen < sizeof word) {
                std::memcpy(word, value, vlen);
                word[vlen] = 0;
            }
            if (key == 9 && std::strncmp(line, "interface", 9) == 0) bus::parseInterface(word, s.bus.interface);
            if (key == 5 && std::strncmp(line, "cable", 5) == 0) bus::parseCable(word, s.bus.cable);
            if (key == 5 && std::strncmp(line, "drive", 5) == 0) bus::parseUnits(word, s.bus.units);
            if (key == 5 && std::strncmp(line, "pin02", 5) == 0) {
                if (std::strcmp(word, "chg") == 0) s.bus.pin2_chng = true;
                if (std::strcmp(word, "nc") == 0) s.bus.pin2_chng = false;
            }
            if (key == 6 && std::strncmp(line, "volume", 6) == 0 && vlen > 0 && vlen <= 3) {
                int v = 0;
                bool digits = true;
                for (size_t i = 0; i < vlen; ++i) {
                    digits = digits && value[i] >= '0' && value[i] <= '9';
                    v = v * 10 + (value[i] - '0');
                }
                if (digits && v <= 100) s.volume = v;
            }
        }
        at = end + 1;
    }
    s.bus = bus::normalised(s.bus);
    return s;
}

size_t format(const Settings& s, char* out, size_t cap) {
    // image2 only for a second drive, so a one-drive file stays as it was.
    char second[NAME + 8] = "";
    if (s.bus.drives() > 1) std::snprintf(second, sizeof second, "image2=%s\n", s.image[1]);
    char units[12];
    // pin02 only when it's set, so files from before it stay as they were.
    int n = std::snprintf(out, cap, "image=%s\n%spack=%s\nvolume=%d\ninterface=%s\ncable=%s\ndrive=%s\n%s",
                          s.image[0], second, s.pack, s.volume, bus::interfaceName(s.bus.interface),
                          bus::cableName(s.bus.cable), bus::unitsName(s.bus, units), s.bus.pin2_chng ? "pin02=chg\n" : "");
    return n < 0 ? 0 : size_t(n) < cap ? size_t(n) : cap - 1;
}

}  // namespace settings
