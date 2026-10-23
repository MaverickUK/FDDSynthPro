#pragma once

// SemVer. Bump in the same commit as any firmware change (see CLAUDE.md).
#define FIRMWARE_VERSION "0.5.0"

// What this build does, set per environment in platformio.ini:
//   0  listen only (slice 0): every pin an input, traces what the host does
//   4  the drive (slices 1-4): head, index, TRK00, WPT, DSKCHG, RDATA from
//      an image on the SD card (or the generated 1.44 MB disk), and the
//      sound events (ACT lines)
#ifndef FIRMWARE_SLICE
#define FIRMWARE_SLICE 0
#endif
