// Tests for the front panel: the UI model's controls, the screens, the
// controls' pins as inputs (controls.h) and the OLED's bus bytes (ssd1306.h). Set
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

#include <vector>

#include "canvas.h"
#include "check.h"
#include "controls.h"
#include "image_list.h"
#include "screens.h"
#include "ssd1306.h"
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
    // The disk coming out is named while it does: GAMES, not MONKEY1.
    Canvas want;
    want.text(48, 36, "GAMES.IMG");
    bool same = true;
    for (int y = 36; y < 43; ++y)
        for (int x = 48; x < 128; ++x) same = same && c.get(x, y) == want.get(x, y);
    CHECK(same);
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

TEST(test_ui_two_drives) {
    // A home screen for each drive: the action goes A, B, head trace...
    // and what's chosen on B's goes to B.
    Card k;
    k.s.drives = 2;
    ui::Model m;
    CHECK_EQ(m.drive(), 0);
    m.input(ui::Input::Action, k.s, 0);
    CHECK(m.screen() == ui::Screen::Home);
    CHECK_EQ(m.drive(), 1);
    m.input(ui::Input::Press, k.s, 10);
    m.input(ui::Input::TurnDown, k.s, 20);
    m.input(ui::Input::Press, k.s, 30);
    size_t to = 0;
    uint8_t drive = 0;
    CHECK(m.takeSwap(to, drive));
    CHECK_EQ(to, 2u);
    CHECK_EQ(drive, 1);
    k.s.swapping = true;
    m.tick(k.s, 100);
    k.s.swapping = false;
    m.tick(k.s, 700);
    CHECK(m.screen() == ui::Screen::Home);
    CHECK_EQ(m.drive(), 1);  // still B's
    m.input(ui::Input::LongAction, k.s, 800);
    CHECK(m.takeEjectToggle(drive));
    CHECK_EQ(drive, 1);
    ui::Screen order[] = {ui::Screen::HeadTrace, ui::Screen::Sounds, ui::Screen::Stats, ui::Screen::Home};
    for (ui::Screen want : order) m.input(ui::Input::Action, k.s, 900);
    CHECK(m.screen() == order[3]);
    CHECK_EQ(m.drive(), 0);  // round to A's
    m.input(ui::Input::LongAction, k.s, 1000);
    CHECK(m.takeEjectToggle(drive));
    CHECK_EQ(drive, 0);
}

TEST(test_screen_home_disk_sizes) {
    // The size, as disks are named: a 1.2 MB 5.25-inch disk isn't "1.44M".
    Card k;
    Canvas hd, five, small;
    screens::home(hd, k.s);
    k.s.kb = 1200;
    k.s.image_name = "WP51.IMG";
    screens::home(five, k.s);
    preview(five, "home_1200");
    k.s.kb = 360;
    screens::home(small, k.s);
    CHECK(lit(hd, 40, 13, 70, 20) != lit(five, 40, 13, 70, 20));
    CHECK(lit(five, 40, 13, 70, 20) != lit(small, 40, 13, 70, 20));
}

