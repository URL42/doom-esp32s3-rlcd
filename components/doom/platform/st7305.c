//
// ST7305 driver. See st7305.h for the packing description and provenance.
//

#include "st7305.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "st7305";

// Pins are fixed by the Waveshare schematic; see CLAUDE.md. These match
// Waveshare's own user_config.h exactly.
#define PIN_CS     40
#define PIN_RESET  41
#define PIN_SCL    11
#define PIN_SDA    12
#define PIN_DC      5
#define PIN_TE      6

// 24 MHz is what Waveshare's ESP-IDF demo uses for this panel.
#define ST7305_SPI_HZ (24 * 1000 * 1000)

#define ST7305_HOST SPI2_HOST

// Address window covering the whole panel. The controller's column addresses
// span 12 pixels each and its row addresses 2 lines each, so these cover the
// full 300x400 native area; RAMWR then auto-increments through it.
#define CASET_START 0x00
#define CASET_END   0x2A
#define RASET_START 0x00
#define RASET_END   0xC7

#define CMD_CASET 0x2A
#define CMD_RASET 0x2B
#define CMD_RAMWR 0x2C

static spi_device_handle_t s_spi;
static bool s_ready;

// CS is active low and must stay asserted for an entire command+data sequence.
static inline void cs_select(void)   { gpio_set_level(PIN_CS, 0); }
static inline void cs_release(void)  { gpio_set_level(PIN_CS, 1); }

static esp_err_t spi_tx(const uint8_t *data, size_t len, bool is_data)
{
    if (len == 0) {
        return ESP_OK;
    }
    gpio_set_level(PIN_DC, is_data ? 1 : 0);

    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    return spi_device_polling_transmit(s_spi, &t);
}

static esp_err_t wr_cmd(uint8_t cmd)
{
    return spi_tx(&cmd, 1, false);
}

static esp_err_t wr_args(const uint8_t *args, size_t n)
{
    return spi_tx(args, n, true);
}

// Convenience: command followed by n argument bytes.
static esp_err_t wr(uint8_t cmd, const uint8_t *args, size_t n)
{
    esp_err_t e = wr_cmd(cmd);
    if (e == ESP_OK && n) {
        e = wr_args(args, n);
    }
    return e;
}

