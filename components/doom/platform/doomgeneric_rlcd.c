//
// doomgeneric platform layer for the Waveshare ESP32-S3-RLCD-4.2.
//
// Input comes over the USB-serial console, which keeps it decoupled from the
// panel. The display is an ST7305 reflective LCD -- see st7305.c.
//

#include "doomgeneric.h"
#include "doomkeys.h"
#include "st7305.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "doom.rlcd";

uint8_t *DG_ScreenBuffer = NULL;

#define FRAMEBUFFER_BYTES (DOOMGENERIC_RESX * DOOMGENERIC_RESY)  // 8-bit indexed

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------

uint32_t DG_GetTicksMs(void)
{
    // esp_timer is a 64-bit microsecond counter started during early boot. Doom
    // only ever uses tick deltas, so truncating to 32 bits is fine -- it wraps
    // after ~49 days of uptime, and unsigned subtraction stays correct across
    // the wrap.
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void DG_SleepMs(uint32_t ms)
{
    // ESP_DOOM used portTICK_RATE_MS, which was renamed in ESP-IDF v4 and no
    // longer exists in v5.
    vTaskDelay(pdMS_TO_TICKS(ms));
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

static uint32_t frame_count;
static uint32_t frames_window_start_ms;

// ---------------------------------------------------------------------------
// Indexed -> 1bpp conversion
// ---------------------------------------------------------------------------
//
// The panel has one bit per pixel, so Doom's 256 shades have to collapse to
// black or white. Which reduction is used is a visible, arguable choice, so it
// sits behind a function pointer and can be swapped at runtime to compare them
// on the actual hardware rather than from a description.

typedef int (*dither_fn)(int x, int y, uint8_t luma);

// 8x8 ordered (Bayer) matrix, scaled to 0..255.
//
// 8x8 rather than 4x4: 64 distinct thresholds instead of 16. On a 1-bit panel
// that is the difference between Doom's distance shading having gradations and
// it banding into a few flat steps -- most visible in dark corridors, which are
// exactly where a 4x4 matrix runs out of levels and goes to mush.
//
// Ordered rather than error-diffused, because it is *temporally stable*: a given
// brightness always resolves the same way at a given screen position, so panning
// the view does not make flat walls crawl. Error diffusion gives better stills
// but reshuffles its entire pattern when the image shifts by one pixel.
//
// Values are the standard recursive Bayer construction (0..63) scaled by 4 and
// offset by 2, so no threshold sits exactly at 0 or 255.
static const uint8_t bayer8[64] = {
      2, 130,  34, 162,  10, 138,  42, 170,
    194,  66, 226,  98, 202,  74, 234, 106,
     50, 178,  18, 146,  58, 186,  26, 154,
    242, 114, 210,  82, 250, 122, 218,  90,
     14, 142,  46, 174,   6, 134,  38, 166,
    206,  78, 238, 110, 198,  70, 230, 102,
     62, 190,  30, 158,  54, 182,  22, 150,
    254, 126, 222,  94, 246, 118, 214,  86,
};

static int DitherBayer(int x, int y, uint8_t luma)
{
    return luma < bayer8[((y & 7) << 3) | (x & 7)];   // 1 = ink
}

// Straight 50% cut. Sharpest text, but Doom's distance shading collapses.
static int DitherThreshold(int x, int y, uint8_t luma)
{
    (void)x; (void)y;
    return luma < 128;
}

static dither_fn s_dither = DitherBayer;

void DG_SetDither(int mode)
{
    s_dither = (mode == DG_DITHER_THRESHOLD) ? DitherThreshold : DitherBayer;
    ESP_LOGI(TAG, "dither mode -> %s", mode == DG_DITHER_THRESHOLD ? "threshold" : "bayer8");
}

// Packed 1bpp frame in the ST7305's own layout.
static uint8_t *s_panel_fb;

void DG_DrawFrame(void)
{
    if (s_panel_fb != NULL) {
        // Doom's 320x200 frame is centred in the 400x300 panel; the surrounding
        // border was cleared once at init and is never redrawn.
        for (int y = 0; y < DOOMGENERIC_RESY; y++) {
            const uint8_t *row = DG_ScreenBuffer + (size_t)y * DOOMGENERIC_RESX;
            const int py = RLCD_FRAME_Y + y;

            for (int x = 0; x < DOOMGENERIC_RESX; x++) {
                uint8_t luma = DG_Palette[row[x]];
                ST7305_SetPixel(s_panel_fb, RLCD_FRAME_X + x, py,
                                s_dither(x, y, luma));
            }
        }

        // TODO(TE): LCD_TE (GPIO6) is configured as an input but not yet waited
        // on. Tearing should be checked on real hardware before deciding whether
        // syncing to it is worth the added latency at 32Hz.
        ST7305_Flush(s_panel_fb);
    }

    frame_count++;

    if ((frame_count % 100u) == 0u) {
        uint32_t now = DG_GetTicksMs();
        uint32_t elapsed = now - frames_window_start_ms;
        // Integer maths only: 100 frames in `elapsed` ms -> fps*100.
        uint32_t fps_x100 = elapsed ? (100u * 100u * 1000u) / elapsed : 0u;
        ESP_LOGI(TAG, "frame %lu | 100 frames in %lu ms | %lu.%02lu fps",
                 (unsigned long)frame_count, (unsigned long)elapsed,
                 (unsigned long)(fps_x100 / 100u), (unsigned long)(fps_x100 % 100u));
        frames_window_start_ms = now;
    }
}

void DG_SetWindowTitle(const char *title)
{
    (void)title;  // No window manager on a bare panel.
}

// ---------------------------------------------------------------------------
// Input -- serial console
// ---------------------------------------------------------------------------
//
// Reading keys from stdin keeps input completely decoupled from display
// bring-up. It also means WASD over a serial terminal works before the panel
// does, which is the only way to prove the game loop is really running while
// DG_DrawFrame is a stub.
//
// Terminals hand us a byte stream, not key up/down events, so the release has to
// be synthesised. HOW it is synthesised matters more than it looks.
//
// The obvious approach -- queue the press and its release together -- does not
// work, and fails in a way that reads as a Doom bug rather than a platform bug.
// The game loop in d_loop.c runs:
//
//     I_StartTic();              // drains us into the event queue
//     ProcessEvents();           // G_Responder: keydown sets gamekeydown[k],
//                                //              keyup clears it
//     BuildTiccmd();             // polls gamekeydown[k]
//
// ProcessEvents drains the whole queue before BuildTiccmd looks at anything, so
// a press and release delivered in the same pass set and clear gamekeydown[k]
// with nothing in between ever observing it. Movement, fire and use are all
// polled that way and would silently never fire. Menus would still work, because
// M_Responder is edge-triggered on ev_keydown -- which is exactly what makes the
// symptom so misleading.
//
// So a pressed key is held down for a fixed span and released later, on a
// subsequent call, guaranteeing at least one BuildTiccmd sees it held. Repeats
// from the terminal's own key-repeat extend the hold rather than re-pressing,
// which turns a held key into continuous motion.

#define KEY_QUEUE_LEN 32

// How long a key stays "down" after its byte arrives. One tic is ~28.6ms at
// 35Hz; this is comfortably several tics, so a tap produces a visible step and
// terminal auto-repeat (typically ~30ms) sustains the hold without gaps.
#define KEY_HOLD_MS 120

#define MAX_HELD_KEYS 8

static unsigned char key_queue[KEY_QUEUE_LEN];
static int key_queue_pressed[KEY_QUEUE_LEN];
static int key_queue_head;
static int key_queue_tail;

// Keys currently synthesised as held, with the time their release is due.
static struct {
    unsigned char key;      // 0 = slot free
    uint32_t release_at;
} held_keys[MAX_HELD_KEYS];

// Returns non-zero if the event was queued.
static int KeyQueuePush(int pressed, unsigned char key)
{
    int next = (key_queue_head + 1) % KEY_QUEUE_LEN;
    if (next == key_queue_tail) {
        return 0;  // Full. Never block the game loop.
    }
    key_queue[key_queue_head] = key;
    key_queue_pressed[key_queue_head] = pressed;
    key_queue_head = next;
    return 1;
}

// Register a key as pressed, or extend its hold if it already is.
static void KeyPress(unsigned char key)
{
    uint32_t now = DG_GetTicksMs();

    for (int i = 0; i < MAX_HELD_KEYS; i++) {
        if (held_keys[i].key == key) {
            held_keys[i].release_at = now + KEY_HOLD_MS;  // repeat: extend
            return;
        }
    }

    for (int i = 0; i < MAX_HELD_KEYS; i++) {
        if (held_keys[i].key == 0) {
            // Only claim the slot if the press actually made it into the queue.
            // Taking the slot on a full queue would leave a key held with no
            // press delivered, and it would never be released.
            if (!KeyQueuePush(1, key)) {
                return;
            }
            held_keys[i].key = key;
            held_keys[i].release_at = now + KEY_HOLD_MS;
            return;
        }
    }
    // More than MAX_HELD_KEYS at once: drop. Doom needs far fewer.
}

// Emit releases for any holds that have expired.
static void ReleaseExpiredKeys(void)
{
    uint32_t now = DG_GetTicksMs();

    for (int i = 0; i < MAX_HELD_KEYS; i++) {
        if (held_keys[i].key == 0) {
            continue;
        }
        // Unsigned comparison via subtraction stays correct across tick wrap.
        if ((int32_t)(now - held_keys[i].release_at) < 0) {
            continue;
        }
        // If the queue is full, keep the hold and retry next call rather than
        // dropping the release -- a dropped release latches the key down
        // forever and the player walks into a wall.
        if (KeyQueuePush(0, held_keys[i].key)) {
            held_keys[i].key = 0;
        }
    }
}

// Translate one byte from the console into a Doom keycode, or 0 if we do not
// care about it.
static unsigned char TranslateByte(int c)
{
    switch (c) {
        case 'w': case 'W': return KEY_UPARROW;
        case 's': case 'S': return KEY_DOWNARROW;
        case 'a': case 'A': return KEY_STRAFE_L;
        case 'd': case 'D': return KEY_STRAFE_R;
        case ',':           return KEY_LEFTARROW;
        case '.':           return KEY_RIGHTARROW;
        case ' ':           return KEY_USE;
        case '\r': case '\n': return KEY_ENTER;
        case 0x1b:          return KEY_ESCAPE;   // bare ESC; see arrow handling
        case 0x7f: case '\b': return KEY_BACKSPACE;
        case '\t':          return KEY_TAB;
        case 'f': case 'F': return KEY_FIRE;
        case 'y': case 'Y': return 'y';          // menu confirmations
        case 'n': case 'N': return 'n';
        case '1': return '1';
        case '2': return '2';
        case '3': return '3';
        case '4': return '4';
        case '5': return '5';
        case '6': return '6';
        case '7': return '7';
        default:  return 0;
    }
}

// Arrow keys arrive as the three-byte sequence ESC '[' {A,B,C,D}. We keep a tiny
// state machine rather than blocking on a read, because DG_GetKey is called from
// the game loop and must never stall it.
static int esc_state;  // 0 = idle, 1 = saw ESC, 2 = saw ESC '['
static uint32_t esc_seen_ms;

// A bare ESC is indistinguishable from the start of an arrow sequence until the
// next byte arrives -- and if the user just pressed Escape to open the menu,
// that byte may never come. Terminals emit all three bytes of an arrow in one
// burst, so anything still pending after this long was a real Escape.
#define ESC_DISAMBIGUATE_MS 50

// Bound the work done per call. Without a cap this loop only exits on EOF, so a
// noisy or continuously-fed console would spin here and stall the game loop --
// and the idle-task watchdog on core 0 is deliberately disabled, so it would
// hang silently rather than panic. Anything unread stays buffered for next call.
#define MAX_BYTES_PER_PUMP 64

static void PumpConsole(void)
{
    for (int n = 0; n < MAX_BYTES_PER_PUMP; n++) {
        int c = fgetc(stdin);
        if (c == EOF) {
            // Both partial escape states need a timeout, not just state 1. A
            // byte lost after ESC '[' would otherwise strand us in state 2,
            // silently swallowing the next key the user pressed.
            if (esc_state != 0 && (DG_GetTicksMs() - esc_seen_ms) > ESC_DISAMBIGUATE_MS) {
                if (esc_state == 1) {
                    KeyPress(KEY_ESCAPE);   // it really was a bare Escape
                }
                esc_state = 0;
            }
            return;
        }

        if (esc_state == 1) {
            if (c == '[') { esc_state = 2; continue; }
            esc_state = 0;
            // Lone ESC followed by something else: emit the ESC we held back,
            // then fall through and process this byte normally.
            KeyPress(KEY_ESCAPE);
        } else if (esc_state == 2) {
            esc_state = 0;
            unsigned char k = 0;
            switch (c) {
                case 'A': k = KEY_UPARROW;    break;
                case 'B': k = KEY_DOWNARROW;  break;
                case 'C': k = KEY_RIGHTARROW; break;
                case 'D': k = KEY_LEFTARROW;  break;
                default: break;
            }
            if (k) {
                KeyPress(k);
            }
            continue;
        }

        if (c == 0x1b) {
            esc_state = 1;  // Might be an arrow; decide on the next byte.
            esc_seen_ms = DG_GetTicksMs();
            continue;
        }

        unsigned char k = TranslateByte(c);
        if (k) {
            KeyPress(k);
        }
    }
}

int DG_GetKey(int *pressed, unsigned char *key)
{
    PumpConsole();
    ReleaseExpiredKeys();

    if (key_queue_tail == key_queue_head) {
        return 0;
    }

    *pressed = key_queue_pressed[key_queue_tail];
    *key = key_queue[key_queue_tail];
    key_queue_tail = (key_queue_tail + 1) % KEY_QUEUE_LEN;

    // Bring-up aid: with no panel there is no other way to see that a keystroke
    // reached the game. ESP_LOGD so it costs nothing unless the log level is
    // raised (CONFIG_LOG_MAXIMUM_LEVEL / esp_log_level_set).
    ESP_LOGD(TAG, "key 0x%02x %s", *key, *pressed ? "down" : "up");

    return 1;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

void DG_Init(void)
{
    // Non-blocking stdin. Without this, fgetc blocks the whole game loop waiting
    // for a keypress. The UART VFS driver honours O_NONBLOCK on the underlying
    // fd; unbuffering stdio stops libc from doing its own blocking refill.
    setvbuf(stdin, NULL, _IONBF, 0);

    // Install the USB-Serial-JTAG driver and route the VFS through it. This is
    // not optional, and the reason is not obvious:
    //
    // Without the driver, usb_serial_jtag_get_read_bytes_available() returns 0
    // unconditionally (it only inspects the driver's RX ring buffer, which does
    // not exist). The VFS non-blocking read path consults *only* that function
    // to decide how much to fetch, and the no-driver FIFO reader is reached
    // solely from the blocking branch. So with O_NONBLOCK set and no driver,
    // read() returns EWOULDBLOCK forever no matter what the host sent -- log
    // output works perfectly and input silently never arrives.
    usb_serial_jtag_driver_config_t usj_cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usj_cfg.rx_buffer_size = 256;   // a few keystrokes is plenty
    usj_cfg.tx_buffer_size = 1024;  // logging is chattier than input

    esp_err_t err = usb_serial_jtag_driver_install(&usj_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_serial_jtag_driver_install failed: %s -- no input",
                 esp_err_to_name(err));
    } else {
        usb_serial_jtag_vfs_use_driver();
    }

    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0) {
        // Worth shouting about: if stdin stays blocking, the first fgetc in
        // PumpConsole never returns and the game loop stops dead, with no
        // output and no watchdog to catch it. Silent hang, no clue why.
        ESP_LOGE(TAG, "could not set stdin non-blocking -- input will hang the game loop");
    }

    DG_ScreenBuffer = heap_caps_malloc(FRAMEBUFFER_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (DG_ScreenBuffer == NULL) {
        ESP_LOGE(TAG, "framebuffer alloc failed (%d bytes)", (int)FRAMEBUFFER_BYTES);
        abort();
    }
    memset(DG_ScreenBuffer, 0, FRAMEBUFFER_BYTES);

    frames_window_start_ms = DG_GetTicksMs();

    ESP_LOGI(TAG, "framebuffer %dx%d 8bpp indexed -> %d bytes in PSRAM",
             DOOMGENERIC_RESX, DOOMGENERIC_RESY, (int)FRAMEBUFFER_BYTES);
    ESP_LOGI(TAG, "heap: internal %u free (%u largest), PSRAM %u free (%u largest)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    // Panel. If this fails the game still runs headless rather than aborting --
    // input and timing remain observable over the console, which is how the
    // whole platform layer was brought up in the first place.
    if (ST7305_Init() == ESP_OK) {
        s_panel_fb = heap_caps_malloc(ST7305_FB_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (s_panel_fb == NULL) {
            ESP_LOGE(TAG, "no DMA-capable memory for the %d-byte panel buffer", ST7305_FB_BYTES);
        } else {
            // Clear once: the 40px side and 50px top/bottom borders around
            // Doom's 320x200 frame never change, so they cost nothing per frame.
            ST7305_TestPattern(s_panel_fb, 4000);
            ST7305_ClearBuffer(s_panel_fb);
            ST7305_Flush(s_panel_fb);
            ESP_LOGI(TAG, "panel ready: %dx%d frame at (%d,%d) on a %dx%d panel",
                     DOOMGENERIC_RESX, DOOMGENERIC_RESY,
                     RLCD_FRAME_X, RLCD_FRAME_Y, ST7305_W, ST7305_H);
        }
    } else {
        ESP_LOGE(TAG, "panel init failed -- running headless");
    }

    // Keep key tracing visible during bring-up without turning on debug logging
    // globally. Drop this once there is a panel to watch instead.
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
}
