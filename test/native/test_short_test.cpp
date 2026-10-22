// Tests for the solder-bridge analysis (short_test), against simulated
// boards with known faults.
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "check.h"
#include "short_test.h"

using namespace short_test;

// A simulated board: groups of pins bridged together, and pins shorted to GND.
struct Board {
    std::vector<uint32_t> groups;  // each a mask of mutually bridged pins
    uint32_t to_gnd = 0;

    uint32_t groupOf(unsigned pin) const {
        for (uint32_t g : groups)
            if ((g >> pin) & 1) return g;
        return 1u << pin;
    }
    // Low with only pull-ups: GND shorts, the LED's pull-down, and anything bridged to them.
    uint32_t baseline() const {
        uint32_t low = 0;
        for (unsigned p = 0; p < 32; ++p)
            if (((to_gnd | EXPECTED_LOW) >> p) & 1) low |= groupOf(p);
        return low & observedMask();
    }
    void measure(uint32_t& base, uint32_t low_when_driven[32]) const {
        base = baseline();
        for (unsigned p = 0; p < 32; ++p)
            low_when_driven[p] = ((drivenMask() >> p) & 1) ? (base | groupOf(p)) & observedMask() : 0;
    }
};

static std::vector<std::string> run(const Board& b, Result* out = nullptr) {
    uint32_t base, low[32];
    b.measure(base, low);
    Result r = analyse(base, low);
    if (out) *out = r;
    std::vector<std::string> lines;
    report(r, [](const char* line, void* ctx) { static_cast<std::vector<std::string>*>(ctx)->push_back(line); },
           &lines);
    return lines;
}

static uint32_t pins(std::initializer_list<unsigned> list) {
    uint32_t m = 0;
    for (unsigned p : list) m |= 1u << p;
    return m;
}

TEST(test_short_masks) {
    CHECK_EQ(drivenMask(), 0x0007FFF0u);  // GPIO 4-18
    CHECK((observedMask() & 0xF) == 0xF);
    CHECK((observedMask() >> 19) & 1);
    CHECK_STR(signalName(10), "GPIO10 (STEP)");
}

TEST(test_short_clean_board) {
    Result r;
    auto lines = run(Board{}, &r);
    CHECK(r.ok());
    CHECK_EQ(lines.size(), 1u);
    CHECK_STR(lines[0], "SHORTTEST OK 15 pins");
}

TEST(test_short_one_bridge) {
    Result r;
    auto lines = run(Board{{pins({9, 10})}}, &r);
    CHECK_EQ(r.pairs, 1u);
    CHECK_EQ(lines.size(), 2u);
    CHECK_STR(lines[0], "SHORT GPIO9 (DIR) GPIO10 (STEP)");
    CHECK_STR(lines[1], "SHORTTEST FAIL 0 stuck, 1 bridged");
}

TEST(test_short_three_way_bridge) {
    Result r;
    run(Board{{pins({9, 10, 11})}}, &r);
    CHECK_EQ(r.pairs, 3u);
}

TEST(test_short_to_an_observed_neighbour) {
    // DENSEL (GPIO4) bridged to GPIO3 before it on the header, and DSKCHG
    // (GPIO18) to GPIO19 after it.
    auto lines = run(Board{{pins({3, 4}), pins({18, 19})}});
    CHECK_STR(lines[0], "SHORT GPIO3 (I2C SCL) GPIO4 (DENSEL)");
    CHECK_STR(lines[1], "SHORT GPIO18 (DSKCHG) GPIO19");
}

TEST(test_short_to_ground) {
    Result r;
    auto lines = run(Board{{}, pins({15})}, &r);
    CHECK_EQ(r.stuck_low, pins({15}));
    CHECK_EQ(r.untested, pins({15}));  // never driven
    CHECK_STR(lines[0], "STUCK GPIO15 (TRK00) low");
    CHECK_STR(lines.back(), "SHORTTEST FAIL 1 stuck, 0 bridged");
}

TEST(test_short_separate_faults) {
    Result r;
    run(Board{{pins({5, 6}), pins({12, 13})}, pins({16})}, &r);
    CHECK_EQ(r.pairs, 2u);
    CHECK_EQ(r.stuck_low, pins({16}));
    CHECK(!r.ok());
}

void runShortTestTests() {
    RUN(test_short_masks);
    RUN(test_short_clean_board);
    RUN(test_short_one_bridge);
    RUN(test_short_three_way_bridge);
    RUN(test_short_to_an_observed_neighbour);
    RUN(test_short_to_ground);
    RUN(test_short_separate_faults);
}
