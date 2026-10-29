// FlashFloppy's IMG.CFG: the GoTek community's way of describing a raw
// image's geometry, for the many machines whose disks aren't PC ones (Akai,
// Roland, Ensoniq, Kaypro...). Pure C++, host-tested against FlashFloppy's
// own example files. The drive reads /IMG.CFG, or /FF/IMG.CFG, from the card.
//
//   [tag::size]          a section: for images named *.tag.img (or .ima,
//   cyls = 80            .dsk), of exactly `size` bytes. Either may be left
//   heads = 2            out: [::737280] matches by size, [akai] by name.
//   secs = 9             An empty tag matches untagged images, and images
//   bps = 512            whose tag no section has.
//   interleave = 2
//   tracks = 0-39.1      what follows applies to these tracks only (cylinder,
//     id = 10            or a range, and .0 or .1 for one head)
//
// The first section that matches wins. Disk parameters: cyls, heads, step,
// file-layout. Track parameters: secs, bps, id, h, interleave, cskew,
// hskew, gap2, gap3, gap4a, iam, mode, rate, rpm, img_bps; gaps and the
// rate may be `auto`, resolved as Greaseweazle does (ibm::resolve).
//
// Not yet: FM recording (mode = fm), mixed sector sizes on one track (a
// bps list), img_bps padding, and a rate or rpm that differs between
// tracks. A section that needs one is refused (Unsupported), and the image
// falls back to its size.
#pragma once

#include <cstddef>
#include <cstdint>

#include "ibm_track.h"

namespace img_cfg {

enum class Result : uint8_t {
    NoMatch,      // no section for this image
    Ok,           // `out` holds its format
    Unsupported,  // the section needs something the drive can't do yet
    Bad,          // the section is malformed, or its layout doesn't fit a track
};

struct Match {
    Result result = Result::NoMatch;
    const char* why = "";  // for Unsupported and Bad: what, for the console
    unsigned line = 0;     // ...and where (from 1)
};

// The image's tag: "akai" for "SAMPLES.akai.img"; "" for "GAMES.IMG".
// Writes it lower-case into `out` (truncated to `cap` - 1).
void imageTag(const char* name, char* out, size_t cap);

// Finds the section of `text` for image `name` of `size` bytes, and
// builds its format into `out`.
Match find(const char* text, size_t len, const char* name, uint32_t size, ibm::Format& out);

}  // namespace img_cfg
