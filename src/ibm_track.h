// IBM-style disks, MFM or FM: a disk's geometry and each track's layout,
// and the encoder that builds a track from its sector data. Pure C++, host-tested
// against Greaseweazle's own encoder (host/tests/test_track_encoder.py),
// which builds the same tracks cell for cell from the same definitions.
//
// One track, from the index pulse:
//   gap 4a   gap4a x 4E
//   IAM      12 x 00, C2 C2 C2 (sync), FC, then gap 1: gap1 x 4E   (unless iam is off)
//   then for each sector, in rotational order (interleave and skew):
//     ID     12 x 00, A1 A1 A1 (sync), FE, C, H, R, N, CRC
//     gap 2  gap2 x 4E
//     data   12 x 00, A1 A1 A1 (sync), FB, the sector's bytes, CRC
//     gap 3  gap3 x 4E
//   gap 4b   4E to the end of the track
// C, H, R, N: cylinder, head, sector ID, size code (2 = 512 bytes).
//
// FM (single density, the 1970s' recording, as on 8-inch disks, the Osborne
// and the BBC Micro) is the same layout with every byte's clock cells set:
// 6 x 00 before each field, and no sync bytes; the marks themselves carry
// missing clocks (FE and FB with clock C7, the index mark FC with D7). Gaps
// are FF. At the same data rate a track holds half as many bytes.
//
// A track can run at half the disk's rate (`scale` 2: an FM boot track on
// an MFM disk): the streamer runs at the disk's rate, so each of its cells
// goes out as two, the second empty. Positions on a track are in words
// (16 cells) at the disk's rate: bytes x scale.
//
// The parameters are FlashFloppy's IMG.CFG ones (img_cfg.h), resolved the
// way Greaseweazle resolves its disk definitions: gaps and the data rate
// left on auto are chosen from the layout (resolve()).
#pragma once

#include <cstddef>
#include <cstdint>

namespace ibm {

// The streamer's buffers hold a track of up to 12,500 cell words: 500
// kbit/s at 300 RPM. Track data (one track's sectors) up to 12 KB.
constexpr uint32_t MAX_TRACK_WORDS = 12500;
constexpr uint32_t MAX_TRACK_DATA = 12288;
constexpr unsigned MAX_CYLINDERS = 255;  // FlashFloppy's limit: Ensoniq's SuperDisk
constexpr unsigned MAX_KINDS = 8;        // distinct track layouts in one disk
constexpr unsigned MAX_SECTORS = 256;

// One track's layout.
struct Track {
    uint16_t sectors = 0;
    uint8_t size_code = 2;  // sector bytes = 128 << size_code
    uint8_t first_id = 1;   // R of the first sector (in ID order)
    int16_t head_id = -1;   // H in each ID field; -1: the physical head
    uint8_t interleave = 1;
    uint8_t cskew = 0;  // rotational slots the first sector moves per cylinder
    uint8_t hskew = 0;  // ...and per head
    bool iam = true;    // an index mark (and gap 1) after gap 4a
    uint8_t gap1 = 50;
    uint8_t gap2 = 22;
    uint8_t gap3 = 84;
    uint8_t gap4a = 80;
    bool fm = false;    // FM, not MFM
    uint8_t scale = 1;  // the disk's rate over this track's: 1 or 2

