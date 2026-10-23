// The FDD Synth's sound output: the mixer (synth.h) feeding an I2S amp.
// Core 0 only: begin() there, so the I2S DMA interrupt is serviced on core 0
// and core 1's drive loop is never interrupted.
#pragma once

#include <cstdint>

#include "synth.h"

namespace audio {

// Starts I2S at the pack's rate, playing what `mixer` makes.
void begin(synth::Mixer& mixer, uint32_t rate);

// Tops up the I2S buffers, a block at a time. Call on every pass of core 0's
// loop; the queued blocks (cfg::AUDIO_BUFFERS) cover the time between calls.
void service();

bool running();
// Times the output ran dry: core 0 stalled longer than the queue.
uint32_t underruns();

}  // namespace audio
