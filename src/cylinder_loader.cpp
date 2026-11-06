#include "cylinder_loader.h"

#include <cstring>

#include "rdata_stream.h"

bool CylinderLoader::writeSector(uint8_t cylinder, uint8_t head, uint8_t sector, const uint8_t* data) {
    return storeSector(cylinder, head, sector, data) && encodeHead(cylinder, head);
}

bool CylinderLoader::storeSector(uint8_t cylinder, uint8_t head, uint8_t sector, const uint8_t* data) {
    const ibm::Format& fmt = source_.format();
    int image_cylinder = fmt.imageCylinder(cylinder);
    if (ready() != cylinder || loading_ >= 0 || head > 1 || image_cylinder < 0 || !present_[head]) return false;
    const ibm::Track& t = fmt.track(uint8_t(image_cylinder), head);
    if (sector >= t.sectors) return false;
    std::memcpy(data_[head] + sector * t.sectorBytes(), data, t.sectorBytes());
    return true;
}

bool CylinderLoader::encodeHead(uint8_t cylinder, uint8_t head) {
    const ibm::Format& fmt = source_.format();
    int image_cylinder = fmt.imageCylinder(cylinder);
    if (ready() != cylinder || loading_ >= 0 || head > 1 || image_cylinder < 0 || !present_[head]) return false;
    if (held_.active && held_.head != head) settleHeld();  // the other head's, first: scratch_ is about to change
    if (!ibm::buildTrack(fmt, uint8_t(image_cylinder), head, data_[head], scratch_)) return false;
    publishHead(head);
    return true;
}

// ---- Showing a write while the buffer streams -------------------------------------------

// Words of unchanged cells that still count as inside a run: a sector's
// data field and CRC change, its sync and mark don't, and a reader inside
// any of it must see all old or all new.
static constexpr uint32_t RUN_GAP_WORDS = 4;

bool CylinderLoader::differs(uint8_t head, uint32_t word) const {
    return rdata::headCells(slots_[word], head) != scratch_[word];
}

void CylinderLoader::copyWords(uint8_t head, uint32_t first, uint32_t count) {
    const uint32_t words = source_.format().track_bytes;
    first %= words;
    uint32_t n = count < words - first ? count : words - first;
    rdata::setHeadWords(head, scratch_ + first, first, n, slots_);
    if (count > n) rdata::setHeadWords(head, scratch_, 0, count - n, slots_);
}

// scratch_ holds `head`'s new track. Copies it over the live buffer starting
// at the word the streamer fetches next, so the copy stays ahead of the head
// all the way round; a run of changed words the head is in is held back.
void CylinderLoader::publishHead(uint8_t head) {
    const uint32_t words = source_.format().track_bytes;
    if (held_.active && held_.head == head) held_.active = false;  // this copy covers it
    uint32_t at = position_ ? position_->nextWord(slots_) : StreamPosition::NOT_STREAMING;
    if (at >= words) {  // not streaming this buffer: no reader to race
        rdata::setHead(head, scratch_, words, slots_);
        return;
    }
    // A changed word the reader may be in: RDATA is up to LAG_WORDS behind `at`.
    uint32_t lag = StreamPosition::LAG_WORDS < words ? StreamPosition::LAG_WORDS : words - 1;
    uint32_t hit = words;
    for (uint32_t back = 0; back <= lag && hit == words; ++back) {
        uint32_t w = (at + words - back) % words;
        if (differs(head, w)) hit = w;
    }
    if (hit == words) {
        copyWords(head, at, words);
        return;
    }
    // The run round it: changed words no more than RUN_GAP_WORDS apart.
    uint32_t first = hit, last = hit;  // inclusive, round the end
    for (uint32_t gap = 0, k = 1; k < words && gap < RUN_GAP_WORDS; ++k) {
        uint32_t w = (hit + words - k) % words;
        if (differs(head, w)) {
            first = w;
            gap = 0;
        } else {
            ++gap;
        }
    }
    for (uint32_t gap = 0, k = 1; k < words && gap < RUN_GAP_WORDS; ++k) {
        uint32_t w = (hit + k) % words;
        if (differs(head, w)) {
            last = w;
            gap = 0;
        } else {
            ++gap;
        }
    }
    uint32_t count = (last + words - first) % words + 1;
    if (count >= words / 2) {  // most of the track: nothing to gain by holding
        copyWords(head, at, words);
        return;
    }
    // The head is in the run: everything after it is ahead of the head, and
    // everything before it behind, so from its end round to its start.
    copyWords(head, (first + count) % words, words - count);
    held_ = Held{true, head, ready(), first, count};
    ++held_runs_;
}

