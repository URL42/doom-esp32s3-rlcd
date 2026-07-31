# CLAUDE.md — DOOM on ESP32-S3-RLCD-4.2

## Project

Port 1993 Doom to a Waveshare ESP32-S3-RLCD-4.2: a 4.2" **fully reflective** LCD
(no backlight), 300×400 native portrait, driven over SPI by an ESP32-S3-WROOM-1-N16R8.

Base: [`bane9/ESP_DOOM`](https://github.com/bane9/ESP_DOOM) — a doomgeneric port for ESP32.
Reference for S3 build config: [`vs4vijay/M5Doom`](https://github.com/vs4vijay/M5Doom).
Upstream: [`ozkl/doomgeneric`](https://github.com/ozkl/doomgeneric).

## Working style

- I code mostly in Python/MicroPython. I'm competent but C and ESP-IDF are less familiar
  territory. **Explain the idioms as you go** — why a thing is done this way, not just what
  the code is. I want to understand this codebase, not just run it.
- Prefer small, reviewable commits over large drops.
- When you make an architectural choice, say what the alternatives were and why you rejected
  them.
- If you're uncertain, say so plainly rather than producing confident-looking code.

## Toolchain

- **ESP-IDF v5.x**, CMake, `idf.py`. Not v3, not v4, not the legacy GNU Make system.
- Target: `esp32s3`.
- 16MB flash, 8MB PSRAM (octal).
- Partition plan: ~3MB app, 4MB `wad` data partition, rest for SD-less storage/OTA.

---

## Hardware facts — AUTHORITATIVE

These are derived from the official Waveshare schematic. **Treat them as ground truth.**
Do not substitute pin numbers from other boards, tutorials, or memory. If you need a pin
that isn't listed here, stop and ask.

### Display — SPI, single data line

| Signal | GPIO |
|---|---|
| LCD_CS | 40 |
| LCD_RESET | 41 |
| LCD_SCL | 11 |
| LCD_SDA | 12 |
| LCD_RS (D/C) | 5 |
| LCD_TE | 6 |

Native resolution **300 × 400 (portrait)**. Note 300 < Doom's 320 columns — the display must
be driven **rotated to 400×300 landscape**, with the 320×200 frame centered (40px side
borders, 50px top/bottom).

### Audio

| Signal | GPIO |
|---|---|
| I2S_MCLK | 16 |
| I2S_SCLK | 9 |
| I2S_LRCK | 45 |
| I2S_DSDIN (playback → ES8311) | 8 |
| I2S_ASDOUT (capture ← ES7210) | 10 |
| PA_CTRL (amp enable) | 46 |

Codec ES8311, mic ADC ES7210, amp NS4150B → MX1.25 2-pin speaker connector.
Both codecs are configured over the shared I²C bus.

### I²C (shared bus)

| Signal | GPIO |
|---|---|
| SDA | 13 |
| SCL | 14 |

Devices: SHTC3, PCF85063 RTC, ES8311, ES7210.

### TF card — SPI

| Signal | GPIO |
|---|---|
| MOSI | 21 |
| SCK | 38 |
| MISO | 39 |
| CS | 17 (through R7) |

R7 is marked NC in the schematic. If the card doesn't enumerate, that's a hardware question,
not a software one — flag it, don't work around it in code.

### Buttons

| Button | GPIO |
|---|---|
| BOOT | 0 |
| KEY | 18 |

### Free GPIO

**GPIO1, GPIO2, GPIO3**, plus UART0 (TX 43 / RX 44) on the expansion header.
Everything else is committed. GPIO19/20 are native USB D−/D+.

### Power / USB

- Type-C wires directly to GPIO19/20 — **native USB OTG, no bridge chip**. Host mode is
  available.
- **No 5V boost.** The board cannot source VBUS. Host mode requires injecting 5V on header
  pin 2.
- **Bluetooth 5 LE only.** No BR/EDR. Any solution requiring Bluetooth Classic (Switch Pro,
  PlayStation, most 8BitDo modes) is impossible on this silicon.

---

## Hard constraints

1. **Do NOT write a panel initialization sequence.** The driver IC is unknown — it sits
   behind the FPC and does not appear in the schematic. Any init sequence you write from
   memory will be for the wrong chip. Wait for the Waveshare demo source or the datasheet.
   Until then, the display layer is a stub with a clearly marked TODO.
2. **Do NOT use `espressif/esp32-doom` as a base.** It targets ESP32-classic, uses the legacy
   Make build system, needs a 2017-era toolchain, has no sound, no savegames, and crashes on
   most menus. Read `components/prboom` for reference on PSRAM zone allocation only.
3. **Do NOT assume the board is physically present.** Until told otherwise there is no
   hardware to flash to. Build and static analysis only. Never claim something "works" on the
   basis of a successful compile.
4. **Do NOT invent GPIO assignments.** Use the tables above.
5. **Do NOT guess at panel refresh rate, colour depth, or partial-update support.** These are
   measured, not assumed, and they gate the whole design.

## Open unknowns — must be resolved by measurement or datasheet, never by guessing

| # | Unknown | Blocks | How it gets resolved |
|---|---|---|---|
| 1 | Panel driver IC | Display driver | Waveshare demo source / datasheet |
| 2 | Bits per pixel | Palette + dither strategy | Same |
| 3 | Max SPI clock | Frame budget | Same |
| 4 | Real refresh rate | **Whole project viability** | Bench measurement, day one |
| 5 | Partial/windowed update support | Sub-frame optimisation | Datasheet |
| 6 | R7 populated? | TF card | Multimeter |

## Design decisions already made

- Keep Doom's **native 8-bit indexed output**. Patch doomgeneric away from its default 32-bit
  XRGB buffer — converting via a 256-entry LUT at blit time is far cheaper.
- **WAD lives in a flash partition, accessed via `esp_partition_mmap`**, with `W_CacheLumpNum`
  patched to return pointers into the mapped region instead of copying lumps into zone
  memory. Watch for `ESP_ERR_NO_MEM` — the mmap window shares MMU address space with PSRAM,
  so windowed mapping may be necessary rather than mapping all 4MB at once.
- **Only the 320×168 3D view needs refreshing most frames**; the 320×32 status bar is
  near-static. Structure the blit path so this optimisation is possible later.
- Use the **TE line** for tear-free sync.
- CPU is not expected to be the bottleneck — a comparable ESP32-S3 Doom build reached ~34 fps
  at 320×240. The panel is the constraint.
