#ifndef DOOM_GENERIC_H
#define DOOM_GENERIC_H

#include <stdint.h>
#include <stdlib.h>

//
// Framebuffer geometry.
//
// ESP_DOOM derived these from its panel driver header (ILI9341_WIDTH/HEIGHT),
// which is what welded doomgeneric to one specific TFT. We deliberately do not
// do that: nothing in this header knows what panel is attached.
//
// The panel is 300x400 native portrait, driven rotated to 400x300 landscape.
// Doom renders 320x200. Rather than letterbox in software, we keep the
// framebuffer at exactly Doom's resolution and let the (future) display driver
// blit it into a 320x200 window offset 40px horizontally and 50px vertically
// within the 400x300 landscape frame.
//
// Two things fall out of that choice:
// Doom now renders at the panel's full native 400x300 rather than its
// traditional 320x200 letterboxed into the middle. ESP_DOOM had already made
// the resolution-dependent renderer arrays runtime allocations, so the renderer
// simply sizes itself to whatever these say; MAXWIDTH/MAXHEIGHT have the room.
//
// The framebuffer is therefore 120000 bytes rather than 64000, and the centring
// offsets below collapse to zero.
//
// One thing does not scale: the STBAR status bar graphic in the WAD is 320px
// wide whatever the screen is, so it is centred rather than pinned left. See
// ST_X in st_stuff.c.
//
#define DOOMGENERIC_RESX 400
#define DOOMGENERIC_RESY 300

// Where the Doom frame sits inside the 400x300 landscape panel. Consumed by the
// display driver when it opens its write window -- see the TODO in
// doomgeneric_rlcd.c.
#define RLCD_PANEL_W 400
#define RLCD_PANEL_H 300
#define RLCD_FRAME_X ((RLCD_PANEL_W - DOOMGENERIC_RESX) / 2)  // 0 -- full panel
#define RLCD_FRAME_Y ((RLCD_PANEL_H - DOOMGENERIC_RESY) / 2)  // 0 -- full panel

// The frame handed to DG_DrawFrame, in Doom's native 8-bit indexed colour.
//
// Doom renders into an 8-bit paletted buffer. Upstream doomgeneric expands that
// to 32-bit XRGB before handing it over, and ESP_DOOM expanded it to RGB565;
// both do a per-pixel table lookup every frame -- 64000 lookups plus twice the
// bytes to move.
//
// We keep the indexed form all the way to the panel. Converting through
// DG_Palette happens in the display driver at blit time, where it can be fused
// into the loop that is already touching every pixel to push it over SPI. That
// halves framebuffer memory and removes a whole pass over the frame.
extern uint8_t *DG_ScreenBuffer;

// Doom's 256-colour palette, reduced to perceptual luminance (0 = black,
// 255 = white).
//
// The panel is an ST7305: 1 bit per pixel, pure black and white, no colour at
// all. So there is nothing to convert colour *to* -- each palette entry becomes
// a brightness, and the blit dithers that brightness down to a single bit.
//
// Doom rewrites this at runtime -- damage flashes red, item pickups flash gold,
// the invulnerability sphere inverts it -- by calling I_SetPalette with a new
// 768-byte RGB triple table. Only these 256 entries change; the framebuffer does
// not. That is why keeping the frame indexed pays off twice here: a full-screen
// palette effect costs 256 luminance conversions rather than 64000.
//
// A red damage flash does still read on a monochrome panel, incidentally: the
// flash palette raises luminance across the board, so the screen visibly
// brightens even though the hue is gone.
extern uint8_t DG_Palette[256];

// Which 8-bit -> 1-bit reduction the blit uses. Switchable so the two can be
// compared on the real panel; see DG_DrawFrame.
#define DG_DITHER_BAYER     0
#define DG_DITHER_THRESHOLD 1
void DG_SetDither(int mode);

// Panel contrast. >1 darkens midtones so more pixels cross a dither threshold;
// <1 lightens. Reflective LCDs need this pushed well above 1.
void DG_SetGamma(float gamma);
float DG_GetGamma(void);

void DG_Init(void);
void DG_DrawFrame(void);
void DG_SleepMs(uint32_t ms);
uint32_t DG_GetTicksMs(void);
int DG_GetKey(int *pressed, unsigned char *key);
void DG_SetWindowTitle(const char *title);

#endif // DOOM_GENERIC_H