TEST(test_screen_home_two_drives) {
    Card k;
    k.s.drives = 2;
    k.s.drive_name = "B";
    Canvas c;
    screens::home(c, k.s);
    preview(c, "home_drive_b");
    Canvas one;
    k.s.drives = 1;
    screens::home(one, k.s);
    CHECK(lit(c, 0, 0, 127, 9) != lit(one, 0, 0, 127, 9));  // "B: GAMES.IMG" in the title
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

// The card's packs, the second loaded.
struct Packs {
    PackList list;
    Packs(ui::Status& s) {
        list.add("Placeholder");
        list.add("Recorded");
        list.add("Zx81");
        s.packs = &list;
        s.pack_index = 1;
        s.pack = list.name(1);
    }
};

TEST(test_ui_choose_a_pack) {
    Card k;
    Packs p(k.s);
    ui::Model m;
    for (int i = 0; i < 2; ++i) m.input(ui::Input::Action, k.s, 0);  // home -> head trace -> sounds
    CHECK(m.screen() == ui::Screen::Sounds);
    m.input(ui::Input::Press, k.s, 10);
    CHECK(m.screen() == ui::Screen::Packs);
    CHECK_EQ(m.selection(), 1u);  // starts on the one loaded
    m.input(ui::Input::TurnUp, k.s, 20);
    m.input(ui::Input::Press, k.s, 30);
    CHECK(m.screen() == ui::Screen::Sounds);
    size_t to = 9;
    CHECK(m.takePackChange(to));
    CHECK_EQ(to, 0u);
    CHECK(!m.takePackChange(to));  // once
    // The one already loaded: nothing to do.
    m.input(ui::Input::Press, k.s, 40);
    m.input(ui::Input::Press, k.s, 50);
    CHECK(!m.takePackChange(to));
    // Action backs out; left alone, the list closes.
    m.input(ui::Input::Press, k.s, 60);
    m.input(ui::Input::Action, k.s, 70);
    CHECK(m.screen() == ui::Screen::Sounds);
    m.input(ui::Input::Press, k.s, 80);
    m.tick(k.s, 80 + ui::Model::LIST_MS);
    CHECK(m.screen() == ui::Screen::Sounds);
    // Elsewhere, press still opens the disk list.
    m.input(ui::Input::Action, k.s, 90000);  // stats
    m.input(ui::Input::Press, k.s, 90010);
    CHECK(m.screen() == ui::Screen::Disks);
}

TEST(test_ui_no_packs_no_list) {
    Card k;  // no pack list
    ui::Model m;
    for (int i = 0; i < 2; ++i) m.input(ui::Input::Action, k.s, 0);
    m.input(ui::Input::Press, k.s, 10);
    CHECK(m.screen() == ui::Screen::Sounds);
}

TEST(test_ui_restored_volume) {
    ui::Model m;
    m.setVolume(40);
    CHECK_EQ(m.volume(), 40);
    m.setVolume(140);
    CHECK_EQ(m.volume(), 100);
}

TEST(test_screen_packs) {
    Card k;
    Packs p(k.s);
    Canvas c;
    screens::packs(c, k.s, 2);
    preview(c, "packs");
    CHECK(lit(c, 0, 0, 127, 9) > 300);  // the title bar
    CHECK(lit(c, 0, 28, 127, 36) > 300);  // the third row, lit (selected)
    c.clear();
    k.s.pack_loading = true;
    screens::sounds(c, k.s);
    preview(c, "sounds_loading");
    Canvas want;
    want.text((Canvas::WIDTH - Canvas::textWidth("LOADING WHEN IDLE")) / 2, 56, "LOADING WHEN IDLE");
    bool same = true;
    for (int y = 56; y < 63; ++y)
        for (int x = 0; x < 128; ++x) same = same && c.get(x, y) == want.get(x, y);
    CHECK(same);
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

// ---- The controls and the bus --------------------------------------------------

// Holds the controls in one state from `from` to `to` ms, a call per ms as
// core 0's loop would (more often, really), and collects the inputs. The
// detents arrive on the first call.
std::vector<ui::Input> hold(controls::Controls& c, uint32_t from, uint32_t to, bool encoder, bool action,
                            int detents = 0) {
    std::vector<ui::Input> got;
    for (uint32_t t = from; t < to; ++t) {
        ui::Input out[controls::Controls::MAX_INPUTS];
        size_t n = c.update(t == from ? detents : 0, encoder, action, t, out);
        got.insert(got.end(), out, out + n);
    }
    return got;
}

TEST(test_controls_turns) {
    controls::Controls c;
    std::vector<ui::Input> got = hold(c, 0, 1, false, false, 3);
    CHECK_EQ(got.size(), 3u);
    for (ui::Input in : got) CHECK(in == ui::Input::TurnUp);
    got = hold(c, 1, 2, false, false, -2);
    CHECK_EQ(got.size(), 2u);
    for (ui::Input in : got) CHECK(in == ui::Input::TurnDown);
    // A very fast turn is cut short rather than overflowing.
    CHECK_EQ(hold(c, 2, 3, false, false, 50).size(), controls::Controls::MAX_INPUTS);
}

TEST(test_controls_encoder_press_is_debounced) {
    controls::Controls c;
    // Contact bounce for a few ms: no press yet.
    std::vector<ui::Input> got = hold(c, 0, 3, true, false);
    auto more = hold(c, 3, 5, false, false);
    got.insert(got.end(), more.begin(), more.end());
    CHECK(got.empty());
    // Then held: one Press, once it has been steady for the debounce time.
    got = hold(c, 5, 5 + cfg::BUTTON_DEBOUNCE_MS - 1, true, false);
    CHECK(got.empty());
    got = hold(c, 5 + cfg::BUTTON_DEBOUNCE_MS - 1, 500, true, false);
    CHECK_EQ(got.size(), 1u);
    CHECK(got[0] == ui::Input::Press);
    // Letting go does nothing.
    CHECK(hold(c, 500, 600, false, false).empty());
}

TEST(test_controls_action_short_and_long) {
    controls::Controls c;
    // A short press: Action when it's let go.
    CHECK(hold(c, 0, 300, false, true).empty());
    std::vector<ui::Input> got = hold(c, 300, 400, false, false);
    CHECK_EQ(got.size(), 1u);
    CHECK(got[0] == ui::Input::Action);
    // A long one: LongAction while still held, and nothing on release.
    got = hold(c, 1000, 1000 + cfg::BUTTON_DEBOUNCE_MS + cfg::BUTTON_LONG_MS + 10, false, true);
    CHECK_EQ(got.size(), 1u);
    CHECK(got[0] == ui::Input::LongAction);
    CHECK(hold(c, 3000, 3100, false, false).empty());
}

TEST(test_controls_drive_the_model) {
    // The firmware's path: pins to inputs to the model to its requests.
    Card k;
    controls::Controls c;
    ui::Model m;
    auto apply = [&](const std::vector<ui::Input>& ins, uint32_t t) {
        for (ui::Input in : ins) m.input(in, k.s, t);
        m.tick(k.s, t);  // as the firmware does on every pass
    };
    apply(hold(c, 0, 50, true, false), 50);  // press: the disk list
    apply(hold(c, 50, 100, false, false), 100);
    CHECK(m.screen() == ui::Screen::Disks);
    apply(hold(c, 100, 101, false, false, -2), 101);  // two clicks anticlockwise: down the list
    apply(hold(c, 101, 150, true, false), 150);       // press: insert it
    size_t to = 0;
    CHECK(m.takeSwap(to));
    CHECK_EQ(to, 3u);
    apply(hold(c, 150, 2500, false, false), 2500);  // (no swap ran: the Swap screen gives up)
    CHECK(m.screen() == ui::Screen::Home);
    apply(hold(c, 2500, 4000, false, true), 4000);  // a long action: eject
    CHECK(m.takeEjectToggle());
}

TEST(test_ssd1306_frame_stream) {
    uint8_t frame[ssd1306::FRAME_BYTES];
    for (size_t i = 0; i < sizeof frame; ++i) frame[i] = uint8_t(i * 7);
    ssd1306::FrameStream st;
    CHECK(st.done());  // nothing to send yet
    st.start(frame);
    std::vector<ssd1306::FrameStream::Byte> bytes;
    while (!st.done()) bytes.push_back(st.next());
    CHECK_EQ(bytes.size(), size_t(7 + 1 + 1024));
    // The window: commands, the whole screen (columns 0-127, pages 0-7).
    const uint8_t window[] = {0x00, 0x21, 0, 127, 0x22, 0, 7};
    for (size_t i = 0; i < 7; ++i) CHECK_EQ(bytes[i].value, window[i]);
    // Then the data, in canvas order.
    CHECK_EQ(bytes[7].value, 0x40);
    bool same = true;
    for (size_t i = 0; i < sizeof frame; ++i) same = same && bytes[8 + i].value == frame[i];
    CHECK(same);
    // Two transfers: a STOP after the window and after the last byte only.
    std::vector<size_t> stops;
    for (size_t i = 0; i < bytes.size(); ++i)
        if (bytes[i].stop) stops.push_back(i);
    CHECK_EQ(stops.size(), 2u);
    CHECK_EQ(stops[0], 6u);
    CHECK_EQ(stops[1], bytes.size() - 1);
    // A cancelled frame is done at once, and the next starts from the top.
    st.start(frame);
    st.next();
    st.cancel();
    CHECK(st.done());
    st.start(frame);
    CHECK_EQ(st.next().value, 0x00);
}

TEST(test_ssd1306_init) {
    CHECK_EQ(ssd1306::INIT[0], 0x00);  // commands
    CHECK_EQ(ssd1306::INIT[1], 0xAE);  // off while setting up
    CHECK_EQ(ssd1306::INIT[sizeof ssd1306::INIT - 1], 0xAF);  // on at the end
    CHECK_EQ(ssd1306::FRAME_BYTES, Canvas::BYTES);
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
    RUN(test_ui_two_drives);
    RUN(test_screen_home_disk_sizes);
    RUN(test_screen_home_two_drives);
    RUN(test_ui_long_action_ejects);
    RUN(test_ui_generated_disk_has_no_list);
    RUN(test_ui_choose_a_pack);
    RUN(test_ui_no_packs_no_list);
    RUN(test_ui_restored_volume);
    RUN(test_screen_packs);
    RUN(test_draw_picks_the_screen);
    RUN(test_controls_turns);
    RUN(test_controls_encoder_press_is_debounced);
    RUN(test_controls_action_short_and_long);
    RUN(test_controls_drive_the_model);
    RUN(test_ssd1306_frame_stream);
    RUN(test_ssd1306_init);
}
