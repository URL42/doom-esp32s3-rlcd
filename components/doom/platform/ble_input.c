//
// BLE HID keyboard input for the ESP32-S3-RLCD-4.2.
//
// The board has BLE only -- no BR/EDR -- so anything pairing as Bluetooth
// Classic (Switch Pro, PlayStation, most 8BitDo gamepad modes) is impossible
// here at the silicon level. What does work is a controller in KEYBOARD mode,
// which advertises as a standard BLE HID keyboard.
//
// Keyboard mode is also much less work than gamepad mode: it sends fixed 8-byte
// boot-protocol reports with standard usage codes, so there is no report
// descriptor to parse. And it drops straight into the existing input path,
// which was already keystroke-based.
//
// One real gain over the serial console: HID reports carry genuine key-down and
// key-up. The 120ms synthetic hold in doomgeneric_rlcd.c exists only because a
// terminal sends a byte stream with no releases. Here we diff successive
// reports and deliver true press/release, so holding a direction actually
// holds it.
//

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "doomkeys.h"

#include "esp_hidh.h"
#include "esp_hid_common.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "esp_nimble_hci.h"

static const char *TAG = "doom.ble";

// Injected into the platform layer's key queue; see doomgeneric_rlcd.c.
extern void DG_InjectKey(int pressed, unsigned char key);

// ---------------------------------------------------------------------------
// HID usage -> Doom keycode
// ---------------------------------------------------------------------------
//
// Boot-protocol keyboard usage codes (HID Usage Table, keyboard page). Program
// the controller to emit these and Doom sees the corresponding action. The
// strafe pair is deliberately on letters rather than the arrow keys, so a
// gamepad face button can drive it -- there is no mouse on this board, and
// strafing on the shoulder of a D-pad is the difference between playable and
// not.
static unsigned char UsageToDoom(uint8_t usage)
{
    switch (usage) {
        case 0x1A: return KEY_UPARROW;     // W  - forward
        case 0x16: return KEY_DOWNARROW;   // S  - back
        case 0x36: return KEY_LEFTARROW;   // ,  - turn left
        case 0x37: return KEY_RIGHTARROW;  // .  - turn right

        case 0x04: return KEY_STRAFE_L;    // A  - strafe left
        case 0x07: return KEY_STRAFE_R;    // D  - strafe right

        case 0x09: return KEY_FIRE;        // F  - fire
        case 0x2C: return KEY_USE;         // spc- use / open

        case 0x28: return KEY_ENTER;       // Enter
        case 0x29: return KEY_ESCAPE;      // Esc
        case 0x2B: return KEY_TAB;         // Tab - automap

        // Arrow keys, in case the controller is programmed to send those.
        case 0x52: return KEY_UPARROW;
        case 0x51: return KEY_DOWNARROW;
        case 0x50: return KEY_LEFTARROW;
        case 0x4F: return KEY_RIGHTARROW;

        // Weapon select.
        case 0x1E: return '1';
        case 0x1F: return '2';
        case 0x20: return '3';
        case 0x21: return '4';
        case 0x22: return '5';
        case 0x23: return '6';
        case 0x24: return '7';

        default:   return 0;
    }
}

// ---------------------------------------------------------------------------
// Report decoding
// ---------------------------------------------------------------------------

#define KB_SLOTS 6

static uint8_t s_prev[KB_SLOTS];

static bool InReport(const uint8_t *slots, uint8_t usage)
{
    for (int i = 0; i < KB_SLOTS; i++) {
        if (slots[i] == usage) {
            return true;
        }
    }
    return false;
}

// A boot keyboard report is: [modifiers][reserved][6 key slots]. The slots hold
// whatever is currently held down, unordered, with no event semantics -- so
// press and release have to be derived by diffing against the previous report.
static void HandleReport(const uint8_t *data, size_t len)
{
    if (len < 3) {
        return;
    }

    uint8_t now[KB_SLOTS] = {0};
    size_t n = len - 2;
    if (n > KB_SLOTS) {
        n = KB_SLOTS;
    }
    memcpy(now, data + 2, n);

    // Released: present before, absent now.
    for (int i = 0; i < KB_SLOTS; i++) {
        uint8_t u = s_prev[i];
        if (u && !InReport(now, u)) {
            unsigned char k = UsageToDoom(u);
            if (k) {
                DG_InjectKey(0, k);
            }
        }
    }

    // Pressed: present now, absent before.
    for (int i = 0; i < KB_SLOTS; i++) {
        uint8_t u = now[i];
        if (u && !InReport(s_prev, u)) {
            unsigned char k = UsageToDoom(u);
            if (k) {
                DG_InjectKey(1, k);
            }
        }
    }

    memcpy(s_prev, now, KB_SLOTS);
}

// ---------------------------------------------------------------------------
// HID host plumbing
// ---------------------------------------------------------------------------

static void hidh_event(void *handler_args, esp_event_base_t base,
                       int32_t id, void *event_data)
{
    esp_hidh_event_data_t *p = (esp_hidh_event_data_t *)event_data;

    switch ((esp_hidh_event_t)id) {
        case ESP_HIDH_OPEN_EVENT:
            ESP_LOGW(TAG, "controller connected");
            memset(s_prev, 0, sizeof(s_prev));
            break;

        case ESP_HIDH_INPUT_EVENT:
            HandleReport(p->input.data, p->input.length);
            break;

        case ESP_HIDH_CLOSE_EVENT:
            ESP_LOGW(TAG, "controller disconnected");
            // Release everything, or a key held at disconnect latches forever
            // and the player walks into a wall.
            for (int i = 0; i < KB_SLOTS; i++) {
                unsigned char k = UsageToDoom(s_prev[i]);
                if (k) {
                    DG_InjectKey(0, k);
                }
            }
            memset(s_prev, 0, sizeof(s_prev));
            break;

        default:
            break;
    }
}

static void ble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t BLE_InputInit(void)
{
    esp_err_t e = nimble_port_init();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(e));
        return e;
    }

    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = 3;
    ble_hs_cfg.sm_their_key_dist = 3;

    esp_hidh_config_t cfg = {
        .callback = hidh_event,
        .event_stack_size = 4096,
        .callback_arg = NULL,
    };
    e = esp_hidh_init(&cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_hidh_init: %s", esp_err_to_name(e));
        return e;
    }

    // The radio runs on core 1; the game loop is pinned to core 0, so the two
    // do not contend for frame time.
    nimble_port_freertos_init(ble_host_task);

    ESP_LOGW(TAG, "BLE HID host up -- put the controller in KEYBOARD mode and pair it");
    return ESP_OK;
}