//
// Initialisation sequence for this panel.
//
// Originally transcribed wholesale from Waveshare's u8g2-based demo, which was
// a mistake: u8g2 writes the panel tile by tile, setting RASET per sub-row and
// expanding 3 bytes into 6 as it goes. We instead dump all 15000 bytes flat
// after a single window set, the way ngttai/esp_lcd_st7305 does. Mixing one
// driver's init with another's write strategy produced coherent-but-wrong
// output on the glass.
//
// Where the two disagree, the values below now follow the esp_lcd driver, since
// that is whose write path we use. The orientation-critical registers (0x36,
// 0x3A, 0xB0, 0xB8, 0xB9) are identical in both, which is why geometry looked
// approximately right while tone did not.
//
static esp_err_t panel_init_sequence(void)
{
    esp_err_t e;

    cs_select();

    #define W(cmd, ...)  do { \
        const uint8_t _a[] = { __VA_ARGS__ }; \
        e = wr((cmd), _a, sizeof(_a)); \
        if (e != ESP_OK) { cs_release(); return e; } \
    } while (0)
    #define C(cmd) do { e = wr_cmd(cmd); if (e != ESP_OK) { cs_release(); return e; } } while (0)

    C(0x01);                                   // software reset
    vTaskDelay(pdMS_TO_TICKS(100));

    W(0xD6, 0x13, 0x02);                       // NVM load control
    W(0xD1, 0x01);                             // booster enable
    W(0xC0, 0x11, 0x04);                       // gate voltage (reference driver's values)
    W(0xC1, 0x3C, 0x3E, 0x3C, 0x3C);           // VSHP 1..4 = 4.8V
    W(0xC2, 0x23, 0x21, 0x23, 0x23);           // VSLP 1..4 = 0.98V
    W(0xC4, 0x5A, 0x5C, 0x5A, 0x5A);           // VSHN 1..4 = -3.6V
    W(0xC5, 0x37, 0x35, 0x37, 0x37);           // VSLN 1..4 = 0.22V

    W(0xD8, 0xA6, 0xE9);                       // OSC setting
    W(0xB2, 0x12);                             // frame rate: HPM 32Hz, LPM 1Hz

    // Update period gate EQ control, high-power mode
    W(0xB3, 0xE5, 0xF6, 0x17, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x71);
    // ...and low-power mode
    W(0xB4, 0x05, 0x46, 0x77, 0x77, 0x77, 0x77, 0x77, 0x76, 0x45);

    W(0x62, 0x32, 0x03, 0x1F);                 // gate timing control
    W(0xB7, 0x13);                             // source EQ enable
    W(0xB0, 0x64);                             // duty setting: 400 line

    C(0x11);                                   // sleep out
    vTaskDelay(pdMS_TO_TICKS(10));

    W(0xC9, 0x00);                             // source voltage select
    W(0x36, 0x48);                             // memory data access control
    W(0x3A, 0x11);                             // data format select
    W(0xB9, 0x20);                             // gamma mode: mono
    W(0xB8, 0x29);                             // panel setting: 1-dot inversion

    W(0x35, 0x00);                             // TE setting
    W(0xD0, 0xFF);                             // enable auto power down
    C(0x38);                                   // high power mode on

    C(0x21);                                   // display inversion ON
    C(0x29);                                   // display on
    W(0xBB, 0x4F);                             // enable clear RAM to 0

    #undef W
    #undef C

    cs_release();

    vTaskDelay(pdMS_TO_TICKS(50));
    return ESP_OK;
}

esp_err_t ST7305_Init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_DC) | (1ULL << PIN_RESET) | (1ULL << PIN_CS),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_FALSE(gpio_config(&io) == ESP_OK, ESP_FAIL, TAG, "gpio_config failed");

    // TE is an input from the panel. Not used for timing yet -- see DG_DrawFrame.
    gpio_config_t te = {
        .pin_bit_mask = (1ULL << PIN_TE),
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&te);

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_SDA,
        .miso_io_num = -1,          // write-only panel
        .sclk_io_num = PIN_SCL,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        // A whole frame goes out in one transaction.
        .max_transfer_sz = ST7305_FB_BYTES + 64,
    };
    esp_err_t e = spi_bus_initialize(ST7305_HOST, &bus, SPI_DMA_CH_AUTO);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(e));
        return e;
    }

    spi_device_interface_config_t dev = {
        .clock_speed_hz = ST7305_SPI_HZ,
        .mode = 0,                  // CPOL=0 CPHA=0, per the panel
        // CS is driven by hand, NOT by the peripheral.
        //
        // With hardware CS the driver deasserts it around every individual
        // spi_device_transmit -- including between a command byte and its
        // arguments. The ST7305 requires CS to stay low for a whole
        // command+data sequence and discards anything fragmented that way, so
        // every SPI call returns ESP_OK and the panel stays blank. Waveshare's
        // own driver sets spics_io_num = -1 for exactly this reason.
        .spics_io_num = -1,
        .queue_size = 2,
    };
    e = spi_bus_add_device(ST7305_HOST, &dev, &s_spi);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device: %s", esp_err_to_name(e));
        return e;
    }

    cs_release();   // idle high before the first transfer

    // Hardware reset. Widths from the u8g2 display descriptor (3ms each).
    gpio_set_level(PIN_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    e = panel_init_sequence();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "init sequence failed: %s", esp_err_to_name(e));
        return e;
    }

    s_ready = true;
    ESP_LOGI(TAG, "ST7305 up: %dx%d landscape, 1bpp, %d-byte frame, SPI %d MHz",
             ST7305_W, ST7305_H, ST7305_FB_BYTES, ST7305_SPI_HZ / 1000000);
    return ESP_OK;
}

