#include "screens.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "font5x7.h"

namespace screens {

// ---- Drawing helpers ------------------------------------------------------------------

static void title(Canvas& c, const char* t) {
    char s[22];
    std::strncpy(s, t, 21);
    s[21] = 0;
    c.fillRect(0, 0, Canvas::WIDTH, TITLE_H);
    int x = (Canvas::WIDTH - Canvas::textWidth(s)) / 2;
    c.text(x < 0 ? 0 : x, 1, s, false);
}

// The 5x7 font, scaled up: each pixel a scale x scale block.
static int bigText(Canvas& c, int x, int y, const char* s, int scale, bool on = true) {
    for (; *s; ++s) {
        char ch = *s;
        if (ch < FONT5X7_FIRST || ch > FONT5X7_LAST) ch = '?';
        const uint8_t* glyph = &FONT5X7[(ch - FONT5X7_FIRST) * 5];
        for (int col = 0; col < 5; ++col)
            for (int row = 0; row < 7; ++row)
                if (glyph[col] & (1u << row)) c.fillRect(x + col * scale, y + row * scale, scale, scale, on);
        x += 6 * scale;
    }
    return x;
}

static void rightText(Canvas& c, int right, int y, const char* s, bool on = true) {
    c.text(right - Canvas::textWidth(s), y, s, on);
}

static void line(Canvas& c, int x0, int y0, int x1, int y1, bool on = true) {
    int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        c.pixel(x0, y0, on);
        if (x0 == x1 && y0 == y1) return;
        int e2 = 2 * err;
        if (e2 >= dy) err += dy, x0 += sx;
        if (e2 <= dx) err += dx, y0 += sy;
    }
}

static void circle(Canvas& c, int cx, int cy, int r, bool fill = false, bool on = true) {
    for (int y = -r; y <= r; ++y)
        for (int x = -r; x <= r; ++x) {
            int d = x * x + y * y;
            if (fill ? d <= r * r + r : (d >= r * r - r && d <= r * r + r)) c.pixel(cx + x, cy + y, on);
        }
}

static void frame(Canvas& c, int x, int y, int w, int h, bool on = true) {
    c.hLine(x, y, w, on);
    c.hLine(x, y + h - 1, w, on);
    c.vLine(x, y, h, on);
    c.vLine(x + w - 1, y, h, on);
}

// A 3.5-inch disk, 26 x 28: the case with its cut corner, the metal shutter
// with its window at the top, and the label below.
static void floppy(Canvas& c, int x, int y, bool on = true) {
    const int w = 26, h = 28;
    frame(c, x, y, w, h, on);
    for (int i = 0; i < 4; ++i) c.pixel(x + w - 1 - i, y + i, !on);  // the cut corner
    line(c, x + w - 5, y, x + w - 1, y + 4, on);
    c.fillRect(x + 6, y, 13, 10, on);       // the shutter
    c.fillRect(x + 13, y + 2, 3, 6, !on);   // its window
    frame(c, x + 4, y + 14, w - 8, h - 16, on);  // the label
    for (int i = 0; i < 3; ++i) c.hLine(x + 7, y + 17 + i * 3, w - 14, on);
}

static void ejectSymbol(Canvas& c, int cx, int cy) {
    for (int i = 0; i < 6; ++i) c.hLine(cx - i, cy - 6 + i, 2 * i + 1);
    c.fillRect(cx - 6, cy + 2, 13, 3);
}

static const char* activityName(const ui::Status& s) {
    if (!s.disk_in) return "NO DISK";
    switch (s.activity) {
        case ui::Activity::Read: return "READING";
        case ui::Activity::Write: return "WRITING";
        case ui::Activity::Seek: return "SEEKING";
        case ui::Activity::Idle: break;
    }
    return s.motor ? "SPINNING" : "IDLE";
}

static const char* formatName(unsigned kb) { return kb == 720 ? "720K" : "1.44M"; }

// ---- The screens --------------------------------------------------------------------------

