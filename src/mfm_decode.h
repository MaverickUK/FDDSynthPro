// Reading MFM back: from flux intervals to cells (a data separator, as in a
// floppy controller), and from cells to ID and data fields. Pure C++,
// host-tested, also against Greaseweazle's encoder (test_write_decode.py).
//
// The drive needs this for writes: the host's WDATA pulses arrive as flux,
// and the drive must find the sectors in them.
#pragma once

#include <cstddef>
#include <cstdint>

#include "crc16.h"
#include "mfm.h"
#include "ram_func.h"

namespace mfm {

// Turns flux intervals into cells: each interval is some 0 cells then a 1.
// A phase-locked loop, as in a floppy controller (and Greaseweazle's
// decoder): it keeps a grid of cell boundaries, places each flux edge on its
// nearest boundary, then moves the grid 60% of the way to the edge (phase)
// and nudges the cell length 5% of the error (period). Each edge is judged
// against the grid, not against the previous edge. Measured
// (test_write_decode.cpp): random jitter up to about 0.2 cell on every edge,
// and a host clock up to about 10% off. Real hosts write with a few percent
// of jitter, plus up to 1/8 cell of write precompensation. Lower gains
// handle more jitter but follow speed errors worse; these are
// Greaseweazle's defaults, proven on real disks.
class DataSeparator {
public:
    // `ticks_per_cell`: the nominal cell length in the capture's ticks.
    explicit DataSeparator(uint32_t ticks_per_cell, int32_t phase_pct = 60, int32_t period_pct = 5)
        : phase_pct_(phase_pct), period_pct_(period_pct) {
        reset(ticks_per_cell);
    }
    void reset(uint32_t ticks_per_cell) {
        nominal_x256_ = cell_x256_ = int32_t(ticks_per_cell << 8);
        lo_x256_ = nominal_x256_ * 9 / 10;
        hi_x256_ = nominal_x256_ * 11 / 10;
        residual_x256_ = 0;
    }

    // How many cells an interval is (1 to 16; its last cell is the 1).
    // Inline: the drive runs it for every flux edge of a write.
    ALWAYS_INLINE unsigned cells(uint32_t interval_ticks) {
        // Time from the last grid point to this edge.
        int32_t t = int32_t(interval_ticks << 8) + residual_x256_;
        int32_t n = (t + cell_x256_ / 2) / cell_x256_;
        if (n < 1) n = 1;
        if (n > 16) n = 16;
        int32_t error = t - n * cell_x256_;  // the edge's distance from its grid point
        // Period: 5% of the error per cell. Kept within 10% of nominal, so a
        // burst of noise can't run the clock away.
        cell_x256_ += error * period_pct_ / (100 * n);
        if (cell_x256_ < lo_x256_) cell_x256_ = lo_x256_;
        if (cell_x256_ > hi_x256_) cell_x256_ = hi_x256_;
        // Phase: the grid moves 60% of the way to the edge.
        residual_x256_ = error * (100 - phase_pct_) / 100;
        return unsigned(n);
    }

    int32_t cellTicksX256() const { return cell_x256_; }

private:
    int32_t phase_pct_;
    int32_t period_pct_;
    int32_t nominal_x256_;
    int32_t lo_x256_, hi_x256_;  // the cell length's limits
    int32_t cell_x256_;      // the cell length now
    int32_t residual_x256_;  // where the last edge was relative to the grid
};

constexpr uint8_t MARK_IDAM = 0xFE;
constexpr uint8_t MARK_DAM = 0xFB;
constexpr uint8_t MARK_DDAM = 0xF8;  // deleted data

struct Field {
    uint8_t mark;
    bool crc_ok;
    uint32_t cell;         // where its first A1 sync word began, in cells from the start of the stream
    uint8_t id[4];         // IDAM: C, H, R, N
    uint32_t data_offset;  // DAM: where its bytes are in the scanner's store
    uint16_t data_len;
};

// Finds fields in a stream of cells: three A1 sync words (0x4489, which only
// a sync can produce), a mark, then the field and its CRC. A data field's
// length comes from the last good ID field's N, or the default given.
//
// In FM there are no sync words: the mark itself is missing clocks (clock C7
// where every other byte's is FF), which only a mark can produce. A field's
// `cell` is then where its mark began.
class FieldScanner {
public:
    // `store` holds data fields' bytes; `fields` the fields found.
    FieldScanner(uint8_t* store, size_t store_len, Field* fields, size_t max_fields)
        : store_(store), store_len_(store_len), fields_(fields), max_fields_(max_fields) {}

