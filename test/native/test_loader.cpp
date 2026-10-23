// Tests for slice 3's loading: image geometry, track sources and the
// cylinder loader, ending with a timing simulation of seeks against the
// host's settle time.
#include <cstdint>
#include <vector>

#include "check.h"
#include "cylinder_loader.h"
#include "rdata_stream.h"
#include "track_source.h"

// ---- Image geometry ----------------------------------------------------------

TEST(test_format_from_image_size) {
    CHECK(formatForImageSize(1474560) == &ibm::HD_1440);
    CHECK(formatForImageSize(737280) == &ibm::DD_720);
    CHECK(formatForImageSize(368640) == nullptr);   // 360 KB, 5.25-inch: not yet
    CHECK(formatForImageSize(1474561) == nullptr);
    CHECK(formatForImageSize(0) == nullptr);
    CHECK_EQ(imageTrackOffset(ibm::HD_1440, 0, 1), 9216u);
    CHECK_EQ(imageTrackOffset(ibm::HD_1440, 79, 1), 1474560u - 9216u);
    CHECK_EQ(imageTrackOffset(ibm::DD_720, 1, 0), 9216u);
}

// ---- A source to test with ---------------------------------------------------

// The generated disk, with tracks that can be made to fail, and a log of reads.
class FakeSource : public TrackSource {
public:
    explicit FakeSource(const ibm::Format& fmt) : gen_(fmt) {}
    const ibm::Format& format() const override { return gen_.format(); }
    bool readTrack(uint8_t cylinder, uint8_t head, uint8_t* out) override {
        reads.push_back(cylinder * 2 + head);
        if (cylinder == failing_cylinder && head == failing_head) return false;
        return gen_.readTrack(cylinder, head, out);
    }

    std::vector<int> reads;  // cylinder * 2 + head
    int failing_cylinder = -1;
    int failing_head = -1;

private:
    GeneratedSource gen_;
};

// A loader and the buffers it needs, sized for any format we serve.
struct Rig {
    explicit Rig(const ibm::Format& fmt = ibm::HD_1440)
        : source(fmt),
          slots(fmt.track_bytes),
          scratch(fmt.track_bytes),
          d0(fmt.trackDataBytes()),
          d1(fmt.trackDataBytes()),
          loader(source, slots.data(), scratch.data(), d0.data(), d1.data()) {}

    void runUntilIdle() {
        for (int i = 0; i < 100 && loader.service() != CylinderLoader::Step::Idle; ++i) {}
    }

    // The buffer as buildCylinder would make it for `cylinder`.
    bool holds(uint8_t cylinder) {
        const ibm::Format& fmt = source.format();
        std::vector<uint8_t> a(fmt.trackDataBytes()), b(fmt.trackDataBytes());
        gen_disk::track(fmt, cylinder, 0, a.data());
        gen_disk::track(fmt, cylinder, 1, b.data());
        std::vector<uint16_t> s(fmt.track_bytes);
        std::vector<uint32_t> want(fmt.track_bytes);
        rdata::buildCylinder(fmt, cylinder, a.data(), b.data(), s.data(), want.data());
        return want == slots;
    }

    // True if one head's cells are a blank track.
    bool headIsBlank(uint8_t head) {
        std::vector<uint16_t> blank(source.format().track_bytes);
        ibm::buildBlankTrack(source.format(), blank.data());
        for (size_t i = 0; i < slots.size(); ++i)
            if (rdata::headCells(slots[i], head) != blank[i]) return false;
        return true;
    }

    FakeSource source;
    std::vector<uint32_t> slots;
    std::vector<uint16_t> scratch;
    std::vector<uint8_t> d0, d1;
    CylinderLoader loader;
};

using Step = CylinderLoader::Step;

// ---- The loader ----------------------------------------------------------------

TEST(test_loader_loads_in_four_steps) {
    Rig r;
    CHECK_EQ(r.loader.ready(), -1);
    CHECK(r.loader.service() == Step::ReadHead0);
    CHECK(r.loader.service() == Step::ReadHead1);
    CHECK_EQ(r.loader.ready(), -1);
    CHECK(r.loader.service() == Step::EncodeHead0);
    CHECK(r.loader.service() == Step::EncodeHead1);
    CHECK_EQ(r.loader.ready(), 0);
    CHECK(r.loader.service() == Step::Idle);
    CHECK(r.holds(0));
    CHECK_EQ(r.loader.loads(), 1u);
    CHECK_EQ(r.source.reads.size(), 2u);
}

TEST(test_loader_follows_the_head) {
    Rig r;
    r.runUntilIdle();
    r.loader.request(41);
    r.runUntilIdle();
    CHECK_EQ(r.loader.ready(), 41);
    CHECK(r.holds(41));
    CHECK(r.loader.service() == Step::Idle);
}

TEST(test_loader_restarts_when_the_head_moves) {
    Rig r;
    r.runUntilIdle();
    r.loader.request(5);
    r.loader.service();  // read head 0 of 5
    r.loader.service();  // read head 1 of 5
    r.loader.request(6);
    CHECK(r.loader.service() == Step::ReadHead0);  // straight on to 6
    r.runUntilIdle();
    CHECK_EQ(r.loader.ready(), 6);
    CHECK(r.holds(6));
    CHECK_EQ(r.loader.restarts(), 1u);
    CHECK_EQ(r.loader.loads(), 2u);  // 0 and 6: 5 was never finished
}

