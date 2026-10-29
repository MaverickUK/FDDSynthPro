// Pin map and sizes. Pure constants, so host tests can include it.
// docs/wiring.md maps each pin to the floppy header.
#pragma once

#include <cstddef>
#include <cstdint>

namespace cfg {

// The pin map is for the BB48R, the BB48 with PSRAM and a microSD slot
// (docs/wiring.md). The floppy interface uses GPIO 4-7 and 12-22, all inside
// one PIO block's GPIO 0-31 window. Avoided on purpose:
//   GPIO 0-3  the BB48 pulls GPIO 2/3 up to 3.3 V through 2.2k (R17/R18,
//             UEXT/Qwiic I2C); GPIO 0/1 stay free for a UART if needed.
//   GPIO 8    the PSRAM's chip select (the board's own wiring).
//   GPIO 9-11, 24  the microSD slot (the board's own wiring).
//   GPIO 25   the on-board LED (2.2k + LED to GND).
//
// Every floppy signal is active low. Inputs come from the host's drivers and
// are pulled up to 3.3 V by resistors on our side (the drive end terminates
// the cable: 10k with the FeatherWing host, 1k for a PC). Outputs go through
// an SN7407 open-collector buffer: GPIO low asserts the line, GPIO high
// releases it.

// Inputs (host -> drive), on consecutive pins so one read captures them all.
constexpr unsigned PIN_DENSEL = 12;  // floppy pin 2: density select
constexpr unsigned PIN_MOTEA = 13;   // floppy pin 10: motor enable A
constexpr unsigned PIN_DRVSB = 14;   // floppy pin 12: drive select B (a PC drive's own select)
constexpr unsigned PIN_DRVSA = 15;   // floppy pin 14: drive select A
constexpr unsigned PIN_MOTEB = 16;   // floppy pin 16: motor enable B (a PC drive's own motor)
constexpr unsigned PIN_DIR = 17;     // floppy pin 18: asserted = step in (towards higher cylinders)
constexpr unsigned PIN_STEP = 18;    // floppy pin 20: one step per assertion
constexpr unsigned PIN_WDATA = 19;   // floppy pin 22: write data (slice 5)
constexpr unsigned PIN_WGATE = 20;   // floppy pin 24: write gate
constexpr unsigned PIN_SIDE1 = 21;   // floppy pin 32: asserted = head 1

// Outputs (drive -> host), through the SN7407. Four below the PSRAM and SD
// pins, one after the inputs.
constexpr unsigned PIN_INDEX = 4;    // floppy pin 8: once per revolution
constexpr unsigned PIN_TRK00 = 5;    // floppy pin 26: head on cylinder 0
constexpr unsigned PIN_WPT = 6;      // floppy pin 28: write protected
constexpr unsigned PIN_RDATA = 7;    // floppy pin 30: read data (flux pulses)
constexpr unsigned PIN_DSKCHG = 22;  // floppy pin 34: disk changed
constexpr unsigned OUTPUT_PINS[] = {PIN_INDEX, PIN_TRK00, PIN_WPT, PIN_RDATA, PIN_DSKCHG};

// How the outputs reach the cable, set per build in platformio.ini:
//   0  through the SN7407 (the default): the GPIOs drive its inputs high and
//      low, and it pulls the cable low.
//   1  wired straight to the header, for bring-up with the FeatherWing host
//      before the SN7407 arrives. Each pin works as open drain: its level is
//      always low, and asserting a line enables the output; releasing it
//      makes the pin an input again, and the host's pull-up takes the line
//      to 5 V, which the RP2350's pins tolerate while it is powered. Never
//      with a real PC: its termination can need more current than a GPIO
//      can sink, and nothing would protect the RP2350 from the cable.
#ifndef FDD_OUTPUTS_OPEN_DRAIN
#define FDD_OUTPUTS_OPEN_DRAIN 0
#endif
constexpr bool OUTPUTS_OPEN_DRAIN = FDD_OUTPUTS_OPEN_DRAIN;

constexpr unsigned PIN_BOARD_LED = 25;  // on-board LED, not a floppy line

// The BB48R's own microSD slot and PSRAM, fixed by the board. Only DAT0 and
// DAT3 of the card are wired, so it runs in SPI mode on SPI1 (the variant
// file's SPI1 pins). The board has the card's pull-ups.
constexpr unsigned PIN_PSRAM_CS = 8;  // used by the core, never by us
constexpr unsigned PIN_SD_CS = 9;     // the card's DAT3
constexpr unsigned PIN_SD_SCK = 10;   // CLK
constexpr unsigned PIN_SD_MOSI = 11;  // CMD
constexpr unsigned PIN_SD_MISO = 24;  // DAT0
// 25 MHz, the SD spec's default speed: a 9 KB track in about 3.5 ms. Too
// slow to read tracks as the head moves (the settle time is 15 ms), so the
// drive serves each disk from a copy in PSRAM (image_cache.h), filled
// before the disk goes in: 160 tracks, about 0.6 s.
constexpr uint32_t SD_SPI_MHZ = 25;

// The image cache: one whole image in PSRAM, per drive: 2 MB holds every
// format the streamer can send at 300 RPM but Ensoniq's 5 MB SuperDisk,
// whose tracks are then read from the card. Two drives use 4 of the 8 MB.
constexpr size_t IMAGE_CACHE_BYTES = 2 * 1024 * 1024;

// Audio: a MAX98357A I2S amp, as on HDDSynth_Native. Below GPIO 32, so the
// I2S PIO program works in the same GPIO 0-31 window as RDATA and WDATA.
constexpr unsigned PIN_I2S_DATA = 26;
constexpr unsigned PIN_I2S_BCLK = 27;  // LRCLK is BCLK + 1 = GPIO 28

// The front panel (drive build, panel.h). The OLED goes on the BB48's own
// Qwiic/UEXT I2C connector (I2C1 on GPIO 2 and 3, already pulled up on the
// board); the encoder and the action button on free pins on EXT2, pulled up
// internally. Each switch connects its pin to ground.
constexpr unsigned PIN_OLED_SDA = 2;        // I2C1, the BB48's Qwiic/UEXT
constexpr unsigned PIN_OLED_SCL = 3;
constexpr unsigned PIN_ENC_A = 32;
constexpr unsigned PIN_ENC_B = 33;
constexpr unsigned PIN_ENC_BUTTON = 34;
constexpr unsigned PIN_ACTION_BUTTON = 35;

// An SSD1306 128x64 at the usual address. A frame is 1,032 bytes on the bus:
// 23 ms at 400 kHz, fed to the I2C FIFO a few bytes per pass of core 0's
// loop, so it never holds anything up.
constexpr uint8_t OLED_I2C_ADDR = 0x3C;
constexpr uint32_t OLED_I2C_HZ = 400000;
constexpr uint32_t OLED_FRAME_MS = 100;       // at most 10 frames a second
constexpr uint32_t HEAD_TRACE_SAMPLE_MS = 50; // the head trace's 128 columns: 6.4 s
constexpr bool ENCODER_REVERSED = false;      // flip if clockwise turns the volume down
constexpr uint32_t BUTTON_DEBOUNCE_MS = 20;
constexpr uint32_t BUTTON_LONG_MS = 1000;     // a long action: eject or insert

// The sample pack lives in RAM (FDD sounds are short), in one allocation
// sized to the pack. Up to 160 KB: the stand-in pack is 152 KB; longer
// samples are cut short.
constexpr size_t SAMPLE_POOL_MAX_BYTES = 160 * 1024;

// The WDATA capture ring: 16 KB is 44 ms of writing at 1.44 MB, far longer
// than core 0 goes between chunks of decoding.
constexpr size_t CAPTURE_RING_BYTES = 16 * 1024;
constexpr size_t WAV_HEADER_BYTES = 4096;  // a WAV's data must start within this (HDDSynth_Native's rule)

// Mixing on core 0: blocks of 128 frames (5.8 ms at 22.05 kHz), 8 of them
// queued (46 ms). That covers core 0's longest stall between top-ups: an SD
// track read or a track encode, a few milliseconds each.
constexpr size_t AUDIO_BLOCK_FRAMES = 128;
constexpr size_t AUDIO_BUFFERS = 8;

constexpr unsigned FIRST_INPUT_PIN = PIN_DENSEL;
constexpr unsigned LAST_INPUT_PIN = PIN_SIDE1;
// The span a pin sample covers. It includes the PSRAM and SD pins (8-11),
// which INPUT_MASK and OUTPUT_MASK (fdd_signals.h) leave out.
constexpr unsigned BUS_FIRST_PIN = PIN_INDEX;
constexpr unsigned BUS_LAST_PIN = PIN_DSKCHG;

// Trace ring between core 1 (producer) and core 0 (consumer):
// 4096 events x 16 bytes = 64 KB.
constexpr size_t TRACE_RING_EVENTS = 4096;

// Steps in the same direction merge into one STEP line unless this far apart.
// Hosts step every 3-6 ms in a seek; a gap this long starts a new seek.
constexpr uint32_t STEP_RUN_GAP_US = 50000;

// Core 0 prints a pending line once the trace has been quiet this long:
// longer than STEP_RUN_GAP_US, so a slow seek still prints as one line.
constexpr unsigned TRACE_FLUSH_MS = 60;

}  // namespace cfg
