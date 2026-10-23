// Disk images on the SD card (SDIO, 4-bit), as a TrackSource. Core 0 only.
//
// mount() lists every image in the card's root (image_list.h: .img or .ima,
// 720 KB or 1.44 MB, in name order); open() serves one. Only one is open at
// a time: opening another closes the last.
#pragma once

#include <cstddef>
#include <cstdint>

#include "image_list.h"
#include "track_source.h"

namespace sd_image {

// Mounts the card and lists its images. False if there's no card.
bool mount();
const ImageList& images();

// Opens image `index` of the list. Returns its source, or nullptr if it
// can't be opened.
TrackSource* open(size_t index);

// The open image's name, and whether its clusters are contiguous (reads are
// fastest then).
const char* name();
bool contiguous();

}  // namespace sd_image
