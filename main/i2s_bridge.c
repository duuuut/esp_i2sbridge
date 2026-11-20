// i2s_bridge.c -- ESP32-P4 as the I2S master the NAM3 board expects.
//
// The FPGA is the slave (nam_i2s_if samples BCK/WS as slow inputs), so this
// side owns the clocks.  One controller, two handles: I2S full duplex, so the
// same BCK/WS drive both directions and there is nothing to drift.
//
//   BCK/WS/SD  -> FPGA i2s_sck / i2s_ws / i2s_sd_in
//   SD         <- FPGA i2s_sd_out
//
// Format read off nam_i2s_if.v: Philips I2S, MSB first, one BCK of delay after
// each WS edge, left channel = the sample, 24 bits used.  We send 32-bit slots
// (48 kHz * 64 = 3.072 MHz BCK, well under the core's clk/4 limit) with the
// audio in the top 24 bits: the FPGA takes the first 24 after the delay bit and
// ignores the rest, so the low 8 bits are don't-care padding and the RX side
// shifts them off again.
//
// A 997 Hz sine goes in, so what comes out is a tone the model has shaped --
// no signal generator, no codec, no audio cable.  Once a second it prints what
// it hears: a flat 0 output means the core is not producing, a reading pinned
// at +/-0x3fffff means it is railing.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "i2s_bridge";

// ---------------------------------------------------------------------------
// Wiring.  These four are placeholders: check them against the WT9932P4-TINY
// pinout and the four pins assigned to the FPGA in fpga/vivado/nam3/board.xdc
// (D17 E17 F18 F21 on the AD/DA header).  Nothing else on the board uses them.
// ---------------------------------------------------------------------------
#define PIN_BCLK 20   // -> FPGA i2s_sck   (D17)
#define PIN_WS   21   // -> FPGA i2s_ws    (E17)
#define PIN_DOUT 22   // -> FPGA i2s_sd_in (F18)
#define PIN_DIN  23   // <- FPGA i2s_sd_out (F21)

#define RATE_HZ    48000
#define TONE_HZ    997          // not a multiple of the frame rate: no bin lock
#define AMP_24BIT  0x400000     // 2^22, i.e. half of 24-bit full scale
#define BLOCK      256          // frames per DMA buffer
#define STATS_SEC  1

static i2s_chan_handle_t tx_chan, rx_chan;

// 32-bit phase accumulator DDS: exact frequency, no accumulated rounding, and
// the sine table is the only thing that costs memory.
static int32_t sine24(uint32_t phase)
{
    // sin() takes the phase as 0..2^32 -> 0..2*pi.  Computed once per sample
    // per channel; the P4 has an FPU and this is 96k calls a second.
    double a = (double)phase * (2.0 * M_PI / 4294967296.0);
    return (int32_t)lrint(sin(a) * (double)AMP_24BIT);
}

static esp_err_t i2s_start(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = BLOCK;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_chan, &rx_chan));

    // 32-bit slots with the audio in the top 24 bits: the FPGA reads 24 bits
    // MSB-first and ignores the padding, and we never have to reason about
    // IDF's 24-in-32 packing.
    i2s_std_config_t cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,          // the FPGA does not want one
            .bclk = PIN_BCLK,
            .ws   = PIN_WS,
            .dout = PIN_DOUT,
            .din  = PIN_DIN,
            .invert_flags = { false, false, false },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_chan, &cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_chan));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_chan));
    return ESP_OK;
}

void app_main(void)
{
    ESP_LOGI(TAG, "I2S master %d Hz, BCK %d Hz, pins bclk=%d ws=%d dout=%d din=%d",
             RATE_HZ, RATE_HZ * 64, PIN_BCLK, PIN_WS, PIN_DOUT, PIN_DIN);
    i2s_start();

    // static, not on the stack: 2 x 4 KB of DMA buffers against a 3.5 KB main
    // task stack is a guaranteed overflow, and DMA needs internal RAM anyway
    static int32_t tx_buf[BLOCK * 2], rx_buf[BLOCK * 2];
    uint32_t phase = 0, dphase = (uint32_t)((double)TONE_HZ * 4294967296.0 / RATE_HZ);

    uint32_t sent = 0, got = 0;
    int32_t peak = 0;
    int64_t sumsq = 0;
    int ring = 0;

    while (true) {
        for (int i = 0; i < BLOCK; i++) {
            int32_t v = sine24(phase) << 8;     // 24 bits -> top of the 32-bit slot
            phase += dphase;
            tx_buf[2 * i]     = v;              // left
            tx_buf[2 * i + 1] = v;              // right: the core takes the left
        }

        size_t written = 0, read = 0;
        esp_err_t e = i2s_channel_write(tx_chan, tx_buf, sizeof(tx_buf),
                                        &written, pdMS_TO_TICKS(100));
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "tx: %s", esp_err_to_name(e));
            continue;
        }
        sent += written / (2 * sizeof(int32_t));

        // The clock is ours, so the RX side has the same number of frames
        // waiting; a timeout here means the FPGA is not driving SD at all.
        e = i2s_channel_read(rx_chan, rx_buf, sizeof(rx_buf), &read, pdMS_TO_TICKS(100));
        if (e == ESP_OK) {
            for (int i = 0; i < (int)(read / (2 * sizeof(int32_t))); i++) {
                int32_t w = rx_buf[2 * i] >> 8;         // back to the 24-bit wire word
                int32_t m = w < 0 ? -w : w;
                if (m > peak) peak = m;
                sumsq += (int64_t)w * w;
                got++;
            }
            ring++;
        }

        if (ring >= RATE_HZ / BLOCK * STATS_SEC) {
            double rms = got ? sqrt((double)sumsq / (double)got) : 0.0;
            ESP_LOGI(TAG, "sent %lu  got %lu  rx peak %ld (%.1f%% FS)  rms %.0f",
                     (unsigned long)sent, (unsigned long)got, (long)peak,
                     100.0f * (float)peak / 8388608.0f, (float)rms);
            peak = 0;
            sumsq = 0;
            got = 0;
            ring = 0;
        }
    }
}