    constexpr uint32_t sectorBytes() const { return 128u << size_code; }
    constexpr uint32_t dataBytes() const { return sectors * sectorBytes(); }
    // Zeros before each field's sync (or, in FM, its mark), and the sync
    // and mark bytes: 12 and 4 in MFM, 6 and 1 in FM.
    constexpr uint32_t presyncBytes() const { return fm ? 6 : 12; }
    constexpr uint32_t syncBytes() const { return fm ? 1 : 4; }
    // The words (at the disk's rate) the layout uses before gap 4b.
    uint32_t usedBytes() const;
    // Where things are, in words from the index: the start of the zeros
    // before the ID field and the data field at rotational `slot`. Their
    // syncs follow presyncBytes() x scale words later.
    uint32_t idFieldStart(uint16_t slot) const;
    uint32_t dataFieldStart(uint16_t slot) const;
    // Which sector (from 0, in ID order) each rotational slot holds on
    // (cylinder, head): Greaseweazle's sec_map. `out` holds `sectors` entries.
    void sectorMap(uint8_t cylinder, uint8_t head, uint16_t* out) const;
};

// How the image file orders its tracks (FlashFloppy's file-layout), flags:
// by default cylinder by cylinder, head 0 then head 1.
enum FileLayout : uint8_t {
    LAYOUT_SEQUENTIAL = 1,     // all of head 0's tracks, then head 1's
    LAYOUT_SIDES_SWAPPED = 2,  // head 1 first
    LAYOUT_REVERSE_SIDE0 = 4,  // head 0's cylinders last to first
    LAYOUT_REVERSE_SIDE1 = 8,
};

// How a track's sectors are laid out and encoded.
enum Encoding : uint8_t {
    ENCODING_IBM = 0,  // IBM MFM: ID and data fields with gaps (this file)
    ENCODING_AMIGA,    // AmigaDOS: a whole track of sectors (amiga_track.h)
};

struct Format {
    uint8_t encoding = ENCODING_IBM;
    uint8_t cyls = 80;
    uint8_t heads = 2;
    uint8_t step = 1;  // head steps per cylinder: 2 for a 40-track disk double-stepped
    uint8_t file_layout = 0;
    uint16_t rpm = 300;
    uint32_t rate = 500000;        // data bits per second
    uint16_t track_bytes = 12500;  // cell words a revolution: rate x 60 / rpm / 8
    // A layout a few percent too long for a revolution (an 8-inch FM boot
    // track with wide gaps) gets a longer track, and the revolution takes
    // that long: up to STRETCH_PCT more. 0: the rpm's.
    Track kinds[MAX_KINDS] = {};
    uint8_t kind_count = 1;
    uint8_t kind_of[MAX_CYLINDERS][2] = {};  // each track's entry in kinds: 0 unless overridden

    const Track& track(uint8_t cylinder, uint8_t head) const {
        return kinds[cylinder < cyls && head < 2 ? kind_of[cylinder][head] : 0];
    }
    uint32_t trackDataBytes(uint8_t cylinder, uint8_t head) const { return track(cylinder, head).dataBytes(); }
    uint32_t dataRate() const { return rate; }
    uint32_t stretched_period_us = 0;
    uint32_t periodUs() const { return stretched_period_us ? stretched_period_us : 60000000u / rpm; }
    bool uniform() const { return kind_count == 1; }

    // The image cylinder under the head at `physical` (the drive's head
    // position), or -1 where the disk has none (between double steps, or
    // beyond its cylinders).
    int imageCylinder(unsigned physical) const {
        if (physical % step) return -1;
        unsigned c = physical / step;
        return c < cyls ? int(c) : -1;
    }
    // The last cylinder the head reaches on this disk.
    unsigned lastPhysicalCylinder() const { return unsigned(cyls) * step - 1; }

    // The image file: where each track's data starts, and its size.
    uint32_t trackOffset(uint8_t cylinder, uint8_t head) const;
    uint32_t imageBytes() const;
    uint32_t maxTrackDataBytes() const;

    // Every track's layout, whole and fitting a revolution. Sets track_bytes
    // from rate and rpm first.
    bool finish();

