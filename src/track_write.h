// What a burst of writing did: which sectors of the track it replaced.
// Pure C++, host-tested.
//
// A burst is what the host wrote between asserting and releasing WGATE.
// Two kinds matter:
//   - A sector write, as a PC's controller does it: after reading the
//     sector's ID field, it writes only the data field (12 x 00, A1 A1 A1,
//     FB, the data, the CRC). The burst names no sector, so its position on
//     the track does: its sync must land where that sector's data field is.
//   - A track write (FORMAT, or `gw write`): from the index, the whole track,
//     ID fields included. Each data field then belongs to the ID before it.
//
// Only data fields with a good CRC for this cylinder and head, in this
// image's format, are taken; anything else is counted and left alone (a
// format change, say, which a fixed-geometry image can't hold).
#pragma once

#include <cstddef>
#include <cstdint>

#include "ibm_track.h"
#include "mfm_decode.h"

namespace track_write {

struct Update {
    uint8_t sector;        // from 0
    uint32_t data_offset;  // the bytes, in the scanner's store
};

struct Result {
    unsigned updates = 0;
    unsigned bad_crc = 0;       // data fields that failed their CRC
    unsigned unplaced = 0;      // good data fields that matched no sector
};

// How far a sector write's sync (FM: its mark) may land from where the
// layout puts it, in words at the disk's rate. Sectors are hundreds of
// bytes apart; a host's timing is within a few. Each gap is longer than
// this on both sides.
constexpr uint32_t POSITION_SLACK_BYTES = 16;

// `cylinder`: the image's (-1 where the head is on no track of the image,
// which places nothing). `start_byte`: where on the track WGATE was
// asserted, in bytes from the index. Fills `out` (up to `max`) and returns
// what happened.
Result interpret(const ibm::Format& fmt, int cylinder, uint8_t head, uint32_t start_byte,
                 const mfm::FieldScanner& fields, Update* out, size_t max);

}  // namespace track_write
