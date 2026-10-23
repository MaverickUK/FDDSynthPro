// Tests for the front panel: the UI model's controls, and the screens. Set
// FDDSYNTH_PREVIEW_DIR to write each screen as a PBM image, then
// test/pbm_to_png.py turns them into the PNGs in docs/oled. Also set
// FDDSYNTH_PREVIEW_ALL for the extra frames the interactive panel page uses
// (scripts/build_panel_page.py): every volume step, list selection and disk.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <string>

#include "canvas.h"
#include "check.h"
#include "image_list.h"
#include "screens.h"
#include "ui.h"

namespace {  // this file's own helpers and tests

void preview(const Canvas& c, const char* name) {
    const char* dir = std::getenv("FDDSYNTH_PREVIEW_DIR");
    if (!dir) return;
    std::ofstream f(std::string(dir) + "/" + name + ".pbm");
    f << "P1\n" << Canvas::WIDTH << " " << Canvas::HEIGHT << "\n";
    for (int y = 0; y < Canvas::HEIGHT; ++y) {
        for (int x = 0; x < Canvas::WIDTH; ++x) f << (c.get(x, y) ? "1 " : "0 ");
        f << "\n";
    }
}

// The extra frames: only with FDDSYNTH_PREVIEW_ALL set too.
void previewExtra(const Canvas& c, const char* name) {
    if (std::getenv("FDDSYNTH_PREVIEW_ALL")) preview(c, name);
}

int lit(const Canvas& c, int x0, int y0, int x1, int y1) {
    int n = 0;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) n += c.get(x, y);
    return n;
}

// A card with five disks, the second one in.
struct Card {
    ImageList list;
    ui::Status s;
    Card() {
        list.add("DOS622.IMG", 1474560);
        list.add("GAMES.IMG", 1474560);
        list.add("MONKEY1.IMG", 1474560);
        list.add("UTILS720.IMA", 737280);
        list.add("WORKBENCH.IMG", 1474560);
        s.images = &list;
        s.image = 1;
        s.image_name = list.name(1);
        s.disk_in = true;
        s.motor = true;
        s.cylinder = 40;
        s.head = 1;
        s.activity = ui::Activity::Read;
        s.rotation = 40;
        s.pack = "Placeholder";
        s.sounds_loaded = 0xFF;
        s.rate = 22050;
        s.pack_kb = 152;
        s.loads = 412;
        s.written = 37;
    }
};

// ---- The screens, rendered ------------------------------------------------------------------

TEST(test_screen_home) {
    Card k;
    Canvas c;
    screens::home(c, k.s);
    preview(c, "home_reading");
    CHECK(lit(c, 0, 0, 127, 9) > 600);   // the lit title bar
    CHECK(lit(c, 2, 22, 34, 42) > 100);  // the big track number

    k.s.activity = ui::Activity::Seek;
    k.s.cylinder = 3;
    k.s.head = 0;
    k.s.rotation = 170;
    c.clear();
    screens::home(c, k.s);
    preview(c, "home_seeking");

    k.s.activity = ui::Activity::Write;
    k.s.cylinder = 79;
    c.clear();
    screens::home(c, k.s);
    preview(c, "home_writing");

    k.s.activity = ui::Activity::Idle;
    k.s.motor = false;
    k.s.write_protected = true;
    k.s.cylinder = 0;
    c.clear();
    screens::home(c, k.s);
    preview(c, "home_idle_write_protected");

    k.s.disk_in = false;
    c.clear();
    screens::home(c, k.s);
    preview(c, "home_no_disk");
}

TEST(test_screen_home_animation) {
    // Frames for an animated preview: the disk turning while the head steps
    // in from 38 to 45, then reads.
    Card k;
    Canvas c;
    for (int f = 0; f < 16; ++f) {
        k.s.rotation = uint8_t(f * 21);
        k.s.cylinder = uint8_t(f < 8 ? 38 + f : 45);
        k.s.activity = f < 8 ? ui::Activity::Seek : ui::Activity::Read;
        c.clear();
        screens::home(c, k.s);
        char name[24];
        std::snprintf(name, sizeof name, "anim_home_%02d", f);
        previewExtra(c, name);
    }
}

