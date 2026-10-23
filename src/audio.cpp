#include "audio.h"

#include <I2S.h>

#include "config.h"

namespace audio {

static I2S s_i2s(OUTPUT);
static synth::Mixer* s_mixer = nullptr;
static uint32_t s_underruns = 0;

void begin(synth::Mixer& mixer, uint32_t rate) {
    s_mixer = &mixer;
    s_i2s.setBCLK(cfg::PIN_I2S_BCLK);  // LRCLK = BCLK + 1
    s_i2s.setDATA(cfg::PIN_I2S_DATA);
    s_i2s.setBitsPerSample(16);
    // One I2S buffer word = one stereo frame (16-bit L + 16-bit R).
    s_i2s.setBuffers(cfg::AUDIO_BUFFERS, cfg::AUDIO_BLOCK_FRAMES);
    s_i2s.begin(int(rate));
}

void setRate(uint32_t rate) {
    if (s_mixer) s_i2s.setFrequency(int(rate));
}

void service() {
    if (!s_mixer) return;
    static int16_t mono[cfg::AUDIO_BLOCK_FRAMES];
    static uint32_t stereo[cfg::AUDIO_BLOCK_FRAMES];
    constexpr size_t block_bytes = sizeof stereo;
    if (s_i2s.getUnderflow()) ++s_underruns;
    while (s_i2s.availableForWrite() >= int(block_bytes)) {
        s_mixer->render(mono, cfg::AUDIO_BLOCK_FRAMES);
        for (size_t n = 0; n < cfg::AUDIO_BLOCK_FRAMES; ++n) {
            // The same sample on both channels, so the MAX98357A plays it
            // whichever channel its SD_MODE selects.
            uint16_t s = uint16_t(mono[n]);
            stereo[n] = (uint32_t(s) << 16) | s;
        }
        s_i2s.write(reinterpret_cast<const uint8_t*>(stereo), block_bytes);
    }
}

bool running() { return s_mixer != nullptr; }
uint32_t underruns() { return s_underruns; }

}  // namespace audio