void home(Canvas& c, const ui::Status& s) {
    title(c, s.image_name[0] ? s.image_name : "Generated disk");

    // The track, big, and the head.
    c.text(2, 13, "TRACK");
    char t[8];
    std::snprintf(t, sizeof t, "%02u", s.cylinder);
    bigText(c, 2, 22, t, 3);
    char h[8];
    std::snprintf(h, sizeof h, "H%u", s.head);
    bigText(c, 40, 22, h, 2);

    // The format, and write protection.
    c.text(40, 13, formatName(s.kb));
    if (s.write_protected) {
        c.fillRect(72, 12, 15, 9);
        c.text(74, 13, "WP", false);
    }

    // The disk: spinning with the motor, or the eject symbol with none in.
    const int cx = 113, cy = 24, r = 11;
    if (!s.disk_in) {
        ejectSymbol(c, cx, cy);
    } else {
        circle(c, cx, cy, r);
        circle(c, cx, cy, 3, true);
        float a = float(s.rotation) * 6.2831853f / 256.0f;
        int ex = cx + int(std::lround(std::sin(a) * (r - 2))), ey = cy - int(std::lround(std::cos(a) * (r - 2)));
        line(c, cx, cy, ex, ey);
        if (s.motor) {  // motion lines, behind the spoke
            float b = a - 0.6f;
            line(c, cx + int(std::lround(std::sin(b) * (r - 4))), cy - int(std::lround(std::cos(b) * (r - 4))),
                 cx + int(std::lround(std::sin(b) * (r - 2))), cy - int(std::lround(std::cos(b) * (r - 2))));
        }
    }

    // What it's doing, and which disk of how many.
    c.text(2, 46, activityName(s));
    if (s.images && s.images->count()) {
        char n[24];  // room for any two unsigneds, as GCC checks
        std::snprintf(n, sizeof n, "%u/%u", unsigned(s.image + 1), unsigned(s.images->count()));
        rightText(c, 126, 46, n);
    }

    // The head across the disk: cylinders 0-79, a tick every 10.
    const int x0 = 3, x1 = 124, base = 62;
    c.hLine(x0, base, x1 - x0 + 1);
    for (int cyl = 0; cyl <= 80; cyl += 10) c.vLine(x0 + cyl * (x1 - x0) / 80, base - 2, 2);
    int hx = x0 + (s.cylinder > 79 ? 79 : s.cylinder) * (x1 - x0) / 79;
    for (int i = 0; i < 4; ++i) c.hLine(hx - 3 + i, 55 + i, 7 - 2 * i);  // a marker pointing down
}

void disks(Canvas& c, const ui::Status& s, size_t selection) {
    size_t n = s.images ? s.images->count() : 0;
    char t[24];
    std::snprintf(t, sizeof t, "Disks %u/%u", unsigned(selection + 1), unsigned(n));
    title(c, t);
    const int rows = 6, row_h = 9, top = 11;
    // Scroll only when the list is longer than the screen, keeping the
    // selection third from the top where it can.
    size_t first = 0;
    if (n > size_t(rows)) {
        first = selection > 2 ? selection - 2 : 0;
        if (first > n - rows) first = n - rows;
    }
    for (int r = 0; r < rows && first + r < n; ++r) {
        size_t i = first + r;
        int y = top + r * row_h;
        bool sel = i == selection;
        if (sel) c.fillRect(0, y - 1, Canvas::WIDTH, row_h);
        if (i == s.image) c.text(1, y, ">", !sel);  // the disk that's in
        char name[16];
        std::strncpy(name, s.images->name(i), 15);
        name[15] = 0;
        if (char* dot = std::strrchr(name, '.')) *dot = 0;  // the extension says nothing here
        c.text(8, y, name, !sel);
        rightText(c, 126, y, s.images->size(i) == 737280 ? "720K" : "1.44M", !sel);
    }
}

