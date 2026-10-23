#pragma once

// SemVer. Bump in the same commit as any firmware change (see CLAUDE.md).
#define FIRMWARE_VERSION "0.3.0"

// What this build does, set per environment in platformio.ini:
//   0  listen only (slice 0): every pin an input, traces what the host does
//   2  the drive (slices 1-2): head, index, TRK00, WPT, DSKCHG, and RDATA
//      from a generated 1.44 MB disk
#ifndef FIRMWARE_SLICE
#define FIRMWARE_SLICE 0
#endif
