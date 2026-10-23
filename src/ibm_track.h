// The IBM PC track layout in MFM: what one revolution of a 720 KB or 1.44 MB
// disk holds. Pure C++, host-tested against Greaseweazle's own encoder
// (host/tests/test_track_encoder.py), which builds the same track cell for
// cell from the same format.
//
// One track, from the index pulse:
//   gap 4a   gap4a x 4E
//   IAM      12 x 00, C2 C2 C2 (sync), FC
//   gap 1    gap1 x 4E
//   then for each sector, in order (sector IDs from 1, no interleave):
//     ID     12 x 00, A1 A1 A1 (sync), FE, C, H, R, N, CRC
//     gap 2  gap2 x 4E
//     data   12 x 00, A1 A1 A1 (sync), FB, the sector's bytes, CRC
//     gap 3  gap3 x 4E
//   gap 4b   4E to the end of the track
// C, H, R, N: cylinder, head, sector ID, size code (2 = 512 bytes).
#pragma once

#include <cstddef>
#include <cstdint>

namespace ibm {

struct Format {
    uint8_t sectors;
    uint8_t size_code;     // sector bytes = 128 << size_code
    uint8_t gap1;
    uint8_t gap2;
    uint8_t gap3;
    uint8_t gap4a;
    uint16_t track_bytes;  // one revolution: data rate (bit/s) x 0.2 s / 8

    uint32_t sectorBytes() const { return 128u << size_code; }
    uint32_t trackDataBytes() const { return sectors * sectorBytes(); }
    // Data bits per second: a track is one 200 ms revolution.
    uint32_t dataRate() const { return uint32_t(track_bytes) * 8 * 5; }
    // The bytes the layout uses before gap 4b.
    uint32_t usedBytes() const;

    // Where things are on the track, in bytes from the index: the start of
    // the 12 x 00 before sector `s`'s ID field and data field (s from 0).
    // Their sync words follow PRESYNC_BYTES later.
    uint32_t idFieldStart(uint8_t s) const;
    uint32_t dataFieldStart(uint8_t s) const;
};

// Greaseweazle's ibm.1440 and ibm.720 (gap3 108 and 84, the standard gaps
// otherwise). DOS FORMAT writes gap3 84 on 1.44 MB disks; any gap3 reads the
// same.
constexpr Format HD_1440{18, 2, 50, 22, 108, 80, 12500};
constexpr Format DD_720{9, 2, 50, 22, 84, 80, 6250};

constexpr uint8_t MARK_IAM = 0xFC;
constexpr uint8_t MARK_IDAM = 0xFE;
constexpr uint8_t MARK_DAM = 0xFB;
constexpr uint8_t GAP_BYTE = 0x4E;
constexpr unsigned PRESYNC_BYTES = 12;

// Builds one track's cells into `out` (fmt.track_bytes words, one per byte).
// `data` holds the track's sectors in ID order, fmt.trackDataBytes() bytes:
// the same layout as that track's slice of an IMG file. Returns false if the
// layout doesn't fit in the track.
bool buildTrack(const Format& fmt, uint8_t cylinder, uint8_t head, const uint8_t* data, uint16_t* out);

// An unformatted-looking track: gap bytes all the way round, no index mark
// and no sectors. What the drive sends for a cylinder the image doesn't
// have (80-83): the host finds no sectors there, as on a real disk.
void buildBlankTrack(const Format& fmt, uint16_t* out);

}  // namespace ibm
