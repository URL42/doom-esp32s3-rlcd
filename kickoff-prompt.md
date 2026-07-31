# Kickoff prompt — paste into Claude Code

> Drop `CLAUDE.md` in the repo root first, then paste everything below the line.

---

We're porting Doom to a Waveshare ESP32-S3-RLCD-4.2 board. Read `CLAUDE.md` first — it has
the authoritative pin map, the hard constraints, and the list of unknowns you must not guess
at. Everything in this task respects those.

**The hardware is not here yet.** It arrives in a few days. This session is Phase 0: get the
project building and the platform layer scaffolded so that day one with the board is spent on
the display, not on toolchain problems. You cannot flash anything. You cannot test against
real hardware. Do not pretend otherwise.

## Scope for this session

1. **Set up the project.**
   - ESP-IDF v5.x, CMake, target `esp32s3`.
   - Vendor in `bane9/ESP_DOOM`, or upstream `ozkl/doomgeneric` if ESP_DOOM's display
     abstraction turns out to be welded to a specific TFT driver. Assess this and tell me
     which you picked and why before you commit to one.
   - Cross-reference `vs4vijay/M5Doom`'s `platformio.ini` and sdkconfig for S3-specific flags
     — PSRAM mode, flash size, and the `build_src_filter` exclusions that drop the
     emscripten / allegro / SDL platform files.

2. **Partition table.** ~3MB app, 4MB `wad` data partition, 16MB flash total. Give me the
   `partitions.csv` and explain the layout.

3. **Platform layer.** Create `doomgeneric_rlcd.c` implementing the five doomgeneric entry
   points:

   | Function | Phase 0 implementation |
   |---|---|
   | `DG_Init` | Init UART stdin for input. Log PSRAM/heap available at boot. |
   | `DG_DrawFrame` | **Stub.** Count frames, log timing every 100 frames. Do not draw. Mark clearly with a TODO — the panel driver IC is unknown. |
   | `DG_GetTicksMs` | `esp_timer_get_time() / 1000` |
   | `DG_SleepMs` | `vTaskDelay(pdMS_TO_TICKS(ms))` |
   | `DG_GetKey` | Read from stdin over the serial console. Map WASD/arrows/ctrl/space to Doom keycodes. |

   The stdin input path matters — it means I can play-test with a keyboard the moment the
   display works, and it keeps input fully decoupled from display bring-up.

4. **WAD access via mmap.** Patch the `W_` layer so `W_CacheLumpNum` returns pointers into an
   `esp_partition_mmap`'d region rather than copying lumps into zone memory. Handle
   `ESP_ERR_NO_MEM` — the mmap window shares MMU address space with PSRAM, so we may need
   windowed mapping instead of one 4MB map. Walk me through this change carefully; it's the
   part of the codebase I most want to actually understand.

5. **Zone memory into PSRAM.** Configure the allocator and report the budget.

6. **It must build clean.** `idf.py build` completing with no errors is the deliverable.

## What I want at the end

- A repo that compiles to a flashable binary.
- A short written summary of: which base you chose and why, the memory budget (app size,
  PSRAM allocated, mmap window size), and anything in the doomgeneric source that looks like
  it will fight us later.
- An explicit list of every place you left a TODO for the unknown panel.

## Stop and ask me if

- You find yourself about to write a display init sequence. The driver IC is unknown. This is
  the single most likely way for this session to go wrong.
- ESP_DOOM's structure makes the display swap harder than starting from upstream doomgeneric.
- The mmap approach hits an ESP-IDF limitation that changes the WAD strategy.
- You need a GPIO that isn't in the `CLAUDE.md` pin table.
- Anything requires the physical board to verify.

## Explicitly out of scope this session

Panel driver, audio (ES8311), controller/USB host, savegames, dual-boot with the existing
dashboard firmware. Later phases. Don't get ahead.
