// Disk images on the SD card (the BB48R's slot, SPI), as a TrackSource. Core 0 only.
//
// mount() lists every image in the card's root (image_list.h: .img or .ima,
// 720 KB or 1.44 MB, in name order); open() serves one. Only one is open at
// a time: opening another closes the last.
#pragma once

#include <cstddef>
#include <cstdint>

#include "image_list.h"
#include "synth.h"
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

// The FDD Synth's sample packs: the folders under /samples (mount() lists them).
const PackList& packs();

// Loads pack `index` of packs(): its spinup.wav, spin.wav, spindown.wav,
// step.wav, seek.wav, bump.wav (and optionally insert.wav, eject.wav):
// mono, 16-bit, one rate. The samples go into one allocation sized to the
// pack, up to `max_bytes`; a pack bigger than that is cut short. The last
// pack loaded is freed first, so whatever plays it (synth::Mixer::reset)
// must be told. Returns true if the pack is complete (synth::Pack::complete).
bool loadPack(synth::Pack& pack, size_t index, size_t max_bytes);

// The settings file (settings.h): its text into `out`, up to `cap` bytes;
// 0 if there isn't one. And writing it, replacing the last.
size_t readSettings(char* out, size_t cap);
bool writeSettings(const char* text, size_t len);

}  // namespace sd_image
