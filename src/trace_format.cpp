#include "trace_format.h"

#include <cstdarg>
#include <cstdio>

#include "config.h"
#include "fdd_signals.h"

static bool stepsIn(uint32_t pins) { return fdd::asserted(pins, cfg::PIN_DIR); }

bool TraceCoalescer::add(const TraceEvent& e, TraceRun& done) {
    if (has_run_ && e.kind == TraceKind::Step && run_.first.kind == TraceKind::Step &&
        stepsIn(e.pins) == stepsIn(run_.first.pins) && e.time_us - run_.last_us <= cfg::STEP_RUN_GAP_US) {
        ++run_.count;
        run_.last_us = e.time_us;
        return false;
    }
    bool had = has_run_;
    if (had) done = run_;
    run_ = TraceRun{e, 1, e.time_us};
    has_run_ = true;
    return had;
}

bool TraceCoalescer::flush(TraceRun& done) {
    if (!has_run_) return false;
    done = run_;
    has_run_ = false;
    return true;
}

namespace {

// Builds a line in a fixed buffer, truncating at size - 1.
class Line {
public:
    Line(char* buf, size_t size) : buf_(buf), size_(size) {
        if (size_) buf_[0] = '\0';
    }

    void add(const char* fmt, ...) {
        if (n_ + 1 >= size_) return;
        va_list args;
        va_start(args, fmt);
        int w = std::vsnprintf(buf_ + n_, size_ - n_, fmt, args);
        va_end(args);
        if (w > 0) n_ = (n_ + size_t(w) >= size_) ? size_ - 1 : n_ + size_t(w);
    }

    // " NAME=ON|OFF" for each pin in `which`, in pin order.
    void addSignals(uint32_t pins, uint32_t which) {
        for (unsigned pin = cfg::BUS_FIRST_PIN; pin <= cfg::BUS_LAST_PIN; ++pin)
            if (which & fdd::maskOf(pin))
                add(" %s=%s", fdd::signalName(pin), fdd::asserted(pins, pin) ? "ON" : "OFF");
    }

    size_t length() const { return n_; }

private:
    char* buf_;
    size_t size_;
    size_t n_ = 0;
};

}  // namespace

size_t formatRun(const TraceRun& run, char* buf, size_t size) {
    Line line(buf, size);
    const TraceEvent& e = run.first;
    if (e.kind == TraceKind::Step) {
        line.add("STEP %lu %s x%lu %lu", (unsigned long)e.time_us, stepsIn(e.pins) ? "IN" : "OUT",
                 (unsigned long)run.count, (unsigned long)(run.last_us - e.time_us));
    } else {
        line.add("SIG %lu", (unsigned long)e.time_us);
        line.addSignals(e.pins, e.changed);
    }
    line.add("\n");
    return line.length();
}

size_t formatPins(uint32_t pins, char* buf, size_t size) {
    Line line(buf, size);
    line.add("PINS");
    line.addSignals(pins, fdd::INPUT_MASK);
    line.add("\n");
    return line.length();
}