void swap(Canvas& c, const ui::Status& s, size_t to) {
    // The 5x7 font has no descenders, so no g, j, p, q or y in the words here.
    title(c, "New disk");
    floppy(c, 6, 24);
    // An arrow beside the disk: up while the old one comes out, down as the
    // new one goes in.
    bool out = !s.disk_in;
    const int ax = 38, top = 22, bottom = 48;
    c.fillRect(ax - 1, top + 4, 3, bottom - top - 8);
    for (int i = 0; i < 5; ++i) {
        int y = out ? top + i : bottom - i;
        c.hLine(ax - i, y, 2 * i + 1);
    }
    c.text(48, 24, out ? "EJECTING" : "INSERTING");
    char name[14];
    std::strncpy(name, s.images && to < s.images->count() ? s.images->name(to) : "", 13);
    name[13] = 0;
    c.text(48, 36, name);
}

void volume(Canvas& c, int percent) {
    char t[16];
    std::snprintf(t, sizeof t, "Volume %d%%", percent);
    title(c, t);
    // 20 rising columns, 5% each: lit ones thick, the rest thin.
    for (int i = 0; i < 20; ++i) {
        int height = 3 + i * 48 / 19;
        int x = 3 + i * 6;
        bool lit = (i + 1) * 5 <= percent;
        c.fillRect(x, 63 - height, lit ? 4 : 1, height);
    }
}

void headTrace(Canvas& c, const ui::Status& s, const ui::HeadTrace& trace) {
    char t[24];
    std::snprintf(t, sizeof t, "Head  C%02u H%u", s.cylinder, s.head);
    title(c, t);
    // Cylinder 0 at the top (the outer edge), 79 at the bottom.
    const int top = 13, bottom = 63;
    for (int x = 0; x < Canvas::WIDTH; x += 4) c.pixel(x, (top + bottom) / 2);  // cylinder 40
    int n = trace.count();
    int px = -1, py = 0;
    for (int i = 0; i < n; ++i) {
        int x = Canvas::WIDTH - n + i;
        int cyl = trace.at(i) > 79 ? 79 : trace.at(i);
        int y = top + cyl * (bottom - top) / 79;
        if (px >= 0) line(c, px, py, x, y);
        px = x, py = y;
    }
}

void sounds(Canvas& c, const ui::Status& s) {
    title(c, s.pack[0] ? s.pack : "No sounds");
    for (int i = 0; i < 8; ++i) {
        int x = i < 4 ? 2 : 66, y = 13 + (i % 4) * 10;
        bool loaded = (s.sounds_loaded >> i) & 1;
        if (loaded)
            c.fillRect(x, y, 7, 7);
        else
            frame(c, x, y, 7, 7);
        c.text(x + 10, y, ui::SOUND_NAMES[i]);
    }
    if (s.pack[0]) {
        char line_[24];
        std::snprintf(line_, sizeof line_, "%u Hz  %u KB", s.rate, s.pack_kb);
        c.text((Canvas::WIDTH - Canvas::textWidth(line_)) / 2, 56, line_);
    }
}

void stats(Canvas& c, const ui::Status& s) {
    title(c, "Stats");
    struct { const char* name; uint32_t value; } rows[] = {
        {"Loads", s.loads}, {"Written", s.written}, {"Bad CRC", s.bad_crc},
        {"Overruns", s.overruns}, {"Underruns", s.underruns},
    };
    for (int i = 0; i < 5; ++i) {
        int y = 13 + i * 10;
        c.text(2, y, rows[i].name);
        char v[12];
        std::snprintf(v, sizeof v, "%lu", (unsigned long)rows[i].value);
        rightText(c, 126, y, v);
    }
}

void draw(Canvas& c, const ui::Model& m, const ui::Status& s, const ui::HeadTrace& trace) {
    c.clear();
    switch (m.screen()) {
        case ui::Screen::Home: home(c, s); break;
        case ui::Screen::Disks: disks(c, s, m.selection()); break;
        case ui::Screen::Swap: swap(c, s, m.selection()); break;
        case ui::Screen::Volume: volume(c, m.volume()); break;
        case ui::Screen::HeadTrace: headTrace(c, s, trace); break;
        case ui::Screen::Sounds: sounds(c, s); break;
        case ui::Screen::Stats: stats(c, s); break;
    }
}

}  // namespace screens
