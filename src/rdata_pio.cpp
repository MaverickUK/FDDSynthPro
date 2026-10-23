#include "rdata_pio.h"

#include <hardware/clocks.h>
#include <hardware/dma.h>
#include <hardware/gpio.h>
#include <hardware/pio.h>

#include "config.h"
#include "fdd_rdata.pio.h"
#include "rdata_stream.h"

static_assert(fdd_rdata_CELL_CYCLES == rdata::CELL_CYCLES, "fdd_rdata.pio and rdata_stream.h disagree");

namespace rdata_pio {

static PIO s_pio;
static uint s_sm;
static uint s_offset;
static int s_data_dma;  // cylinder -> TX FIFO
static int s_ctrl_dma;  // rewinds the data channel when it finishes
static const uint32_t* s_cylinder;
static uint32_t s_words;
static const uint32_t* s_rewind_addr;  // what the control channel writes: the buffer's start

static void configureData(bool chain) {
    dma_channel_config c = dma_channel_get_default_config(s_data_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(s_pio, s_sm, true));
    // Chaining to itself means "no chain": used while aborting, because an
    // abort can otherwise trigger the chained channel.
    channel_config_set_chain_to(&c, chain ? s_ctrl_dma : s_data_dma);
    dma_channel_configure(s_data_dma, &c, &s_pio->txf[s_sm], s_cylinder, s_words, false);
}

void begin(const uint32_t* cylinder, uint32_t words, uint32_t data_rate_bps) {
    s_cylinder = cylinder;
    s_words = words;
    s_rewind_addr = cylinder;

    bool ok = pio_claim_free_sm_and_add_program_for_gpio_range(&fdd_rdata_program, &s_pio, &s_sm, &s_offset,
                                                                cfg::PIN_SIDE1, cfg::PIN_RDATA - cfg::PIN_SIDE1 + 1,
                                                                true);
    hard_assert(ok);

    // RDATA: released (high) before the state machine owns it.
    pio_sm_set_pins_with_mask(s_pio, s_sm, 1u << cfg::PIN_RDATA, 1u << cfg::PIN_RDATA);
    pio_sm_set_consecutive_pindirs(s_pio, s_sm, cfg::PIN_RDATA, 1, true);
    pio_gpio_init(s_pio, cfg::PIN_RDATA);
    // After pio_gpio_init: setting the function rewrites the pin's control
    // register, overrides included.
    gpio_set_outover(cfg::PIN_RDATA, GPIO_OVERRIDE_HIGH);

    pio_sm_config c = fdd_rdata_program_get_default_config(s_offset);
    sm_config_set_sideset_pins(&c, cfg::PIN_RDATA);
    sm_config_set_jmp_pin(&c, cfg::PIN_SIDE1);  // read only; it stays an SIO input
    sm_config_set_out_shift(&c, false, true, 32);  // top bits first, autopull
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    rdata::Divider div = rdata::clockDivider(clock_get_hz(clk_sys), data_rate_bps);
    sm_config_set_clkdiv_int_frac8(&c, div.integer, div.frac);
    pio_sm_init(s_pio, s_sm, s_offset, &c);

    s_data_dma = dma_claim_unused_channel(true);
    s_ctrl_dma = dma_claim_unused_channel(true);
    dma_channel_config cc = dma_channel_get_default_config(s_ctrl_dma);
    channel_config_set_transfer_data_size(&cc, DMA_SIZE_32);
    channel_config_set_read_increment(&cc, false);
    channel_config_set_write_increment(&cc, false);
    // Writing the read address with trigger restarts the data channel.
    dma_channel_configure(s_ctrl_dma, &cc, &dma_hw->ch[s_data_dma].al3_read_addr_trig, &s_rewind_addr, 1,
                          false);

    restartAtIndex();
}

void restartAtIndex() {
    pio_sm_set_enabled(s_pio, s_sm, false);
    configureData(false);
    dma_channel_abort(s_data_dma);
    dma_channel_abort(s_ctrl_dma);
    pio_sm_clear_fifos(s_pio, s_sm);
    pio_sm_restart(s_pio, s_sm);  // empties the OSR, so the first OUT pulls
    pio_sm_exec(s_pio, s_sm, pio_encode_jmp(s_offset));
    configureData(true);
    dma_channel_start(s_data_dma);
    pio_sm_set_enabled(s_pio, s_sm, true);
}

void enable(bool on) { gpio_set_outover(cfg::PIN_RDATA, on ? GPIO_OVERRIDE_NORMAL : GPIO_OVERRIDE_HIGH); }

}  // namespace rdata_pio
