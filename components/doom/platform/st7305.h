//
// ST7305 reflective-LCD driver for the Waveshare ESP32-S3-RLCD-4.2.
//
// The panel is 300x400 native portrait, monochrome (1 bit per pixel), driven
// here rotated to 400x300 landscape. There is no backlight and no colour.
//
// Every register value in the init sequence is transcribed from Waveshare's own
// demo source for this board (waveshareteam/ESP32-S3-RLCD-4.2), cross-checked
// against upstream u8g2's ST7305 driver. None of it is written from memory.
//

#ifndef ST7305_H
#define ST7305_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// Panel geometry in the landscape orientation we drive it in.
#define ST7305_W 400
#define ST7305_H 300

// The controller packs a 2(x) x 4(y) block of pixels into each byte, and the
// buffer is column-major over those blocks:
//
//     index = (x / 2) * (ST7305_H / 4) + (y / 4)
//     bit   = 7 - ((y % 4) * 2 + (x % 2))
//
// which comes to (400/2) * (300/4) = 200 * 75 = 15000 bytes for a full frame --
// exactly 400*300/8, as it must be for 1bpp.
#define ST7305_BLOCKS_Y   (ST7305_H / 4)                 // 75
#define ST7305_FB_BYTES   ((ST7305_W / 2) * ST7305_BLOCKS_Y)  // 15000

// The same buffer viewed the way the controller addresses it: in the panel's
// native portrait orientation it is row-major, 200 addressable rows of 75 bytes.
// index = (y_portrait / 2) * 75 + (x_portrait / 4), which is exactly what the
// landscape formula above collapses to after rotation.
#define ST7305_ROW_BYTES  ST7305_BLOCKS_Y                // 75
#define ST7305_ROWS       (ST7305_W / 2)                 // 200

// Bring up SPI and initialise the panel. Safe to call once.
esp_err_t ST7305_Init(void);

// Push a full 15000-byte framebuffer in the packed format described above.
// Blocks until the transfer completes.
esp_err_t ST7305_Flush(const uint8_t *packed);

// Set every pixel of a packed buffer to white (the panel's rest state).
void ST7305_ClearBuffer(uint8_t *packed);

// Draw an unmistakable test pattern and hold it briefly.
//
// Bring-up aid, and worth keeping: with no other output device, this is the only
// way to tell "the driver works" from "the driver works and the game blit is
// wrong". The pattern is deliberately asymmetric so orientation and mirroring
// are readable at a glance.
void ST7305_TestPattern(uint8_t *packed, int hold_ms);

// Set one pixel in a packed buffer. `black` selects ink rather than background.
// Coordinates are landscape 0..399 x 0..299. Bounds-checked.
static inline void ST7305_SetPixel(uint8_t *packed, int x, int y, bool black)
{
    if ((unsigned)x >= ST7305_W || (unsigned)y >= ST7305_H) {
        return;
    }
    // The panel is mounted upside down relative to our coordinate system.
    int dy = ST7305_H - 1 - y;
    uint32_t index = (uint32_t)(x >> 1) * ST7305_BLOCKS_Y + (dy >> 2);
    uint8_t  bit   = 7u - (uint8_t)(((dy & 3) << 1) + (x & 1));

    if (black) {
        packed[index] |= (uint8_t)(1u << bit);
    } else {
        packed[index] &= (uint8_t)~(1u << bit);
    }
}

#endif // ST7305_H