TEST(test_screen_per_disk_frames) {
    // For the interactive preview: with each disk in, the home screen
    // turning (8 frames), with it ejected, and the swap to it.
    Card k;
    Canvas c;
    char name[32];
    for (size_t d = 0; d < k.list.count(); ++d) {
        k.s.image = d;
        k.s.image_name = k.list.name(d);
        k.s.kb = k.list.size(d) == 737280 ? 720 : 1440;
        k.s.disk_in = true;
        k.s.activity = ui::Activity::Read;
        k.s.cylinder = uint8_t(12 + d * 14);
        k.s.head = uint8_t(d & 1);
        for (int f = 0; f < 8; ++f) {
            k.s.rotation = uint8_t(f * 32);
            c.clear();
            screens::home(c, k.s);
            std::snprintf(name, sizeof name, "p_home_%u_%u", unsigned(d), unsigned(f));
            previewExtra(c, name);
        }
        k.s.disk_in = false;
        k.s.motor = true;
        c.clear();
        screens::home(c, k.s);
        std::snprintf(name, sizeof name, "p_nodisk_%u", unsigned(d));
        previewExtra(c, name);
        c.clear();
        screens::swap(c, k.s, d);
        std::snprintf(name, sizeof name, "p_eject_%u", unsigned(d));
        previewExtra(c, name);
        k.s.disk_in = true;
        c.clear();
        screens::swap(c, k.s, d);
        std::snprintf(name, sizeof name, "p_insert_%u", unsigned(d));
        previewExtra(c, name);
    }
}

TEST(test_screen_disks) {
    Card k;
    Canvas c;
    for (size_t i = 0; i < k.list.count(); ++i) {  // each selection, for the interactive preview
        c.clear();
        screens::disks(c, k.s, i);
        char name[16];
        std::snprintf(name, sizeof name, "disks_%u", unsigned(i));
        previewExtra(c, name);
    }
    c.clear();
    screens::disks(c, k.s, 3);
    preview(c, "disks");
    CHECK(lit(c, 0, 37, 127, 45) > 300);  // the selected row (the fourth) is lit
}

TEST(test_screen_swap) {
    Card k;
    Canvas c;
    k.s.disk_in = false;
    screens::swap(c, k.s, 2);
    preview(c, "swap_ejecting");
    k.s.disk_in = true;
    c.clear();
    screens::swap(c, k.s, 2);
    preview(c, "swap_inserting");
}

TEST(test_screen_volume) {
    Canvas c;
    for (int v = 0; v <= 100; v += 5) {  // each step, for the interactive preview
        c.clear();
        screens::volume(c, v);
        char name[16];
        std::snprintf(name, sizeof name, "volume_%d", v);
        previewExtra(c, name);
    }
    c.clear();
    screens::volume(c, 75);
    preview(c, "volume_75");
    CHECK(lit(c, 0, 12, 127, 63) > 200);
}

TEST(test_screen_head_trace) {
    // A DOS-like few seconds: idle at 0, a seek out to 40, single steps, a
    // seek back for the FAT, then a full sweep.
    Card k;
    ui::HeadTrace trace;
    int cyl = 0;
    auto hold = [&](int n) { for (int i = 0; i < n; ++i) trace.add(uint8_t(cyl)); };
    auto seek = [&](int to, int per) { while (cyl != to) { cyl += cyl < to ? 1 : -1; if (cyl % per == 0) trace.add(uint8_t(cyl)); } trace.add(uint8_t(cyl)); };
    hold(10); seek(40, 8); hold(14); cyl = 41; hold(10); cyl = 42; hold(8); seek(0, 8); hold(10);
    seek(79, 6); hold(12); seek(20, 6); hold(20);
    Canvas c;
    k.s.cylinder = uint8_t(cyl);
    screens::headTrace(c, k.s, trace);
    preview(c, "head_trace");
    CHECK(trace.count() > 100 && trace.count() <= ui::HeadTrace::SAMPLES);
}

TEST(test_screen_sounds) {
    Card k;
    Canvas c;
    k.s.sounds_loaded = 0x3F;  // no insert or eject
    screens::sounds(c, k.s);
    preview(c, "sounds");
}

TEST(test_screen_stats) {
    Card k;
    Canvas c;
    screens::stats(c, k.s);
    preview(c, "stats");
}

// ---- The controls ---------------------------------------------------------------------------

TEST(test_ui_volume_overlay) {
    Card k;
    ui::Model m;
    m.input(ui::Input::TurnUp, k.s, 1000);
    CHECK(m.screen() == ui::Screen::Volume);
    CHECK_EQ(m.volume(), 80);
    for (int i = 0; i < 10; ++i) m.input(ui::Input::TurnUp, k.s, 1100);
    CHECK_EQ(m.volume(), 100);
    m.tick(k.s, 1100 + ui::Model::OVERLAY_MS - 1);
    CHECK(m.screen() == ui::Screen::Volume);
    m.tick(k.s, 1100 + ui::Model::OVERLAY_MS);
    CHECK(m.screen() == ui::Screen::Home);
}