    // The first track's layout, for a uniform format (tests and the
    // generated disk).
    uint32_t trackDataBytes() const { return kinds[0].dataBytes(); }
    uint32_t sectorBytes() const { return kinds[0].sectorBytes(); }
};

// How much longer than a revolution a track may be: Greaseweazle warns
// beyond this, and a host's controller tolerates it.
constexpr uint32_t STRETCH_PCT = 5;

// Words in a revolution at `rate` bit/s and `rpm`.
constexpr uint16_t trackWords(uint32_t rate, uint32_t rpm) { return uint16_t(uint64_t(rate) * 15 / (2 * rpm)); }

// A track's parameters as written (IMG.CFG, Greaseweazle's diskdefs): -1
// for auto.
struct TrackSpec {
    int sectors = 0;
    int size_code = -1;  // required if sectors > 0
    int first_id = 1;
    int head_id = -1;
    int interleave = 1, cskew = 0, hskew = 0;
    bool iam = true;
    int gap1 = -1, gap2 = -1, gap3 = -1, gap4a = -1;
    bool fm = false;
    int rate = 0;  // this track's data rate in bit/s; 0: auto
};

// Greaseweazle's rules for the auto data rate: the lowest of 250, 500 and
// 1000 kbit/s (FM: 125 and 250) whose revolution holds the layout (with 3%
// to spare).
uint32_t autoRate(const TrackSpec& spec, uint32_t rpm);

// Resolves the gaps left on auto, as Greaseweazle does (IBMTrack_Fixed),
// for a track at `rate` (its own): gap 3 as big as fits, up to the size's
// standard; then gap 4a halved if a 1% pre-index gap wouldn't fit. False if
// the layout can't fit at all. The track's scale is the caller's to set.
bool resolve(const TrackSpec& spec, uint32_t rate, uint32_t rpm, Track& out);

// A whole disk of one layout: cyls x heads tracks of `spec`.
bool uniformFormat(uint8_t cyls, uint8_t heads, const TrackSpec& spec, uint32_t rate, uint32_t rpm, Format& out);

// A disk of one track layout with every gap given: a constant.
constexpr Format fixedFormat(uint8_t cyls, uint8_t heads, const Track& t, uint32_t rate, uint16_t rpm) {
    Format f{};
    f.cyls = cyls;
    f.heads = heads;
    f.rate = rate;
    f.rpm = rpm;
    f.track_bytes = trackWords(rate, rpm);
    f.kinds[0] = t;
    return f;
}

constexpr Track pcTrack(uint16_t sectors, uint8_t gap3) {
    Track t{};
    t.sectors = sectors;
    t.gap3 = gap3;
    return t;
}

// The PC formats, as Greaseweazle defines them (ibm.1440 and ibm.720:
// gap3 108 and 84). DOS FORMAT writes gap3 84 on 1.44 MB disks; any gap3
// reads the same.
inline constexpr Format HD_1440 = fixedFormat(80, 2, pcTrack(18, 108), 500000, 300);
inline constexpr Format DD_720 = fixedFormat(80, 2, pcTrack(9, 84), 250000, 300);

// An Amiga ADF: 80 cylinders, 2 heads, 11 AmigaDOS sectors a track, at
// 250 kbit/s (a DD drive's 2 us cells; Paula's are 1.97 us).
Format amigaDD();

// The format of a raw PC image of `bytes`, by its size alone, as
// Greaseweazle's ibm.* definitions: 160, 180, 320, 360 KB (40 cylinders),
// 720, 800 KB, 1.2 MB (360 RPM), 1.44 MB, 1.68 MB (DMF). False otherwise
// (2.88 MB's 1 Mbit/s track is longer than the streamer holds).
bool formatForSize(uint32_t bytes, Format& out);

constexpr uint8_t MARK_IAM = 0xFC;
constexpr uint8_t MARK_IDAM = 0xFE;
constexpr uint8_t MARK_DAM = 0xFB;
constexpr uint8_t GAP_BYTE = 0x4E;
constexpr unsigned PRESYNC_BYTES = 12;

// Builds one track's cells into `out` (fmt.track_bytes words, one per byte).
// `cylinder` is the image's; `data` holds the track's sectors in ID order,
// fmt.trackDataBytes(cylinder, head) bytes: the same layout as that track's
// slice of an IMG file. Returns false if the layout doesn't fit the track.
bool buildTrack(const Format& fmt, uint8_t cylinder, uint8_t head, const uint8_t* data, uint16_t* out);

// An unformatted-looking track: gap bytes all the way round, no index mark
// and no sectors. What the drive sends where the image has no track
// (beyond its cylinders, a missing head, between double steps): the host
// finds no sectors there, as on a real disk.
void buildBlankTrack(const Format& fmt, uint16_t* out);

}  // namespace ibm
