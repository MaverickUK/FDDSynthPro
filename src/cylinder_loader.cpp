#include "cylinder_loader.h"

#include <cstring>

#include "rdata_stream.h"

bool CylinderLoader::writeSector(uint8_t cylinder, uint8_t head, uint8_t sector, const uint8_t* data) {
    const ibm::Format& fmt = source_.format();
    int image_cylinder = fmt.imageCylinder(cylinder);
    if (ready() != cylinder || loading_ >= 0 || head > 1 || image_cylinder < 0 || !present_[head]) return false;
    const ibm::Track& t = fmt.track(uint8_t(image_cylinder), head);
    if (sector >= t.sectors) return false;
    std::memcpy(data_[head] + sector * t.sectorBytes(), data, t.sectorBytes());
    if (!ibm::buildTrack(fmt, uint8_t(image_cylinder), head, data_[head], scratch_)) return false;
    rdata::setHead(head, scratch_, fmt.track_bytes, slots_);
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
        if (ready_.load(std::memory_order_relaxed) == wanted_) return Step::Idle;
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