TEST(test_loader_clears_ready_from_the_first_write) {
    Rig r;
    r.loader.request(3);
    r.runUntilIdle();
    r.loader.request(4);
    r.loader.service();
    r.loader.service();
    CHECK_EQ(r.loader.ready(), 3);  // reading only: the buffer still holds 3
    r.loader.service();
    CHECK_EQ(r.loader.ready(), -1);  // half of 4 is in the buffer now
    r.loader.request(3);             // even going back to 3 needs a full load
    r.runUntilIdle();
    CHECK_EQ(r.loader.ready(), 3);
    CHECK(r.holds(3));
}

TEST(test_loader_head_moving_back_during_reads) {
    Rig r;
    r.loader.request(10);
    r.runUntilIdle();
    r.loader.request(11);
    r.loader.service();  // read 11 head 0: the buffer is untouched
    r.loader.request(10);
    CHECK(r.loader.service() == Step::Idle);  // still holds 10
    CHECK_EQ(r.loader.ready(), 10);
}

TEST(test_loader_blank_beyond_the_image) {
    Rig r;
    r.loader.request(82);
    r.runUntilIdle();
    CHECK_EQ(r.loader.ready(), 82);
    CHECK(r.headIsBlank(0));
    CHECK(r.headIsBlank(1));
}

TEST(test_loader_read_error_blanks_one_head) {
    Rig r;
    r.source.failing_cylinder = 7;
    r.source.failing_head = 1;
    r.loader.request(7);
    r.runUntilIdle();
    CHECK_EQ(r.loader.ready(), 7);
    CHECK(r.headIsBlank(1));
    CHECK(!r.headIsBlank(0));
}

TEST(test_loader_dd_format) {
    Rig r(ibm::DD_720);
    r.loader.request(79);
    r.runUntilIdle();
    CHECK(r.holds(79));
}

// ---- Timing: is the cylinder ready when the host reads? -------------------------
//
// An event simulation: the host steps at fixed intervals while the loader
// works through its steps, each taking a fixed time. The loader only sees a
// new cylinder between steps, as on core 0. The answer is how long after the
// host's last step the buffer holds its cylinder; the host waits 15 ms (its
// settle time) before reading.

struct Costs {
    uint32_t read_us;    // one track from the SD card
    uint32_t encode_us;  // one track encoded into the buffer
};

static uint32_t readyAfterLastStep(int from, int to, uint32_t step_interval_us, uint32_t first_step_us,
                                   Costs c) {
    Rig r;
    r.loader.request(uint8_t(from));
    r.runUntilIdle();
    int dir = to > from ? 1 : -1;
    int steps = (to - from) * dir;
    uint32_t last_step = first_step_us + uint32_t(steps - 1) * step_interval_us;
    uint32_t t = 0;
    int taken = 0;
    for (int guard = 0; guard < 100000; ++guard) {
        while (taken < steps && first_step_us + uint32_t(taken) * step_interval_us <= t) {
            ++taken;
            r.loader.request(uint8_t(from + dir * taken));
        }
        if (taken == steps && r.loader.ready() == to) return t - last_step;
        Step s = r.loader.service();
        if (s == Step::Idle)
            t = first_step_us + uint32_t(taken) * step_interval_us;  // wait for the next step
        else
            t += (s == Step::ReadHead0 || s == Step::ReadHead1) ? c.read_us : c.encode_us;
    }
    return UINT32_MAX;
}

// The worst case over the phase of the steps against the loader's work.
static uint32_t worstReady(int from, int to, uint32_t interval_us, Costs c) {
    uint32_t worst = 0;
    // About 12 phases per interval: enough to land a step at every point of a
    // load, which is 11 ms long.
    uint32_t stride = interval_us / 12 > 250 ? interval_us / 12 : 250;
    for (uint32_t phase = 0; phase < interval_us; phase += stride) {
        uint32_t v = readyAfterLastStep(from, to, interval_us, phase, c);
        if (v > worst) worst = v;
    }
    return worst;
}

TEST(test_ready_within_settle_time) {
    // SD over SDIO: a 9 KB track in about 3 ms (3 MB/s; cards manage more).
    // Encoding a track: assumed 2.5 ms on the RP2350 until measured
    // (build_us on the board; bringup.md step 8).
    const Costs sdio{3000, 2500};
    const uint32_t SETTLE_US = 15000;
    // The longest a load can take after the last step: the step in progress
    // when the head arrives, then a whole load.
    const uint32_t bound = 3000 + 2 * 3000 + 2 * 2500;
    struct { int from, to; uint32_t interval; } seeks[] = {
        {0, 1, 3000},     // one step, PC step rate
        {0, 79, 3000},    // a full seek at the PC's fastest rate
        {79, 0, 10000},   // Greaseweazle
        {0, 79, 26000},   // the Feather host's 13 + 13 ms steps
        {40, 41, 26000},
    };
    for (auto& s : seeks) {
        uint32_t worst = worstReady(s.from, s.to, s.interval, sdio);
        CHECK(worst <= bound);
        CHECK(worst < SETTLE_US);
    }
}

TEST(test_slow_card_misses_the_settle_time) {
    // SPI at about 1 MB/s: 9 ms a track. Too slow for 15 ms; the host then
    // finds no sectors on its first revolution and retries on the next.
    const Costs spi{9000, 2500};
    CHECK(worstReady(0, 1, 3000, spi) > 15000);
}

void runLoaderTests() {
    RUN(test_format_from_image_size);
    RUN(test_loader_loads_in_four_steps);
    RUN(test_loader_follows_the_head);
    RUN(test_loader_restarts_when_the_head_moves);
    RUN(test_loader_clears_ready_from_the_first_write);
    RUN(test_loader_head_moving_back_during_reads);
    RUN(test_loader_blank_beyond_the_image);
    RUN(test_loader_read_error_blanks_one_head);
    RUN(test_loader_dd_format);
    RUN(test_ready_within_settle_time);
    RUN(test_slow_card_misses_the_settle_time);
}