TEST(test_ui_choose_and_insert_a_disk) {
    Card k;
    ui::Model m;
    m.input(ui::Input::Press, k.s, 0);
    CHECK(m.screen() == ui::Screen::Disks);
    CHECK_EQ(m.selection(), 1u);  // starts on the disk that's in
    m.input(ui::Input::TurnDown, k.s, 10);
    m.input(ui::Input::TurnDown, k.s, 20);
    CHECK_EQ(m.selection(), 3u);
    m.input(ui::Input::Press, k.s, 30);
    CHECK(m.screen() == ui::Screen::Swap);
    size_t to = 0;
    CHECK(m.takeSwap(to));
    CHECK_EQ(to, 3u);
    CHECK(!m.takeSwap(to));  // once
    // The swap runs, then finishes: back home.
    k.s.swapping = true;
    m.tick(k.s, 100);
    CHECK(m.screen() == ui::Screen::Swap);
    k.s.swapping = false;
    m.tick(k.s, 700);
    CHECK(m.screen() == ui::Screen::Home);
}

TEST(test_ui_disk_list_wraps_and_cancels) {
    Card k;
    ui::Model m;
    m.input(ui::Input::Press, k.s, 0);
    m.input(ui::Input::TurnUp, k.s, 0);
    m.input(ui::Input::TurnUp, k.s, 0);
    CHECK_EQ(m.selection(), 4u);  // round from the first to the last
    m.input(ui::Input::Action, k.s, 0);
    CHECK(m.screen() == ui::Screen::Home);
    size_t to;
    CHECK(!m.takeSwap(to));
    // Pressing on the disk that's already in does nothing.
    m.input(ui::Input::Press, k.s, 0);
    m.input(ui::Input::Press, k.s, 0);
    CHECK(m.screen() == ui::Screen::Home);
    CHECK(!m.takeSwap(to));
    // Left alone, the list closes.
    m.input(ui::Input::Press, k.s, 0);
    m.tick(k.s, ui::Model::LIST_MS);
    CHECK(m.screen() == ui::Screen::Home);
}

TEST(test_ui_action_cycles_screens) {
    Card k;
    ui::Model m;
    ui::Screen order[] = {ui::Screen::HeadTrace, ui::Screen::Sounds, ui::Screen::Stats, ui::Screen::Home};
    for (ui::Screen want : order) {
        m.input(ui::Input::Action, k.s, 0);
        CHECK(m.screen() == want);
    }
    // The volume overlay returns to the screen it covered.
    m.input(ui::Input::Action, k.s, 0);
    m.input(ui::Input::TurnDown, k.s, 0);
    m.tick(k.s, ui::Model::OVERLAY_MS);
    CHECK(m.screen() == ui::Screen::HeadTrace);
}

TEST(test_ui_long_action_ejects) {
    Card k;
    ui::Model m;
    CHECK(!m.takeEjectToggle());
    m.input(ui::Input::LongAction, k.s, 0);
    CHECK(m.takeEjectToggle());
    CHECK(!m.takeEjectToggle());
    CHECK(m.screen() == ui::Screen::Home);
}

TEST(test_ui_generated_disk_has_no_list) {
    ui::Status s;  // no images: the generated disk
    ui::Model m;
    m.input(ui::Input::Press, s, 0);
    CHECK(m.screen() == ui::Screen::Home);
}

TEST(test_draw_picks_the_screen) {
    Card k;
    ui::Model m;
    ui::HeadTrace trace;
    Canvas a, b;
    screens::draw(a, m, k.s, trace);
    screens::home(b, k.s);
    CHECK(std::memcmp(a.data(), b.data(), Canvas::BYTES) == 0);
}

}  // namespace

void runUiTests() {
    RUN(test_screen_home);
    RUN(test_screen_home_animation);
    RUN(test_screen_per_disk_frames);
    RUN(test_screen_disks);
    RUN(test_screen_swap);
    RUN(test_screen_volume);
    RUN(test_screen_head_trace);
    RUN(test_screen_sounds);
    RUN(test_screen_stats);
    RUN(test_ui_volume_overlay);
    RUN(test_ui_choose_and_insert_a_disk);
    RUN(test_ui_disk_list_wraps_and_cancels);
    RUN(test_ui_action_cycles_screens);
    RUN(test_ui_long_action_ejects);
    RUN(test_ui_generated_disk_has_no_list);
    RUN(test_draw_picks_the_screen);
}