// Copies the held run once the head has left it (rebuilding the head:
// scratch_ is shared). True if it did work (a rebuild).
bool CylinderLoader::settleHeld() {
    if (!held_.active) return false;
    const ibm::Format& fmt = source_.format();
    const uint32_t words = fmt.track_bytes;
    if (ready() != held_.cylinder) {  // a load rewrites it anyway
        held_.active = false;
        return false;
    }
    // Clear of it: not inside it (nor RDATA, behind), nor about to be within
    // `lead` words, the time the rebuild takes (an encode, 3 ms on the board,
    // is 190 words at 500 kbit/s; a sixteenth of the track is 780).
    auto clear = [&](uint32_t lead) {
        uint32_t at = position_ ? position_->nextWord(slots_) : StreamPosition::NOT_STREAMING;
        if (at >= words) return true;
        uint32_t into = (at + lead + words - held_.first) % words;
        return into >= held_.count + StreamPosition::LAG_WORDS + lead;
    };
    if (!clear(words / 16)) return false;
    int image_cylinder = fmt.imageCylinder(uint8_t(held_.cylinder));
    if (image_cylinder < 0 || !ibm::buildTrack(fmt, uint8_t(image_cylinder), held_.head, data_[held_.head], scratch_)) {
        held_.active = false;
        return false;
    }
    if (!clear(0)) return true;  // slower than it should be: try again next pass
    held_.active = false;
    copyWords(held_.head, held_.first, held_.count);
    return true;
}

CylinderLoader::Step CylinderLoader::service() {
    if (loading_ >= 0 && loading_ != wanted_) {
        // The head moved: start again for where it is now.
        ++restarts_;
        loading_ = -1;
    }
    const ibm::Format& fmt = source_.format();
    const bool raw = fmt.encoding == ibm::ENCODING_RAW;
    if (loading_ < 0) {
        if (ready_.load(std::memory_order_relaxed) == wanted_) return settleHeld() ? Step::Held : Step::Idle;
        held_.active = false;  // the load rewrites the buffer
        loading_ = wanted_;
        next_ = raw ? Step::EncodeHead0 : Step::ReadHead0;  // a flux track needs no reading apart
    }

    int image_cylinder = fmt.imageCylinder(unsigned(loading_));
    uint8_t cyl = uint8_t(image_cylinder);  // when there is one
    Step done = next_;
    switch (next_) {
        case Step::ReadHead0:
        case Step::ReadHead1: {
            uint8_t head = next_ == Step::ReadHead0 ? 0 : 1;
            present_[head] = image_cylinder >= 0 && hasTrack(fmt, cyl, head) && source_.readTrack(cyl, head, data_[head]);
            next_ = head == 0 ? Step::ReadHead1 : Step::EncodeHead0;
            break;
        }
        case Step::EncodeHead0:
        case Step::EncodeHead1: {
            uint8_t head = next_ == Step::EncodeHead0 ? 0 : 1;
            ready_.store(-1, std::memory_order_release);  // before the first write into the live buffer
            if (raw) {
                // The cells themselves, two bytes a word, MSB first.
                uint8_t* bytes = reinterpret_cast<uint8_t*>(scratch_);
                present_[head] =
                    image_cylinder >= 0 && hasTrack(fmt, cyl, head) && source_.readTrack(cyl, head, bytes);
                weak_count_[head] = 0;
                if (present_[head]) {
                    for (uint32_t i = 0; i < fmt.track_bytes; ++i)
                        scratch_[i] = uint16_t(bytes[2 * i] << 8 | bytes[2 * i + 1]);
                    findWeak(head, fmt.track_bytes);
                    for (unsigned w = 0; w < weak_count_[head]; ++w) noise(scratch_, fmt.track_bytes, weak_[head][w]);
                } else {
                    ibm::buildBlankTrack(fmt, scratch_);
                }
            } else if (!present_[head] || !ibm::buildTrack(fmt, cyl, head, data_[head], scratch_)) {
                ibm::buildBlankTrack(fmt, scratch_);
            }
            rdata::setHead(head, scratch_, fmt.track_bytes, slots_);
            if (head == 0) {
                next_ = Step::EncodeHead1;
            } else {
                ready_.store(loading_, std::memory_order_release);  // after the last
                loading_ = -1;
                next_ = Step::Idle;
                ++loads_;
            }
            break;
        }
        case Step::Idle:
        case Step::Held:  // not a load's
            break;
    }
    return done;
}

