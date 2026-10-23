// Disk images on the SD card (SDIO, 4-bit), as a TrackSource. Core 0 only.
//
// For now the drive serves one image per power-on: the first file in the
// card's root, in name order, that ends .img or .ima and has the size of a
// format we serve (720 KB or 1.44 MB). Choosing between images comes later.
#pragma once

#include <cstdint>

#include "track_source.h"

namespace sd_image {

// Mounts the card and opens the image. Returns its source, or nullptr if
// there is no card or no image (the drive then serves the generated disk).
TrackSource* open();

// The image's file name, and whether its clusters are contiguous (reads are
// fastest then). Only meaningful after open() succeeded.
const char* name();
bool contiguous();

}  // namespace sd_image
