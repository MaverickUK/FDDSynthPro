#include "mfm_decode.h"

#include "crc16.h"
#include "mfm.h"

namespace mfm {

unsigned DataSeparator::cells(uint32_t interval_ticks) {
    // Time from the last grid point to this edge.
    int32_t t = int32_t(interval_ticks << 8) + residual_x256_;
    int32_t n = (t + cell_x256_ / 2) / cell_x256_;
    if (n < 1) n = 1;
    if (n > 16) n = 16;
    int32_t error = t - n * cell_x256_;  // the edge's distance from its grid point
    // Period: 5% of the error per cell. Kept within 10% of nominal, so a
    // burst of noise can't run the clock away.
    cell_x256_ += error * period_pct_ / (100 * n);
    int32_t lo = nominal_x256_ * 9 / 10, hi = nominal_x256_ * 11 / 10;
    if (cell_x256_ < lo) cell_x256_ = lo;
    if (cell_x256_ > hi) cell_x256_ = hi;
    // Phase: the grid moves 60% of the way to the edge.
    residual_x256_ = error * (100 - phase_pct_) / 100;
    return unsigned(n);
}

void FieldScanner::reset(uint16_t default_data_len) {
    default_len_ = next_len_ = default_data_len;
    state_ = State::Hunting;
    cells_ = shift_ = 0;
    word_bits_ = syncs_ = 0;
    store_pos_ = count_ = 0;
    overflowed_ = 0;
}

void FieldScanner::startField(uint8_t mark) {
    current_ = Field{};
    current_.mark = mark;
    current_.cell = sync_cell_;
    crc_ = crc16::INITIAL;
    for (int i = 0; i < 3; ++i) crc_ = crc16::update(crc_, 0xA1);
    crc_ = crc16::update(crc_, mark);
    body_pos_ = 0;
    if (mark == MARK_IDAM) {
        body_len_ = 6;
    } else {
        body_len_ = size_t(next_len_) + 2;
        if (store_pos_ + next_len_ > store_len_) {
            ++overflowed_;
            state_ = State::Hunting;
            return;
        }
        current_.data_offset = uint32_t(store_pos_);
        current_.data_len = next_len_;
    }
    state_ = State::Body;
}

void FieldScanner::finishField() {
    current_.crc_ok = crc_ == 0;  // the CRC over the field and its own CRC is 0
    if (current_.mark == MARK_IDAM) {
        for (int i = 0; i < 4; ++i) current_.id[i] = header_[i];
        // The data field that follows has the size this ID gives.
        next_len_ = current_.crc_ok && header_[3] <= 3 ? uint16_t(128u << header_[3]) : default_len_;
    } else {
        store_pos_ += current_.data_len;
        next_len_ = default_len_;
    }
    if (count_ < max_fields_)
        fields_[count_++] = current_;
    else
        ++overflowed_;
    state_ = State::Hunting;
}

void FieldScanner::addCell(bool one) {
    shift_ = (shift_ << 1) | (one ? 1u : 0u);
    ++cells_;

    if (state_ == State::Hunting) {
        if ((shift_ & 0xFFFF) == SYNC_A1) {
            state_ = State::Syncs;
            syncs_ = 1;
            sync_cell_ = cells_ - 16;
            word_bits_ = 0;
        }
        return;
    }

    // Aligned to whole words from here on.
    if (++word_bits_ < 16) return;
    word_bits_ = 0;
    uint16_t word = uint16_t(shift_);

    if (state_ == State::Syncs) {
        if (word == SYNC_A1) {
            ++syncs_;
        } else if (syncs_ >= 3) {
            uint8_t mark = decodeWord(word);
            if (mark == MARK_IDAM || mark == MARK_DAM || mark == MARK_DDAM)
                startField(mark);
            else
                state_ = State::Hunting;
        } else {
            state_ = State::Hunting;
        }
        return;
    }

    // Body: one more byte of the field.
    uint8_t byte = decodeWord(word);
    crc_ = crc16::update(crc_, byte);
    if (current_.mark == MARK_IDAM)
        header_[body_pos_] = byte;
    else if (body_pos_ < current_.data_len)
        store_[current_.data_offset + body_pos_] = byte;
    if (++body_pos_ == body_len_) finishField();
}

}  // namespace mfm
