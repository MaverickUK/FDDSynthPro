#include "short_test.h"

#include <cstdio>

#include "config.h"
#include "fdd_signals.h"

namespace short_test {

uint32_t drivenMask() { return (fdd::INPUT_MASK | fdd::OUTPUT_MASK) << cfg::BUS_FIRST_PIN; }

uint32_t observedMask() { return drivenMask() | 0xFu | (1u << 23) | EXPECTED_LOW; }

const char* signalName(unsigned gpio) {
    static const char* const names[32] = {
        "GPIO0",           "GPIO1",          "GPIO2 (I2C SDA)", "GPIO3 (I2C SCL)",
        "GPIO4 (INDEX)",   "GPIO5 (TRK00)",  "GPIO6 (WPT)",     "GPIO7 (RDATA)",
        "GPIO8 (PSRAM CS)", "GPIO9 (SD CS)", "GPIO10 (SD SCK)", "GPIO11 (SD MOSI)",
        "GPIO12 (DENSEL)", "GPIO13 (MOTEA)", "GPIO14 (DRVSB)",  "GPIO15 (DRVSA)",
        "GPIO16 (MOTEB)",  "GPIO17 (DIR)",   "GPIO18 (STEP)",   "GPIO19 (WDATA)",
        "GPIO20 (WGATE)",  "GPIO21 (SIDE1)", "GPIO22 (DSKCHG)", "GPIO23 (CHNG)",
        "GPIO24 (SD MISO)", "GPIO25 (LED)",  "GPIO26 (AMP DIN)", "GPIO27 (AMP BCLK)",
        "GPIO28 (AMP LRC)", "GPIO29",        "GPIO30",          "GPIO31",
    };
    return gpio < 32 ? names[gpio] : "?";
}

Result analyse(uint32_t baseline_low, const uint32_t low_when_driven[32]) {
    Result r;
    uint32_t observed = observedMask();
    r.stuck_low = baseline_low & observed & ~EXPECTED_LOW;
    r.untested = drivenMask() & baseline_low;
    for (unsigned a = 0; a < 32; ++a) {
        if (!((drivenMask() >> a) & 1) || ((r.untested >> a) & 1)) continue;
        // Pins that went low because a was driven, beyond those already low.
        uint32_t followers = low_when_driven[a] & observed & ~baseline_low & ~(1u << a);
        for (unsigned b = 0; b < 32; ++b) {
            if ((followers >> b) & 1) {
                r.bridged[a] |= 1u << b;
                r.bridged[b] |= 1u << a;
            }
        }
    }
    for (unsigned a = 0; a < 32; ++a)
        for (unsigned b = a + 1; b < 32; ++b)
            if ((r.bridged[a] >> b) & 1) ++r.pairs;
    return r;
}

void report(const Result& r, void (*emit)(const char*, void*), void* ctx) {
    char line[96];
    if (r.ok()) {
        unsigned n = 0;
        for (uint32_t m = drivenMask(); m; m &= m - 1) ++n;
        std::snprintf(line, sizeof line, "SHORTTEST OK %u pins", n);
        emit(line, ctx);
        return;
    }
    for (unsigned g = 0; g < 32; ++g) {
        if ((r.stuck_low >> g) & 1) {
            std::snprintf(line, sizeof line, "STUCK %s low", signalName(g));
            emit(line, ctx);
        }
    }
    for (unsigned a = 0; a < 32; ++a) {
        for (unsigned b = a + 1; b < 32; ++b) {
            if ((r.bridged[a] >> b) & 1) {
                std::snprintf(line, sizeof line, "SHORT %s %s", signalName(a), signalName(b));
                emit(line, ctx);
            }
        }
    }
    unsigned stuck = 0;
    for (uint32_t m = r.stuck_low; m; m &= m - 1) ++stuck;
    std::snprintf(line, sizeof line, "SHORTTEST FAIL %u stuck, %u bridged", stuck, r.pairs);
    emit(line, ctx);
}

}  // namespace short_test
