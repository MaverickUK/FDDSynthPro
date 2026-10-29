// Tests for what the drive remembers across power-off (src/settings.*): the
// file's format, what a damaged or hand-edited file does, and when a change
// is written to the card.
#include <cstring>
#include <string>

#include "check.h"
#include "image_list.h"
#include "settings.h"

namespace {  // this file's own helpers and tests

settings::Settings parse(const std::string& text) { return settings::parse(text.data(), text.size()); }

TEST(test_settings_round_trip) {
    // As the firmware writes it: the disk that was in, under `last`.
    settings::Settings s;
    std::strcpy(s.last[0], "MONKEY1.IMG");
    std::strcpy(s.pack, "Recorded");
    s.volume = 40;
    char text[settings::FILE_MAX];
    size_t n = settings::format(s, text, sizeof text);
    CHECK_STR(std::string(text, n),
              "last=MONKEY1.IMG\npack=Recorded\nvolume=40\ninterface=ibmpc\ncable=twisted\ndrive=a\n");
    CHECK(settings::parse(text, n) == s);
    s.bus = {bus::Interface::Shugart, bus::Cable::Straight, 0b100};
    n = settings::format(s, text, sizeof text);
    CHECK(settings::parse(text, n) == s);
    CHECK(std::string(text, n).find("interface=shugart\ncable=straight\ndrive=ds2\n") != std::string::npos);
    // Two drives: the second's disk too.
    s.bus = {bus::Interface::IbmPc, bus::Cable::Twisted, 0b11};
    std::strcpy(s.last[1], "WORK.IMG");
    n = settings::format(s, text, sizeof text);
    CHECK_STR(std::string(text, n), "last=MONKEY1.IMG\nlast2=WORK.IMG\npack=Recorded\nvolume=40\n"
                                    "interface=ibmpc\ncable=twisted\ndrive=a,b\n");
    CHECK(settings::parse(text, n) == s);
}

TEST(test_settings_fixed_image) {
    // `image`, set by hand, is the disk at every power-on; the firmware
    // writes it back as it found it, beside the remembered `last`.
    settings::Settings s = parse("image=DOS622.IMG\nlast=GAMES.IMG\nimage2=WORK.IMG\ndrive=a,b\n");
    CHECK_STR(s.image[0], "DOS622.IMG");
    CHECK_STR(s.image[1], "WORK.IMG");
    CHECK_STR(s.last[0], "GAMES.IMG");
    std::strcpy(s.last[0], "MONKEY1.IMG");  // another disk chosen on the panel
    char text[settings::FILE_MAX];
    size_t n = settings::format(s, text, sizeof text);
    std::string written(text, n);
    CHECK(written.find("image=DOS622.IMG\nimage2=WORK.IMG\nlast=MONKEY1.IMG\nlast2=\n") == 0);
    CHECK(settings::parse(text, n) == s);
    // Unset, it isn't written at all.
    CHECK(std::string(text, settings::format(settings::Settings{}, text, sizeof text)).find("image") ==
          std::string::npos);
}

TEST(test_settings_bus) {
    // An old file, from before the bus settings: a PC drive past the twist.
    settings::Settings s = parse("image=GAMES.IMG\npack=Recorded\nvolume=75\n");
    CHECK(s.bus == bus::Setup{});
    s = parse("interface=ibmpc\r\ncable=straight\r\ndrive=b\r\n");
    CHECK(s.bus.interface == bus::Interface::IbmPc);
    CHECK(s.bus.cable == bus::Cable::Straight);
    CHECK_EQ(s.bus.units, 0b10);
    // The unit may come before the interface.
    s = parse("drive=ds1\ninterface=shugart\n");
    CHECK(s.bus.interface == bus::Interface::Shugart);
    CHECK_EQ(s.bus.units, 0b10);
    // Two drives, either order; the first is the lower unit.
    s = parse("drive=b,a\nimage2=WORK.IMG\n");
    CHECK_EQ(s.bus.units, 0b11);
    CHECK_EQ(s.bus.drives(), 2u);
    CHECK_STR(s.image[1], "WORK.IMG");
    s = parse("interface=shugart\ndrive=ds2,ds0\n");
    CHECK_EQ(s.bus.unit(0), 0);
    CHECK_EQ(s.bus.unit(1), 2);
    // A Shugart unit on a PC: only the units a PC has.
    CHECK_EQ(parse("drive=ds0,ds2\n").bus.units, 0b01);
    CHECK_EQ(parse("drive=ds2\n").bus.units, 0b01);
    // Unknown values keep the defaults; DS3 (pin 6) isn't wired; three
    // drives, or one twice, aren't taken.
    s = parse("interface=amstrad\ncable=crossed\ndrive=ds3\ndrive=c\ndrive=ds12\ndrive=\ndrive=a,a\n"
              "drive=a,b,\ndrive=,a\n");
    CHECK(s.bus == bus::Setup{});
    CHECK_EQ(parse("interface=shugart\ndrive=ds0,ds1,ds2\n").bus.units, 0b001);
}

TEST(test_settings_defaults) {
    settings::Settings s = parse("");  // no file, or an empty one
    CHECK_STR(s.image[0], "");
    CHECK_STR(s.pack, "");
    CHECK_EQ(s.volume, 75);
}

TEST(test_settings_hand_edited) {
    // Windows line ends, trailing spaces, a comment-ish line, keys in any
    // order, a missing last newline, names with spaces.
    settings::Settings s = parse("# my drive\r\nvolume=90 \r\npack=My Drive\r\nimage=DOS 6.22.IMG");
    CHECK_STR(s.image[0], "DOS 6.22.IMG");
    CHECK_STR(s.pack, "My Drive");
    CHECK_EQ(s.volume, 90);
}

TEST(test_settings_bad_values_keep_the_defaults) {
    CHECK_EQ(parse("volume=101\n").volume, 75);
    CHECK_EQ(parse("volume=-5\n").volume, 75);
    CHECK_EQ(parse("volume=loud\n").volume, 75);
    CHECK_EQ(parse("volume=\n").volume, 75);
    CHECK_EQ(parse("volume=0\n").volume, 0);
    // A name too long for the drive's lists is refused, never cut short
    // (which would name another file).
    std::string longName(settings::NAME, 'A');
    CHECK_STR(parse("image=" + longName + "\n").image[0], "");
    CHECK_STR(parse("imagery=GAMES.IMG\nimage\n").image[0], "");  // not the key, or no value
}

TEST(test_settings_garbage_never_crashes) {
    // Random bytes, as a corrupted file might hold: always parses, values sane.
    uint32_t seed = 7;
    for (int round = 0; round < 200; ++round) {
        std::string text;
        for (int i = 0; i < 120; ++i) {
            seed = seed * 1103515245 + 12345;
            const char pick[] = "image=pack=volume=\n\r 0123456789AZaz.";
            text += (seed >> 16) % 3 ? pick[(seed >> 8) % (sizeof pick - 1)] : char(seed >> 24);
        }
        settings::Settings s = parse(text);
        CHECK(s.volume >= 0 && s.volume <= 100);
        CHECK(std::strlen(s.image[0]) < settings::NAME && std::strlen(s.pack) < settings::NAME);
    }
}

TEST(test_settings_save_timer) {
    settings::SaveTimer t;
    CHECK(!t.due(100000, true));  // nothing changed
    t.changed(1000);
    t.changed(1500);  // a turn of the volume: many changes
    CHECK(!t.due(1500 + settings::SaveTimer::SETTLE_MS - 1, true));
    CHECK(t.due(1500 + settings::SaveTimer::SETTLE_MS, true));
    // The host is using the drive: wait, however long.
    CHECK(!t.due(60000, false));
    CHECK(t.due(60001, true));
    t.saved();
    CHECK(!t.due(70000, true));
    CHECK(!t.dirty());
}

TEST(test_pack_list) {
    PackList p;
    CHECK(p.add("Recorded"));
    CHECK(p.add("placeholder"));
    CHECK(p.add("Amiga"));
    CHECK(!p.add(".Spotlight-V100"));  // hidden folders aren't packs
    CHECK(!p.add(""));
    CHECK_EQ(p.count(), 3u);
    CHECK_STR(p.name(0), "Amiga");  // name order, ignoring case
    CHECK_STR(p.name(1), "placeholder");
    CHECK_STR(p.name(2), "Recorded");
    CHECK_EQ(p.find("RECORDED"), 2u);
    CHECK_EQ(p.find("Missing"), 3u);
    CHECK_EQ(p.next(2), 0u);
    CHECK_EQ(p.prev(0), 2u);
    ImageList images;
    images.add("GAMES.IMG", 1474560);
    CHECK_EQ(images.find("games.img"), 0u);
    CHECK_EQ(images.find("OTHER.IMG"), 1u);
}

}  // namespace

void runSettingsTests() {
    RUN(test_settings_round_trip);
    RUN(test_settings_fixed_image);
    RUN(test_settings_bus);
    RUN(test_settings_defaults);
    RUN(test_settings_hand_edited);
    RUN(test_settings_bad_values_keep_the_defaults);
    RUN(test_settings_garbage_never_crashes);
    RUN(test_settings_save_timer);
    RUN(test_pack_list);
}
