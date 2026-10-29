// Disk images on the SD card (the BB48R's slot, SPI), as a TrackSource. Core 0 only.
//
// mount() reads IMG.CFG (img_cfg.h: /IMG.CFG or /FF/IMG.CFG) and lists
// every image in the card's root whose format it knows, from IMG.CFG or its
// size (image_list.h: .img, .ima or .dsk, in name order); open() serves one
// to a drive (bus.h:
// one or two). Each drive has one open at a time: opening another closes
// its last. An image the other drive has open opens read-only, so the two
// never write the same file behind each other's cache.
#pragma once

#include <cstddef>
#include <cstdint>

#include "bus.h"
#include "image_list.h"
#include "synth.h"
#include "track_source.h"

namespace sd_image {

// Mounts the card and lists its images. False if there's no card.
bool mount();
const ImageList& images();

// Opens image `index` of the list for drive `drive`. Returns its source,
// or nullptr if it can't be opened.
TrackSource* open(size_t index, unsigned drive = 0);

// The drive's open image's name, and whether its clusters are contiguous
// (reads are fastest then).
const char* name(unsigned drive = 0);
bool contiguous(unsigned drive = 0);

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
