// i2s_bridge.c -- the PC's sound card, on the FPGA's I2S pins.
//
//   PC --USB--> output_cb --> spk_stream --> I2S TX --> FPGA i2s_sd_in
//   PC <--USB-- input_cb  <-- mic_stream <-- I2S RX <-- FPGA i2s_sd_out
//
// The P4 is the I2S master (the FPGA samples BCK/WS as slow inputs), so this
// side owns the audio clock.  The host's clock is not that clock: they are two
// oscillators a few ppm apart, which is what the two stream buffers absorb --
// 2048 samples is 43 ms, and the drift over that is sub-sample.
//
// Format: the UAC side is 48 kHz 16-bit mono (what the descriptors announce,
// and the core is mono anyway).  The wire side is Philips I2S with 32-bit
// slots and the audio in the top 24 bits, which is what nam_i2s_if.v reads.
// 16-bit sample -> 24-bit wire is s << 7; putting that in the top of a 32-bit
// slot is s << 15, and the receive side is the same shift back.
//
// The core is a WaveNet with a 6347-sample receptive field: what comes back on
// i2s_sd_out is 132 ms behind what went in.  Fine for processing or listening
// to a track, far too slow to monitor while playing.

#include <string.h>

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "usb_device_uac.h"

static const char *TAG = "uac_i2s";

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
#define BLOCK      256                        // frames per I2S DMA buffer
#define RING_BYTES (2048 * sizeof(int16_t))   // 43 ms each way

static i2s_chan_handle_t tx_chan, rx_chan;

// host -> FPGA and FPGA -> host, both 16-bit mono at RATE_HZ
static StreamBufferHandle_t spk_stream, mic_stream;

static uint32_t spk_starved, mic_short, mic_dropped;

// ---------------------------------------------------------------------------
// UAC side.  Both callbacks run in their own task (the component gives the
// speaker one and the microphone one), so a zero-timeout stream send is all
// this needs -- and it must not block: the microphone side is pulled on a
// timer, not on a demand signal.
// ---------------------------------------------------------------------------
static esp_err_t uac_output_cb(uint8_t *buf, size_t len, void *ctx)
{
    size_t n = xStreamBufferSend(spk_stream, buf, len, 0);
    if (n < len) {
        spk_starved += (len - n) / sizeof(int16_t);
    }
    return ESP_OK;
}

static esp_err_t uac_input_cb(uint8_t *buf, size_t len, size_t *bytes_read, void *ctx)
{
    size_t n = xStreamBufferReceive(mic_stream, buf, len, 0);
    if (n < len) {
        // The host asks for audio whether or not the FPGA has produced any yet
        // (it needs 132 ms of history before its first output).  Send silence
        // rather than a short packet: a short packet is a rate change.
        memset(buf + n, 0, len - n);
        mic_short += (len - n) / sizeof(int16_t);
    }
    *bytes_read = len;
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// I2S side
// ---------------------------------------------------------------------------
static esp_err_t i2s_start(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 8;
    chan_cfg.dma_frame_num = BLOCK;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx_chan, &rx_chan), TAG, "channel");

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
    // The default mclk multiple of 256 puts mclk/bclk at 4, exactly the
    // driver's measured minimum for an internal full-duplex RX slave
    // (i2s_std.c: "RX slave >= 4").  384 gives 6 and costs nothing.
    cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx_chan, &cfg), TAG, "tx init");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx_chan, &cfg), TAG, "rx init");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx_chan), TAG, "tx enable");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx_chan), TAG, "rx enable");
    return ESP_OK;
}

static void i2s_task(void *arg)
{
    // static, not on the stack: these are DMA buffers, and 2 x 4 KB does not
    // belong in a 3.5 KB task stack
    static int16_t spk[BLOCK], mic[BLOCK];
    static int32_t tx_buf[BLOCK * 2], rx_buf[BLOCK * 2];
    uint32_t blocks = 0;

    while (true) {
        size_t n = xStreamBufferReceive(spk_stream, spk, sizeof(spk), 0);
        int have = n / (int)sizeof(int16_t);

        for (int i = 0; i < BLOCK; i++) {
            // silence when the host has nothing to say, so BCK/WS never stop
            int32_t v = (i < have) ? ((int32_t)spk[i] << 15) : 0;
            tx_buf[2 * i]     = v;      // left
            tx_buf[2 * i + 1] = v;      // right: the core reads the left
        }

        size_t written = 0, read = 0;
        if (i2s_channel_write(tx_chan, tx_buf, sizeof(tx_buf), &written,
                              pdMS_TO_TICKS(100)) != ESP_OK) {
            continue;
        }

        // The clock is ours, so the RX side has the same number of frames
        // waiting; a timeout means the FPGA is not driving SD at all.
        if (i2s_channel_read(rx_chan, rx_buf, sizeof(rx_buf), &read,
                             pdMS_TO_TICKS(100)) == ESP_OK) {
            int frames = read / (2 * sizeof(int32_t));
            for (int i = 0; i < frames; i++) {
                mic[i] = (int16_t)(rx_buf[2 * i] >> 15);   // 32-bit slot -> s16
            }
            size_t put = xStreamBufferSend(mic_stream, mic,
                                           frames * sizeof(int16_t), 0);
            // the host pulls at its own rate and there is no feedback for the
            // microphone direction, so this is where drift shows up
            mic_dropped += (frames * sizeof(int16_t) - put) / sizeof(int16_t);
        }

        if (++blocks % ((RATE_HZ / BLOCK) * 5) == 0) {
            ESP_LOGI(TAG, "spk starved %lu  mic short %lu  mic dropped %lu  "
                     "buffered %d/%d",
                     (unsigned long)spk_starved, (unsigned long)mic_short,
                     (unsigned long)mic_dropped,
                     (int)xStreamBufferBytesAvailable(spk_stream),
                     (int)xStreamBufferBytesAvailable(mic_stream));
            spk_starved = 0;
            mic_short = 0;
            mic_dropped = 0;
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "USB UAC -> I2S %d Hz, pins bclk=%d ws=%d dout=%d din=%d",
             RATE_HZ, PIN_BCLK, PIN_WS, PIN_DOUT, PIN_DIN);

    spk_stream = xStreamBufferCreate(RING_BYTES, 1);
    mic_stream = xStreamBufferCreate(RING_BYTES, 1);
    configASSERT(spk_stream && mic_stream);

    ESP_ERROR_CHECK(i2s_start());
    xTaskCreate(i2s_task, "i2s", 4096, NULL, 10, NULL);

    uac_device_config_t cfg = {
        .output_cb = uac_output_cb,
        .input_cb  = uac_input_cb,
    };
    ESP_ERROR_CHECK(uac_device_init(&cfg));
    ESP_LOGI(TAG, "enumerated: speaker + microphone, %d Hz mono", RATE_HZ);
}
