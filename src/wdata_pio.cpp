#include "wdata_pio.h"

#include <hardware/clocks.h>
#include <hardware/dma.h>
#include <hardware/pio.h>

#include <initializer_list>

#include "config.h"
#include "fdd_wdata.pio.h"
#include "rdata_stream.h"

namespace wdata_pio {

static PIO s_pio;
static uint s_sm;
static uint s_offset;
static int s_dma;
static size_t s_capacity_words;

void begin(uint32_t data_rate_bps) {
    // PIO 2 or 1, as for RDATA: PIO 0 belongs to the SD card's SDIO driver.
    bool ok = false;
    for (PIO p : {pio2, pio1}) {
        int sm = pio_claim_unused_sm(p, false);
        if (sm < 0) continue;
        if (pio_get_gpio_base(p) == 0 && pio_can_add_program(p, &fdd_wdata_program)) {
            s_pio = p;
            s_sm = uint(sm);
            s_offset = pio_add_program(p, &fdd_wdata_program);
            ok = true;
            break;
        }
        pio_sm_unclaim(p, uint(sm));
    }
    hard_assert(ok);

    pio_sm_config c = fdd_wdata_program_get_default_config(s_offset);
    sm_config_set_jmp_pin(&c, cfg::PIN_WDATA);  // read only: WDATA stays an SIO input
    sm_config_set_in_shift(&c, true, true, 32);  // first interval in the low byte, autopush
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_RX);
    rdata::Divider div = rdata::clockDivider(clock_get_hz(clk_sys), data_rate_bps,
                                             fdd_wdata_TICKS_PER_CELL * 2);
    sm_config_set_clkdiv_int_frac8(&c, div.integer, div.frac);
    pio_sm_init(s_pio, s_sm, s_offset, &c);

    s_dma = dma_claim_unused_channel(true);
}

void setRate(uint32_t data_rate_bps) {
    rdata::Divider div = rdata::clockDivider(clock_get_hz(clk_sys), data_rate_bps, fdd_wdata_TICKS_PER_CELL * 2);
    pio_sm_set_clkdiv_int_frac8(s_pio, s_sm, div.integer, div.frac);
}

void start(uint8_t* buffer, size_t capacity) {
    s_capacity_words = capacity / 4;
    pio_sm_set_enabled(s_pio, s_sm, false);
    pio_sm_clear_fifos(s_pio, s_sm);
    pio_sm_restart(s_pio, s_sm);
    pio_sm_exec(s_pio, s_sm, pio_encode_jmp(s_offset + fdd_wdata_offset_start));

    dma_channel_config c = dma_channel_get_default_config(s_dma);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(s_pio, s_sm, false));
    dma_channel_configure(s_dma, &c, buffer, &s_pio->rxf[s_sm], s_capacity_words, true);

    pio_sm_set_enabled(s_pio, s_sm, true);
}

size_t stop() {
    pio_sm_set_enabled(s_pio, s_sm, false);
    // Let the DMA take what's left in the FIFO, then stop it.
    while (!pio_sm_is_rx_fifo_empty(s_pio, s_sm) && dma_channel_is_busy(s_dma)) tight_loop_contents();
    uint32_t remaining = dma_channel_hw_addr(s_dma)->transfer_count;
    dma_channel_abort(s_dma);
    return (s_capacity_words - remaining) * 4;
}

uint32_t edgeTicks() { return fdd_wdata_EDGE_TICKS; }
uint32_t ticksPerCell() { return fdd_wdata_TICKS_PER_CELL; }

}  // namespace wdata_pio
