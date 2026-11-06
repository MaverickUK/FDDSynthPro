// Loads the cylinder under the head into the RDATA streamer's buffer
// (rdata_stream.h), a step at a time. Pure C++, host-tested; core 0 runs it.
//
// A load is four steps: read head 0's track, read head 1's, encode head 0
// into the buffer, encode head 1. Core 0 calls service() on every pass of
// its loop; each call does at most one step, so the console and the trace
// keep going between them.
//
// Cylinders here are the head's (physical) ones; the format maps each to
// the image's (ibm::Format::imageCylinder), and a cylinder or head the image
// doesn't have is sent blank.
//
// A flux image's track (ibm::ENCODING_RAW) is its cells already: a load is
// two steps, a head each, copied in. Runs of 8 or more empty cells are weak
// bits (hfe.h): they go out as random flux, made again by refreshWeak().
//
// If the head moves during a load, the load starts again for the new
// cylinder at the next step: during a seek only the last cylinder is ever
// finished, so it's ready one load after the last step, not two.
//
// The buffer is built in place while the DMA streams it. ready() is -1 from
// the first write into it until the load finishes; the drive keeps RDATA
// released meanwhile, so the host never reads a half-built or stale
// cylinder (stale IDs would look like a seek error).
//
// A write is shown by re-encoding the head and copying it over the live
// buffer while the DMA streams it. The copy is far faster than the disk, so
// it starts at the word the streamer fetches next (StreamPosition) and
// stays ahead of the head; only the run of changed words the head is in
// (a sector being read) is held back, and copied by service() once the head
// has left it. Otherwise the host would read that sector half old and half
// new: a CRC error, and DOS FORMAT's verify failed every first try.
#pragma once

#include <atomic>
#include <cstdint>

#include "track_source.h"

// Where the streamer is in a cylinder buffer: the next word its DMA fetches
// (RDATA is up to LAG_WORDS behind it, in the PIO's FIFO), or NOT_STREAMING
// if it isn't streaming that buffer.
class StreamPosition {
public:
    static constexpr uint32_t NOT_STREAMING = UINT32_MAX;
    static constexpr uint32_t LAG_WORDS = 12;  // the joined TX FIFO's 8, the shift register's 1, and some
    virtual uint32_t nextWord(const uint32_t* slots) const = 0;

protected:
    ~StreamPosition() = default;
};

class CylinderLoader {
public:
    enum class Step : uint8_t { Idle, ReadHead0, ReadHead1, EncodeHead0, EncodeHead1, Held };

    // `slots`: the streamer's buffer, format().track_bytes words.
    // `scratch`: format().track_bytes cell words.
    // `data0`, `data1`: format().maxTrackDataBytes() bytes each
    // (ibm::MAX_TRACK_DATA covers every format).
    CylinderLoader(TrackSource& source, uint32_t* slots, uint16_t* scratch, uint8_t* data0, uint8_t* data1)
        : source_(source), slots_(slots), scratch_(scratch), data_{data0, data1} {}

    // Forgets what the buffer holds, for a new disk: the next service()
    // starts a load. ready() is -1 until it finishes.
    void reset() {
        loading_ = -1;
        next_ = Step::Idle;
        present_[0] = present_[1] = false;
        held_.active = false;
        ready_.store(-1, std::memory_order_release);
    }

    // The cylinder the head is on now. Cheap; call as often as you like.
    void request(uint8_t cylinder) { wanted_ = cylinder; }

    // Where the streamer is, for writes shown while it streams. Without one,
    // a write is copied over the buffer at once, start to end.
    void setStreamPosition(const StreamPosition* position) { position_ = position; }

    // Does the next step of a load, if there is one. Returns the step done
    // (Idle: nothing to do). Between loads, it copies a write's held-back
    // run once the head has left it (Step::Held).
    Step service();

    // A sector the host wrote: puts it in the loaded track data and
    // re-encodes that head into the buffer, so the host reads it back at
    // once. The buffer is rewritten in place with the same cells except the
    // sector's, so RDATA stays valid throughout; a sector under the head is
    // held back until it has passed (see the top). Returns false (and
    // changes nothing) unless the buffer holds `cylinder`.
    bool writeSector(uint8_t cylinder, uint8_t head, uint8_t sector, const uint8_t* data);

    // writeSector() in two halves, for a burst of many sectors: store each
    // into the track data, then encode the head once (an encode is about
    // 3 ms on the board, and a whole track took 18 of them). Until
    // encodeHead(), the host still reads the old cells. Same conditions and
    // results as writeSector().
    bool storeSector(uint8_t cylinder, uint8_t head, uint8_t sector, const uint8_t* data);
    bool encodeHead(uint8_t cylinder, uint8_t head);

    // A flux image's write: `n` words of cells from word `first` of the
    // head's track (round its end), into the live buffer. Weak bits it
    // covers aren't weak any more. False unless the buffer holds `cylinder`.
    bool writeCells(uint8_t cylinder, uint8_t head, uint32_t first, const uint16_t* cells, uint32_t n);

    // New random flux in the loaded cylinder's weak bits: a real drive reads
    // different noise on every revolution. Core 0 calls it often; cheap
    // when there are none.
    void refreshWeak();
    unsigned weakRuns() const { return weak_count_[0] + weak_count_[1]; }

    // The cylinder the buffer holds, or -1 while it is being built. Safe to
    // read from the other core: it turns -1 before the first write into the
    // buffer, and to the cylinder only after the last.
    int ready() const { return ready_.load(std::memory_order_acquire); }
    // The buffer doesn't hold the cylinder asked for yet: a load is under
    // way, against the host's settle time. Core 0 holds back other slow work
    // (a panel frame) until it's done.
    bool loading() const { return ready() != wanted_; }
    // Loads finished, and loads abandoned because the head moved.
    uint32_t loads() const { return loads_; }
    uint32_t restarts() const { return restarts_; }
    // Writes whose run under the head was held back, and one waiting now.
    uint32_t heldRuns() const { return held_runs_; }
    bool holding() const { return held_.active; }

private:
    TrackSource& source_;
    uint32_t* slots_;
    uint16_t* scratch_;
    uint8_t* data_[2];
    bool present_[2] = {false, false};

    // Weak runs in each head's loaded track, in cells.
    struct Run {
        uint32_t start, cells;
    };
    static constexpr unsigned MAX_WEAK = 32;
    void findWeak(uint8_t head, uint32_t words);
    void noise(uint16_t* cells, uint32_t words, const Run& r);
    Run weak_[2][MAX_WEAK];
    unsigned weak_count_[2] = {0, 0};
    uint32_t rng_ = 0x2545F491u;

    // A write's run held back from the copy: words [first, first + count)
    // of `head`'s track (round the end), still the old cells.
    struct Held {
        bool active = false;
        uint8_t head = 0;
        int cylinder = -1;
        uint32_t first = 0, count = 0;
    };
    void publishHead(uint8_t head);
    bool differs(uint8_t head, uint32_t word) const;
    void copyWords(uint8_t head, uint32_t first, uint32_t count);  // from scratch_, round the end
    bool settleHeld();
    const StreamPosition* position_ = nullptr;
    Held held_;
    uint32_t held_runs_ = 0;

    int wanted_ = 0;
    std::atomic<int> ready_{-1};
    int loading_ = -1;  // the cylinder being loaded, or -1
    Step next_ = Step::Idle;
    uint32_t loads_ = 0;
    uint32_t restarts_ = 0;
};