// ---- Flux images ----------------------------------------------------------------------

static bool cellAt(const uint16_t* cells, uint32_t i) { return (cells[i / 16] >> (15 - i % 16)) & 1; }

void CylinderLoader::findWeak(uint8_t head, uint32_t words) {
    // Runs of 8 or more empty cells: none in any recording, so weak bits.
    uint32_t total = words * 16, run = 0;
    for (uint32_t i = 0; i <= total; ++i) {
        if (i < total && !cellAt(scratch_, i)) {
            ++run;
            continue;
        }
        if (run >= 8 && weak_count_[head] < MAX_WEAK) weak_[head][weak_count_[head]++] = Run{i - run, run};
        run = 0;
    }
}

void CylinderLoader::noise(uint16_t* cells, uint32_t words, const Run& r) {
    // Random flux, as a drive's amplifier makes of no signal: transitions at
    // random, never two cells running.
    bool last = false;
    for (uint32_t k = 0; k < r.cells; ++k) {
        uint32_t i = r.start + k;
        if (i >= words * 16) break;
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        bool one = !last && (rng_ % 3) == 0;
        uint16_t mask = uint16_t(0x8000u >> (i % 16));
        if (one)
            cells[i / 16] |= mask;
        else
            cells[i / 16] &= uint16_t(~mask);
        last = one;
    }
}

void CylinderLoader::refreshWeak() {
    if (!weak_count_[0] && !weak_count_[1]) return;
    if (loading_ >= 0 || ready() < 0) return;
    const uint32_t words = source_.format().track_bytes;
    for (uint8_t head = 0; head < 2; ++head)
        for (unsigned w = 0; w < weak_count_[head]; ++w) {
            const Run& r = weak_[head][w];
            // The words it touches, out of the live buffer, noised, and back.
            uint32_t first = r.start / 16, last = (r.start + r.cells - 1) / 16;
            for (uint32_t i = first; i <= last && i < words; ++i) scratch_[i] = rdata::headCells(slots_[i], head);
            noise(scratch_, words, r);
            for (uint32_t i = first; i <= last && i < words; ++i) rdata::setHeadWords(head, &scratch_[i], i, 1, slots_);
        }
}

bool CylinderLoader::writeCells(uint8_t cylinder, uint8_t head, uint32_t first, const uint16_t* cells, uint32_t n) {
    const ibm::Format& fmt = source_.format();
    if (ready() != cylinder || loading_ >= 0 || head > 1 || fmt.encoding != ibm::ENCODING_RAW) return false;
    const uint32_t words = fmt.track_bytes;
    for (uint32_t k = 0; k < n; ++k) rdata::setHeadWords(head, &cells[k], (first + k) % words, 1, slots_);
    // What was written is real flux now.
    uint32_t from = first % words * 16, to = from + n * 16;
    unsigned kept = 0;
    for (unsigned w = 0; w < weak_count_[head]; ++w) {
        const Run& r = weak_[head][w];
        bool covered = false;
        const uint32_t shifts[2] = {0, words * 16};  // a write round the end covers the start too
        for (uint32_t shift : shifts)
            covered = covered || (r.start + shift < to && r.start + r.cells + shift > from);
        if (!covered) weak_[head][kept++] = r;
    }
    weak_count_[head] = kept;
    return true;
}