esp_err_t ST7305_Flush(const uint8_t *packed)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    // Row-addressed, not a flat dump.
    //
    // Dumping all 15000 bytes after a single CASET/RASET assumes the controller
    // auto-increments from the end of one row into the start of the next. It
    // does not, and the result on glass is regular vertical striping -- the
    // image is coherent but smeared across the wrong addresses.
    //
    // u8g2's driver for this exact 300x400 panel re-addresses every row, and
    // that is what this now does: CASET, then RASET with start == end, then
    // RAMWR, for each of the 200 addressable rows.
    //
    // Note CASET starts at 0x01, not 0x00. That is u8g2's value for this panel;
    // starting at zero shifts every row by one column unit.
    //
    // CS stays asserted across the whole frame, as it must.
    static const uint8_t caset[] = { 0x01, 0x2A };

    cs_select();

    esp_err_t e = ESP_OK;
    for (int row = 0; row < ST7305_ROWS && e == ESP_OK; row++) {
        const uint8_t raset[] = { (uint8_t)row, (uint8_t)row };

        e = wr(CMD_CASET, caset, sizeof(caset));
        if (e == ESP_OK) e = wr(CMD_RASET, raset, sizeof(raset));
        if (e == ESP_OK) e = wr_cmd(CMD_RAMWR);
        if (e == ESP_OK) e = spi_tx(packed + (size_t)row * ST7305_ROW_BYTES,
                                    ST7305_ROW_BYTES, true);
    }

    cs_release();
    return e;
}

void ST7305_ClearBuffer(uint8_t *packed)
{
    // 0 bits are background on this panel (inversion is off).
    memset(packed, 0x00, ST7305_FB_BYTES);
}

void ST7305_TestPattern(uint8_t *packed, int hold_ms)
{
    ST7305_ClearBuffer(packed);

    // 1px border around the full panel -- proves the extents and the addressing.
    for (int x = 0; x < ST7305_W; x++) {
        ST7305_SetPixel(packed, x, 0, true);
        ST7305_SetPixel(packed, x, ST7305_H - 1, true);
    }
    for (int y = 0; y < ST7305_H; y++) {
        ST7305_SetPixel(packed, 0, y, true);
        ST7305_SetPixel(packed, ST7305_W - 1, y, true);
    }

    // Solid block in the TOP-LEFT only. Asymmetric on both axes, so a flip or a
    // mirror is obvious rather than ambiguous.
    for (int y = 10; y < 60; y++) {
        for (int x = 10; x < 110; x++) {
            ST7305_SetPixel(packed, x, y, true);
        }
    }

    // Diagonal from top-left to bottom-right: shows shear if the row stride or
    // the 2x4 block packing is wrong.
    for (int i = 0; i < ST7305_H; i++) {
        ST7305_SetPixel(packed, (i * ST7305_W) / ST7305_H, i, true);
    }

    // Vertical grey ramp via 4x4 ordered dither, right half. If the panel is
    // alive but the packing is off, this degenerates into visible banding.
    static const uint8_t bayer[16] = {
          8, 136,  40, 168,
        200,  72, 232, 104,
         56, 184,  24, 152,
        248, 120, 216,  88,
    };
    for (int y = 80; y < ST7305_H - 20; y++) {
        int level = (255 * (y - 80)) / (ST7305_H - 100);
        for (int x = 220; x < ST7305_W - 20; x++) {
            ST7305_SetPixel(packed, x, y, level < bayer[((y & 3) << 2) | (x & 3)]);
        }
    }

    ESP_LOGW(TAG, "TEST PATTERN: border + solid block TOP-LEFT + diagonal + grey ramp right");
    ST7305_Flush(packed);
    vTaskDelay(pdMS_TO_TICKS(hold_ms));
}