    void reset(uint16_t default_data_len, bool fm = false);
    void addCell(bool one);
    // An interval's cells at once: n - 1 zeros, then a 1 (n from 1 to 16).
    // The same as n addCell() calls, and faster: the drive runs it for every
    // flux edge of a write. An MFM sync word (0x4489) ends in a 1, so it can
    // only complete on an interval's last cell, and one check an interval
    // finds every sync. FM marks can end in a 0: FM goes cell by cell.
    ALWAYS_INLINE void addInterval(unsigned n) {
        if (fm_) {
            for (unsigned k = 1; k < n; ++k) addCell(false);
            addCell(true);
            return;
        }
        shift_ = (shift_ << n) | 1u;
        cells_ += n;
        if (state_ == State::Hunting) {
            huntSync();
            return;
        }
        word_bits_ += n;
        if (word_bits_ < 16) return;
        word_bits_ -= 16;  // the cells after the word, this interval's last
        takeWord(uint16_t(shift_ >> word_bits_));
        // The word ended a field or a run of syncs: the cells after it are
        // hunted, and only the last (the 1) can end a sync.
        if (state_ == State::Hunting && word_bits_ > 0) huntSync();
    }

    size_t count() const { return count_; }
    const Field& field(size_t i) const { return fields_[i]; }
    // Fields that didn't fit in `fields` or `store`.
    uint32_t overflowed() const { return overflowed_; }

private:
    enum class State : uint8_t { Hunting, Syncs, Body };
    void startField(uint8_t mark);
    void finishField();
    ALWAYS_INLINE void huntSync() {
        if ((shift_ & 0xFFFF) != SYNC_A1) return;
        state_ = State::Syncs;
        syncs_ = 1;
        sync_cell_ = cells_ - 16;
        word_bits_ = 0;
    }
    // A whole word of cells, once aligned (Syncs or Body).
    ALWAYS_INLINE void takeWord(uint16_t word) {
        if (state_ == State::Syncs) {
            takeSyncWord(word);
            return;
        }
        // Body: one more byte of the field.
        uint8_t byte = decodeWordFast(word);
        crc_ = crc16::updateFast(crc_, byte);
        if (current_.mark == MARK_IDAM)
            header_[body_pos_] = byte;
        else if (body_pos_ < current_.data_len)
            store_[current_.data_offset + body_pos_] = byte;
        if (++body_pos_ == body_len_) finishField();
    }
    void takeSyncWord(uint16_t word);

    uint8_t* store_;
    size_t store_len_;
    Field* fields_;
    size_t max_fields_;

    bool fm_ = false;
    uint16_t default_len_ = 512;
    uint16_t next_len_ = 512;
    State state_ = State::Hunting;
    uint32_t cells_ = 0;     // cells seen
    uint32_t shift_ = 0;     // the last 32 cells
    unsigned word_bits_ = 0; // cells of the current word, once aligned
    unsigned syncs_ = 0;
    uint32_t sync_cell_ = 0;
    Field current_{};
    uint8_t header_[6];      // an ID field's bytes and CRC
    size_t body_len_ = 0;    // bytes the current field needs, CRC included
    size_t body_pos_ = 0;
    uint16_t crc_ = 0;
    size_t store_pos_ = 0;
    size_t count_ = 0;
    uint32_t overflowed_ = 0;
};

}  // namespace mfm
