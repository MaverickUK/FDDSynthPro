#pragma once

// SemVer. Bump in the same commit as any firmware change (see CLAUDE.md).
#define FIRMWARE_VERSION "0.2.0"

// What this build does, set per environment in platformio.ini:
//   0  listen only (slice 0): every pin an input, traces what the host does
//   1  the drive (slice 1): head, index, TRK00, WPT and DSKCHG
#ifndef FIRMWARE_SLICE
#define FIRMWARE_SLICE 0
#endif
