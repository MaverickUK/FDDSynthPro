// Renders ACT lines with the firmware's mixer (src/synth.*), for the Python
// tests to compare sample for sample with the player (host/tests/test_synth.py).
//
//   render_events <pack folder> < trace.txt > out.raw
//
// stdout: 16-bit little-endian mono samples. Timing as player.py: 0.2 s
// before the first event, 0.5 s after the last, each event at
// (t - t0) * rate / 1e6.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "activity.h"
#include "synth.h"
#include "wav.h"

static const int64_t LEAD_IN_US = 200000;
static const int64_t TAIL_US = 500000;

struct Loaded {
    std::vector<int16_t> samples;
    uint32_t rate = 0;
};

static bool load(const std::string& path, Loaded& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> file;
    uint8_t buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) file.insert(file.end(), buf, buf + n);
    std::fclose(f);
    WavInfo info;
    if (parseWavHeader(file.data(), file.size(), uint32_t(file.size()), info) != WavError::Ok || !wavIsPlayable(info))
        return false;
    out.rate = info.sampleRate;
    out.samples.resize(info.dataBytes / 2);
    std::memcpy(out.samples.data(), &file[info.dataOffset], info.dataBytes & ~1u);
    return true;
}

static bool parseEvent(const char* line, int64_t& t, activity::Event& e) {
    char sound[16] = "", a[16] = "", b[16] = "";
    long long us = 0;
    int got = std::sscanf(line, "ACT %lld %15s %15s %15s", &us, sound, a, b);
    if (got < 2) return false;
    using activity::Sound;
    struct { const char* name; Sound s; } names[] = {
        {"MOTOR_ON", Sound::MotorOn}, {"MOTOR_OFF", Sound::MotorOff}, {"STEP", Sound::Step},
        {"SEEK_START", Sound::SeekStart}, {"SEEK_END", Sound::SeekEnd}, {"BUMP", Sound::Bump},
        {"INSERT", Sound::Insert}, {"EJECT", Sound::Eject},
    };
    for (auto& n : names) {
        if (std::strcmp(sound, n.name) == 0) {
            t = us;
            e = activity::Event{uint32_t(us), n.s, std::strcmp(a, "IN") == 0 || std::strcmp(a, "INNER") == 0, 0, 0};
            return true;
        }
    }
    return false;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: render_events <pack folder> < trace > out.raw\n");
        return 2;
    }
    std::string dir = argv[1];
    const char* names[] = {"spinup", "spin", "spindown", "step", "seek", "bump", "insert", "eject"};
    Loaded loaded[8];
    synth::Pack pack;
    synth::Sample* slots[] = {&pack.spinup, &pack.spin, &pack.spindown, &pack.step,
                              &pack.seek,   &pack.bump, &pack.insert,   &pack.eject};
    for (int i = 0; i < 8; ++i) {
        if (!load(dir + "/" + names[i] + ".wav", loaded[i])) continue;
        *slots[i] = synth::Sample{loaded[i].samples.data(), uint32_t(loaded[i].samples.size())};
        pack.rate = loaded[i].rate;
    }
    if (!pack.complete()) {
        std::fprintf(stderr, "%s: not a complete pack\n", dir.c_str());
        return 1;
    }

    std::vector<std::pair<int64_t, activity::Event>> events;
    char line[256];
    while (std::fgets(line, sizeof line, stdin)) {
        int64_t t;
        activity::Event e;
        if (parseEvent(line, t, e)) events.push_back({t, e});
    }
    if (events.empty()) return 1;

    int64_t t0 = events.front().first - LEAD_IN_US;
    int64_t end = events.back().first + TAIL_US;
    size_t total = size_t((end - t0) * pack.rate / 1000000 + 1);
    std::vector<int16_t> out(total);
    synth::Mixer mixer(pack);
    for (auto& [t, e] : events) {
        size_t at = size_t((t - t0) * pack.rate / 1000000);
        if (at > mixer.position()) mixer.render(&out[mixer.position()], at - mixer.position());
        mixer.event(e);
    }
    mixer.render(&out[mixer.position()], total - mixer.position());
    std::fwrite(out.data(), 2, out.size(), stdout);
    std::fprintf(stderr, "samples=%zu stolen=%u\n", out.size(), mixer.voicesStolen());
    return 0;
}
