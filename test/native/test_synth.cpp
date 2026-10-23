// Tests for the FDD Synth mixer (src/synth.*) beyond what the comparison
// with the Python player covers (host/tests/test_synth.py): the WAV parser
// on the stand-in pack, voice stealing, incomplete packs, and the volume.
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <vector>

#include "check.h"
#include "synth.h"
#include "wav.h"

namespace {  // this file's own helpers and tests

static std::vector<uint8_t> readFile(const char* path) {
    std::vector<uint8_t> out;
    FILE* f = std::fopen(path, "rb");
    if (!f) return out;
    uint8_t buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return out;
}

TEST(test_wav_stand_in_pack) {
    // Run from the repo root (run_native_tests.sh does).
    const char* names[] = {"spinup", "spin", "spindown", "step", "seek", "bump", "insert", "eject"};
    for (const char* name : names) {
        char path[128];
        std::snprintf(path, sizeof path, "samples/Placeholder/%s.wav", name);
        std::vector<uint8_t> f = readFile(path);
        WavInfo info;
        CHECK(!f.empty());
        CHECK(parseWavHeader(f.data(), f.size(), uint32_t(f.size()), info) == WavError::Ok);
        CHECK(wavIsPlayable(info));
        CHECK_EQ(info.sampleRate, 22050u);
    }
    // Not a WAV at all.
    const uint8_t junk[64] = {'R', 'I', 'F', 'X'};
    WavInfo info;
    CHECK(parseWavHeader(junk, sizeof junk, sizeof junk, info) == WavError::NotRiff);
}

// A pack of constant tones, easy to reason about.
struct ToyPack {
    std::vector<int16_t> one = std::vector<int16_t>(100, 100);
    std::vector<int16_t> loop = std::vector<int16_t>(10, 10);
    synth::Pack pack;
    ToyPack() {
        synth::Sample s{one.data(), uint32_t(one.size())}, l{loop.data(), uint32_t(loop.size())};
        pack.rate = 22050;
        pack.spinup = pack.spindown = pack.step = pack.bump = pack.insert = pack.eject = s;
        pack.spin = pack.seek = l;
    }
};

static activity::Event ev(activity::Sound s) { return activity::Event{0, s, false, 0, 0}; }

TEST(test_synth_silent_without_events) {
    ToyPack t;
    synth::Mixer m(t.pack);
    std::vector<int16_t> out(500, 1);
    m.render(out.data(), out.size());
    bool silent = true;
    for (int16_t v : out) silent = silent && v == 0;
    CHECK(silent);
    CHECK_EQ(m.position(), 500u);
}

TEST(test_synth_one_shot_and_loop) {
    ToyPack t;
    synth::Mixer m(t.pack);
    m.event(ev(activity::Sound::MotorOn));
    std::vector<int16_t> out(300);
    m.render(out.data(), out.size());
    CHECK_EQ(out[0], 100);    // spin-up
    CHECK_EQ(out[99], 100);
    CHECK_EQ(out[100], 10);   // the loop takes over
    CHECK_EQ(out[299], 10);
    m.event(ev(activity::Sound::MotorOff));
    m.render(out.data(), 200);
    CHECK_EQ(out[0], 100);    // spin-down, loop stopped
    CHECK_EQ(out[100], 0);
}

TEST(test_synth_steals_the_oldest_voice) {
    ToyPack t;
    synth::Mixer m(t.pack);
    std::vector<int16_t> out(1);
    for (int i = 0; i < 40; ++i) {
        m.event(ev(activity::Sound::Bump));
        m.render(out.data(), 1);
    }
    CHECK_EQ(m.voicesStolen(), 8u);
    CHECK_EQ(out[0], 32 * 100);  // 32 voices at once, no more
}

TEST(test_synth_saturates) {
    std::vector<int16_t> loud(10, 30000);
    synth::Pack p;
    p.rate = 22050;
    p.step = synth::Sample{loud.data(), 10};
    synth::Mixer m(p);
    m.event(ev(activity::Sound::Step));
    m.event(ev(activity::Sound::Step));
    std::vector<int16_t> out(10);
    m.render(out.data(), out.size());
    CHECK_EQ(out[0], 32767);
}

TEST(test_synth_pack_complete) {
    ToyPack t;
    CHECK(t.pack.complete());
    t.pack.insert = t.pack.eject = synth::Sample{};
    CHECK(t.pack.complete());  // optional
    t.pack.seek = synth::Sample{};
    CHECK(!t.pack.complete());
}

// A held tone, for the volume tests.
struct Tone {
    std::vector<int16_t> data = std::vector<int16_t>(4000, -1000);
    synth::Pack pack;
    Tone() {
        pack.rate = 22050;
        pack.spinup = synth::Sample{data.data(), uint32_t(data.size())};
    }
};

TEST(test_synth_volume_unity_is_the_mix) {
    Tone t;
    synth::Mixer m(t.pack);
    CHECK_EQ(m.gain(), synth::Mixer::UNITY);
    m.setVolume(100);
    m.event(ev(activity::Sound::MotorOn));
    std::vector<int16_t> out(100);
    m.render(out.data(), out.size());
    bool same = true;
    for (int16_t v : out) same = same && v == -1000;
    CHECK(same);  // what the player makes, exactly
}

TEST(test_synth_volume_glides) {
    Tone t;
    synth::Mixer m(t.pack);
    m.event(ev(activity::Sound::MotorOn));
    m.setVolume(50);  // a quarter of the gain: 50% squared
    std::vector<int16_t> out(1000);
    m.render(out.data(), out.size());
    // Down from the full level a little at a time: no click.
    bool smooth = true;
    for (size_t i = 1; i < out.size(); ++i) smooth = smooth && std::abs(out[i] - out[i - 1]) <= 5;
    CHECK(smooth);
    CHECK(out[0] < -990);
    CHECK_EQ(out[synth::Mixer::VOLUME_RAMP], -250);  // there within the ramp
    CHECK_EQ(out[999], -250);
    m.setVolume(0);
    m.render(out.data(), out.size());
    CHECK_EQ(out[999], 0);
    CHECK_EQ(m.gain(), 0u);
}

TEST(test_synth_pack_info) {
    ToyPack t;
    CHECK_EQ(t.pack.loadedMask(), 0xFF);
    t.pack.insert = synth::Sample{};
    CHECK_EQ(t.pack.loadedMask(), 0xBF);  // bit 6: insert, as ui::SOUND_NAMES
    CHECK_EQ(t.pack.bytes(), uint32_t(2 * (5 * 100 + 2 * 10)));
}

}  // namespace

void runSynthTests() {
    RUN(test_wav_stand_in_pack);
    RUN(test_synth_silent_without_events);
    RUN(test_synth_one_shot_and_loop);
    RUN(test_synth_steals_the_oldest_voice);
    RUN(test_synth_saturates);
    RUN(test_synth_pack_complete);
    RUN(test_synth_volume_unity_is_the_mix);
    RUN(test_synth_volume_glides);
    RUN(test_synth_pack_info);
}
