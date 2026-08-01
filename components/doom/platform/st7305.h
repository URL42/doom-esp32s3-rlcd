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

// Panel geometry in the controller's NATIVE orientation: 300 wide, 400 tall.
//
// Earlier revisions called the 400-pixel axis "width" and treated the panel as
// 400x300 landscape. The packing was right -- the image was recognisable -- but
// the axes were named backwards, so everything appeared rotated 90 degrees. The
// rotation belongs in the blit, not in the addressing.
#define ST7305_W 300
#define ST7305_H 400

// Each byte holds a 4(x) x 2(y) block of pixels in native orientation, and the
// buffer is row-major over those blocks:
//
//     index = (y / 2) * 75 + (x / 4)
//     bit   = 7 - ((x % 4) * 2 + (y % 2))
//
// (300/4) * (400/2) = 75 * 200 = 15000 bytes = 300*400/8, as it must be for 1bpp.
#define ST7305_ROW_BYTES  (ST7305_W / 4)                 // 75
#define ST7305_ROWS       (ST7305_H / 2)                 // 200
#define ST7305_FB_BYTES   (ST7305_ROW_BYTES * ST7305_ROWS)  // 15000

// Flip control, kept runtime-adjustable: bit0 mirrors X, bit1 mirrors Y. Which
// way round the glass is mounted is not something the datasheet settles.
extern int ST7305_Mapping;

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

// Drive every pixel to both extremes to clear retained images. Uses the buffer
// passed in as scratch, so the caller must redraw afterwards.
void ST7305_Deghost(uint8_t *scratch);

// Set one pixel in a packed buffer. `black` selects ink rather than background.
// Coordinates are landscape 0..399 x 0..299. Bounds-checked.
// Pixel mapping variant. Two are dimensionally valid for a 15000-byte buffer:
//   0 = column-major  index = (x/2)*75 + (y/4)      -- x is the major axis
//   1 = row-major     index = (y/4)*200 + (x/2)     -- y is the major axis
// They differ by a 90 degree rotation on the glass. Which one the controller
// actually wants is not something the datasheet excerpts settle, so it is
// selectable at runtime rather than guessed. Bit 1 flips the Y inversion.
extern int ST7305_Mapping;

// Set one pixel, in NATIVE PORTRAIT coordinates: x 0..299, y 0..399.
// Callers wanting a landscape image rotate on the way in; see DG_DrawFrame.
static inline void ST7305_SetPixel(uint8_t *packed, int x, int y, bool black)
{
    if ((unsigned)x >= ST7305_W || (unsigned)y >= ST7305_H) {
        return;
    }
    if (ST7305_Mapping & 1) x = ST7305_W - 1 - x;
    if (ST7305_Mapping & 2) y = ST7305_H - 1 - y;

    uint32_t index = (uint32_t)(y >> 1) * ST7305_ROW_BYTES + (uint32_t)(x >> 2);
    uint8_t  bit   = 7u - (uint8_t)(((x & 3) << 1) + (y & 1));

    if (black) {
        packed[index] |= (uint8_t)(1u << bit);
    } else {
        packed[index] &= (uint8_t)~(1u << bit);
    }
}

#endif // ST7305_H
