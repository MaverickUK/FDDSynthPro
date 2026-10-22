// Trace events (recorded on core 1) and their text form (printed on core 0).
// Pure C++, host-tested.
//
// Every line starts with a keyword so the Python test runner can parse it:
//   SIG <t_us> <NAME>=ON|OFF ...           input lines that changed, in pin order
//   STEP <t_us> IN|OUT x<count> <span_us>  a run of steps in one direction
// ON means asserted (the line is low). IN is towards higher cylinders (DIR
// asserted). A run of steps becomes one STEP line: <t_us> is the first
// step's time, x<count> how many, <span_us> the time from the first to the last.
#pragma once

#include <cstddef>
#include <cstdint>

enum class TraceKind : uint8_t { Change, Step };

struct TraceEvent {
    uint32_t time_us;
    uint32_t pins;     // the pin sample at the time (fdd_signals.h)
    uint32_t changed;  // Change: the sample bits that changed. Step: 0.
    TraceKind kind;
};

struct TraceRun {
    TraceEvent first;
    uint32_t count;
    uint32_t last_us;
};

// Merges consecutive steps in the same direction (a seek or a recalibrate
// is up to 80+ of them) into one run, so a seek reads as one line.
class TraceCoalescer {
public:
    // Adds an event. Returns true and fills `done` when this ends a run that
    // should now be printed. Changes are never merged.
    bool add(const TraceEvent& e, TraceRun& done);
    // Ends the current run, if any. Returns true and fills `done` if there was one.
    bool flush(TraceRun& done);

private:
    bool has_run_ = false;
    TraceRun run_{};
};

// Formats a run as one line with a trailing newline. Returns the length
// (truncated to size - 1 if the buffer is too small).
size_t formatRun(const TraceRun& run, char* buf, size_t size);

// Formats the input lines of a sample as "PINS DENSEL=OFF MOTEA=OFF ...".
size_t formatPins(uint32_t pins, char* buf, size_t size);
