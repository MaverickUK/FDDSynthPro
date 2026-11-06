#include "amiga_pio.h"

#include <hardware/gpio.h>
#include <hardware/pio.h>

#include <initializer_list>

#include "config.h"
#include "fdd_edge.pio.h"
#include "fdd_step.pio.h"
#include "fdd_gate.pio.h"
#include "fdd_gate_od.pio.h"
#include "fdd_signals.h"

namespace amiga_pio {

// The lines the gate drives: every output but RDATA (the streamer's).
static constexpr unsigned GATE_PINS[] = {cfg::PIN_INDEX, cfg::PIN_TRK00, cfg::PIN_WPT, cfg::PIN_DSKCHG, cfg::PIN_CHNG};
static constexpr unsigned GATE_BASE = cfg::PIN_INDEX;
static constexpr unsigned GATE_COUNT = cfg::PIN_CHNG - cfg::PIN_INDEX + 1;  // OUT pins GPIO 4-23
static_assert(GATE_BASE == cfg::BUS_FIRST_PIN, "the gate's bit 0 is sample layout's");
static_assert(cfg::PIN_RDATA > GATE_BASE && cfg::PIN_RDATA < GATE_BASE + GATE_COUNT,
              "RDATA is inside the gate's OUT range: the gate must be in another PIO block");

static PIO s_edge_pio, s_step_pio, s_gate_pio;
static uint s_step_sm, s_select_sm, s_gate_sm;
static uint32_t s_levels = 0xFFFFFFFFu;  // what the gate was last sent
static bool s_running = false;

static uint32_t gateMask() {
    uint32_t m = 0;
    for (unsigned pin : GATE_PINS) m |= 1u << pin;
    return m;
}

// A PIO block in the GPIO 0-31 window with `sms` free state machines and room
// for `program`, other than `not_this` (nullptr: any). PIO 0 is the libraries'.
static bool claim(const pio_program_t* program, unsigned sms, PIO not_this, PIO& pio, uint* sm, uint& offset) {
    for (PIO p : {pio1, pio2}) {
        if (p == not_this || pio_get_gpio_base(p) != 0 || !pio_can_add_program(p, program)) continue;
        int got[2] = {-1, -1};
        unsigned n = 0;
        for (; n < sms; ++n) {
            got[n] = pio_claim_unused_sm(p, false);
            if (got[n] < 0) break;
        }
        if (n == sms) {
            pio = p;
            for (unsigned i = 0; i < sms; ++i) sm[i] = uint(got[i]);
            offset = pio_add_program(p, program);
            return true;
        }
        for (unsigned i = 0; i < n; ++i) pio_sm_unclaim(p, uint(got[i]));
    }
    return false;
}

// Either sampler: IN from the first input, the edge's pin as the jmp pin.
static void startSampler(PIO pio, uint sm, uint offset, pio_sm_config c, unsigned pin) {
    sm_config_set_in_pins(&c, cfg::FIRST_INPUT_PIN);
    sm_config_set_in_shift(&c, false, false, 32);  // shift left: GPIO 12 in bit 0 of the ten
    sm_config_set_jmp_pin(&c, pin);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    sm_config_set_clkdiv_int_frac8(&c, 1, 0);
    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_enabled(pio, sm, true);
}

// The PIO block whose program drives RDATA: the gate must be in another.
static PIO rdataPio() {
    switch (gpio_get_function(cfg::PIN_RDATA)) {
        case GPIO_FUNC_PIO1: return pio1;
        case GPIO_FUNC_PIO2: return pio2;
        default: return nullptr;
    }
}

static uint32_t gateWord(uint32_t levels) {
    // Open drain: 1 = output = asserted; else the levels themselves.
    return cfg::OUTPUTS_OPEN_DRAIN ? (~levels & fdd::OUTPUT_MASK) : levels;
}

bool begin(unsigned select_pin) {
    uint edge_offset, step_offset, gate_offset;
    const pio_program_t* gate = cfg::OUTPUTS_OPEN_DRAIN ? &fdd_gate_od_program : &fdd_gate_program;
    if (!claim(&fdd_edge_program, 1, nullptr, s_edge_pio, &s_select_sm, edge_offset)) return false;
    if (!claim(&fdd_step_program, 1, nullptr, s_step_pio, &s_step_sm, step_offset)) {
        pio_sm_unclaim(s_edge_pio, s_select_sm);
        pio_remove_program(s_edge_pio, &fdd_edge_program, edge_offset);
        return false;
    }
    if (!claim(gate, 1, rdataPio(), s_gate_pio, &s_gate_sm, gate_offset)) {
        pio_sm_unclaim(s_edge_pio, s_select_sm);
        pio_remove_program(s_edge_pio, &fdd_edge_program, edge_offset);
        pio_sm_unclaim(s_step_pio, s_step_sm);
        pio_remove_program(s_step_pio, &fdd_step_program, step_offset);
        return false;
    }

    startSampler(s_step_pio, s_step_sm, step_offset, fdd_step_program_get_default_config(step_offset), cfg::PIN_STEP);
    startSampler(s_edge_pio, s_select_sm, edge_offset, fdd_edge_program_get_default_config(edge_offset), select_pin);

    // The gate: every line released before the pins are its.
    const uint32_t mask = gateMask();
    if (cfg::OUTPUTS_OPEN_DRAIN) {
        pio_sm_set_pins_with_mask(s_gate_pio, s_gate_sm, 0, mask);  // the level stays low
        pio_sm_set_pindirs_with_mask(s_gate_pio, s_gate_sm, 0, mask);
    } else {
        pio_sm_set_pins_with_mask(s_gate_pio, s_gate_sm, mask, mask);
        pio_sm_set_pindirs_with_mask(s_gate_pio, s_gate_sm, mask, mask);
    }
    pio_sm_config c = (cfg::OUTPUTS_OPEN_DRAIN ? fdd_gate_od_program_get_default_config
                                               : fdd_gate_program_get_default_config)(gate_offset);
    sm_config_set_out_pins(&c, GATE_BASE, GATE_COUNT);
    sm_config_set_jmp_pin(&c, select_pin);
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv_int_frac8(&c, 1, 0);
    pio_sm_init(s_gate_pio, s_gate_sm, gate_offset, &c);
    // X starts as the first levels (all released): `pull noblock` falls back
    // on X while the FIFO is empty.
    s_levels = fdd::OUTPUT_MASK;
    pio_sm_put(s_gate_pio, s_gate_sm, gateWord(s_levels));
    pio_sm_set_enabled(s_gate_pio, s_gate_sm, true);
    for (unsigned pin : GATE_PINS) pio_gpio_init(s_gate_pio, pin);  // the cable's from here
    s_running = true;
    return true;
}

bool takeStepSample(uint32_t& pins) {
    if (!s_running || pio_sm_is_rx_fifo_empty(s_step_pio, s_step_sm)) return false;
    pins = stepPins(pio_sm_get(s_step_pio, s_step_sm));
    return true;
}

bool takeSelectSample(uint32_t& pins) {
    if (!s_running || pio_sm_is_rx_fifo_empty(s_edge_pio, s_select_sm)) return false;
    pins = samplePins(pio_sm_get(s_edge_pio, s_select_sm));
    return true;
}

void setLevels(uint32_t levels) {
    levels &= fdd::OUTPUT_MASK;
    if (!s_running || levels == s_levels || pio_sm_is_tx_fifo_full(s_gate_pio, s_gate_sm)) return;
    pio_sm_put(s_gate_pio, s_gate_sm, gateWord(levels));
    s_levels = levels;
}

}  // namespace amiga_pio
