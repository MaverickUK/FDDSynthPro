#pragma once

// SemVer. Bump in the same commit as any firmware change (see CLAUDE.md).
#define FIRMWARE_VERSION "0.4.0"

// What this build does, set per environment in platformio.ini:
//   0  listen only (slice 0): every pin an input, traces what the host does
//   3  the drive (slices 1-3): head, index, TRK00, WPT, DSKCHG, and RDATA
//      from an image on the SD card (or the generated 1.44 MB disk)
#ifndef FIRMWARE_SLICE
#define FIRMWARE_SLICE 0
#endif
