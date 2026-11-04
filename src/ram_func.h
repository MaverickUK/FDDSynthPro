// RAM_FUNC: a function the board runs from RAM instead of flash.
//
// Both cores run from flash through one 16 KB XIP cache. Core 1's loop
// keeps evicting core 0's code from it, so a long loop on core 0 (encoding a
// track: 12,500 bytes) stalls on flash fetches: measured on the board, the
// encode ran at 2-3x its speed from RAM. The linker script copies
// .time_critical.* into RAM at boot.
//
// On the Mac (the host tests) it's nothing: Mach-O wants other section
// names, and there's no XIP cache to dodge. Keep this header free of SDK
// headers, so the modules that use it stay host-testable.
#pragma once

#if defined(__arm__)
#define RAM_FUNC(name) __attribute__((noinline, section(".time_critical." #name)))
#else
#define RAM_FUNC(name)
#endif

// ALWAYS_INLINE: a small helper that a RAM_FUNC calls in its inner loop.
// Left to itself, GCC keeps such helpers out of line, in flash, and the RAM
// function calls out to them for every byte.
#define ALWAYS_INLINE __attribute__((always_inline)) inline
