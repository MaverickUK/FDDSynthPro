#include "settings.h"

#include <cstdio>
#include <cstring>

namespace settings {

bool Settings::operator==(const Settings& o) const {
    return std::strcmp(image, o.image) == 0 && std::strcmp(pack, o.pack) == 0 && volume == o.volume;
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
            if (key == 5 && std::strncmp(line, "image", 5) == 0) setName(s.image, value, vlen);
            if (key == 4 && std::strncmp(line, "pack", 4) == 0) setName(s.pack, value, vlen);
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
    return s;
}

size_t format(const Settings& s, char* out, size_t cap) {
    int n = std::snprintf(out, cap, "image=%s\npack=%s\nvolume=%d\n", s.image, s.pack, s.volume);
    return n < 0 ? 0 : size_t(n) < cap ? size_t(n) : cap - 1;
}

}  // namespace settings
