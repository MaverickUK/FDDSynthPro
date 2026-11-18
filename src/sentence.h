// Sentences as the HDD Synth and SynthMinder write them: `$BODY*CS`, CS the
// XOR of BODY's characters as two hex digits, and BODY's fields separated by
// commas. Pure C++, header-only, host-tested (test_mailbox.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace sentence {

constexpr size_t MAX_FIELDS = 12;

inline uint8_t checksum(const char* body, size_t n) {
    uint8_t cs = 0;
    for (size_t i = 0; i < n; ++i) cs ^= uint8_t(body[i]);
    return cs;
}

// Writes `$body*CS` and a line feed to `out` (room `cap`); returns the bytes
// written, or 0 if it doesn't fit.
inline size_t make(const char* body, size_t n, char* out, size_t cap) {
    if (n + 5 > cap) return 0;
    static const char HEX[] = "0123456789ABCDEF";
    out[0] = '$';
    std::memcpy(out + 1, body, n);
    uint8_t cs = checksum(body, n);
    out[n + 1] = '*';
    out[n + 2] = HEX[cs >> 4];
    out[n + 3] = HEX[cs & 15];
    out[n + 4] = '\n';
    return n + 5;
}

static inline int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// A sentence split into its fields, in place: `$FDC,1,VOL,60*CS` gives
// FDC, 1, VOL, 60. The line may end in CR, LF or both.
struct Parsed {
    const char* field[MAX_FIELDS];
    size_t len[MAX_FIELDS];
    size_t count = 0;

    bool is(size_t i, const char* s) const {
        return i < count && std::strlen(s) == len[i] && std::memcmp(field[i], s, len[i]) == 0;
    }
};

// False unless `line` is `$BODY*CS` with the right checksum.
inline bool parse(const char* line, size_t n, Parsed& out) {
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) --n;
    if (n < 4 || line[0] != '$' || line[n - 3] != '*') return false;
    int hi = hexDigit(line[n - 2]), lo = hexDigit(line[n - 1]);
    if (hi < 0 || lo < 0) return false;
    const char* body = line + 1;
    size_t body_n = n - 4;
    if (checksum(body, body_n) != uint8_t(hi << 4 | lo)) return false;
    out.count = 0;
    size_t start = 0;
    for (size_t i = 0; i <= body_n; ++i) {
        if (i == body_n || body[i] == ',') {
            if (out.count == MAX_FIELDS) return false;
            out.field[out.count] = body + start;
            out.len[out.count] = i - start;
            ++out.count;
            start = i + 1;
        }
    }
    return true;
}

// A value safe to put in a field: `,`, `*` and `$` become `_`, as the synth
// does in pack names. `out` gets at most cap - 1 characters and a NUL.
inline void clean(const char* in, char* out, size_t cap) {
    size_t i = 0;
    for (; in[i] && i + 1 < cap; ++i) out[i] = (in[i] == ',' || in[i] == '*' || in[i] == '$') ? '_' : in[i];
    out[i] = 0;
}

}  // namespace sentence
