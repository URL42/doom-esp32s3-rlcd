# Session handoff — DOOM on ESP32-S3-RLCD-4.2

Paste this into a new session to resume without re-deriving anything.
Repo: <https://github.com/URL42/doom-esp32s3-rlcd> — everything below is committed and pushed.

---

## Where the project is

Doom runs on the board: full 400×300 on the reflective panel, correct orientation,
calibrated tone, ~27 fps, savegames persisting, BLE controller pairing. The demo plays
indefinitely without crashing.

**The headline bug: starting a New Game crashes the board.** Diagnosed, not fixed — see below.

### Working and verified on hardware

| Subsystem | State |
|---|---|
| Build (ESP-IDF v5.5.2, esp32s3) | clean |
| Boot, 8 MB octal PSRAM | verified |
| Zone heap 3 MB in PSRAM | verified, placement asserted at runtime |
| WAD via `esp_partition_mmap` | full 5 MB window, zero-copy lumps |
| Display (ST7305, 1 bpp, 8×8 Bayer) | 400×300, ~27 fps |
| Tone (levels + curve) | calibrated: all 16 grey-ramp bands distinguishable |
| Savegames | FAT on `storage` partition, mounted at `/fat`, Doom uses `/fat/doom/` |
| Input over USB-Serial-JTAG | working |
| BLE HID controller | pairs, keys arrive correctly mapped |
| Sound — game-side logic | live (channels, distance attenuation) |
| Sound — audio output | **stub, silent** |

Performance, measured per frame: **dither 6.8 ms, flush 17.5 ms, ~27 fps**
(was 25.5 / 24.7 / 14.6 at the start of the optimisation work).

---

## THE BUG TO FIX FIRST

**Symptom:** select New Game from the menu → board reboots → comes up in the demo.
It looks like "the demo restarted"; it is actually a crash and reboot.

**This is not a menu bug.** The menu works. `G_DoNewGame` is reached (verified by
instrumenting it). The board then panics:

```
Guru Meditation Error: Core 0 panic'ed (LoadProhibited)
EXCVADDR: 0x00000008

Z_FreeTags      z_zone.c:306
P_SetupLevel    p_setup.c:769
G_DoLoadLevel   g_game.c:655
G_InitNew       g_game.c:1889
G_DoNewGame     g_game.c:1722
```

`P_SetupLevel` calls `Z_FreeTags` to release the previous level. The zone block list is
corrupt — a `block->next` is NULL, so reading `block->tag` faults at offset 8. Corruption
only visible when the allocator walks its list means **something overran a heap buffer
earlier, silently, during demo playback**.

### Prime suspect: `solidsegs` overflow in `r_bsp.c`

```c
#define MAXSEGS  32
cliprange_t solidsegs[MAXSEGS];   // plain global, in .bss
...
newend++;                          // line ~122, NO bounds check anywhere
```

`MAXSEGS` is referenced exactly twice: the declaration and the array init. There is **no
guard on `newend`**. This is a known vanilla Doom overflow (`R_ClipSolidWallSegment`) and it
scales with how many distinct wall segments are visible — which rises with a wider view. At
320×200 you rarely reach 32; at 400 wide you plausibly do. Overrunning a `.bss` global writes
into whatever follows it, which is exactly this signature.

**Suggested fix:** raise `MAXSEGS` to ~128, or scale it from `SCREENWIDTH`. Then flash and
try New Game. If it still crashes, add a guard in `R_ClipSolidWallSegment` that drops the
segment rather than overrunning, and check whether the crash moves.

**Already ruled out:** `MAXOPENINGS` (fixed anyway — it was genuinely wrong, see below),
`MAXDRAWSEGS` (guarded at `r_segs.c:383`), `MAXVISSPRITES` (guarded, returns an overflow
sprite), `MAXVISPLANES` (guarded, "no more visplanes"), `storedemo` (only set for Doom II
without MAP01).

### This is a recurring pattern — expect more of it

Five instances found so far of a bare 320×200 constant surviving the resolution change:

1. **Status bar coordinates** — absolute positions baked for a 200-tall screen; caused an
   `I_Error` on every boot at 400×300. Fixed with `ST_XSHIFT`/`ST_YSHIFT`.
2. **View size** — `scaledviewwidth = setblocks*32` where 32 is 320/10; pinned the 3D view at
   320×168 regardless of screen size. Fixed to derive from the real screen.
3. **`R_MapPlane` spans** — degenerate spans (x2 < x1) fire during normal play at 400×300;
   vanilla treats them as fatal. Now skipped with a one-time log.
4. **`MAXOPENINGS`** — was `SCREENWIDTH*64`, where 64 is tuned for a 200-tall screen;
   consumption scales with *height*. Now scaled by `SCREENHEIGHT/200`. (Latent bug, not the
   crash cause.)
5. **`MAXSEGS`** — suspected, above.

When something breaks at 400×300, look for a hardcoded constant before anything else.

---

## Other known issues

**Title/menu screens do not fill the panel.** `TITLEPIC`, menus and the status bar are fixed
320×200 assets in the WAD, drawn top-left. The 3D view *is* resolution-independent and does
fill 400×300. The reference build gets this free because it is PrBoom, which scales WAD
graphics; vanilla-derived doomgeneric does not. Needs patch scaling at draw time — a real
feature, not a constant.

**Motion smear ("dragging").** Unresolved. Tried and eliminated:
- Higher frame rate → **made it worse** (this is the key clue)
- Frame pacing 45/60 ms → no change
- Short gate-EQ profile (`0xB3`) → no change
- TE refresh sync → **TE does not toggle on this board.** Flush went 17.5 → 39.3 ms, which is
  the 40 ms bail-out firing every frame. Default off; needs a scope on GPIO6 before more code.

A reference build on the *same board* (@ThatProject, PrBoom-based) has no visible drag, so it
is **not** a hardware limit — my earlier conclusion that it was is wrong. The one untried
mechanism is **dirty-row updates**: only send rows that changed instead of rewriting all
15000 bytes each frame. Different in kind from everything above — it reduces how many pixels
are disturbed rather than how they are driven. Also cuts flush time regardless.

**Audio is a silent stub.** Game-side logic is live and exercised; only the backend is empty.

**Board occasionally drops off USB** (`/dev/cu.usbmodem*` disappears). Happened twice, both
times with BLE active. Unplug/replug recovers it. Unproven whether it is a power brownout from
the radio or a crash taking the USB peripheral down. To separate: build with BLE disabled and
run the same duration.

---

## Suggested order of work

1. **Fix the New Game crash** (`MAXSEGS`). Highest value — it is the difference between a demo
   reel and a playable game.
2. **Verify the controller end to end** — pairing works and keys arrive, but nobody has played
   it since the input race and lag fixes landed.
3. **Audio.** Reference implementation identified: `kodediy/kodedot_examples`, `Doom/main/doom_sound.c`.
   Lift the DMX header parse (u16 format=3, u16 rate, u32 samples, **16-byte guard pads front
   and back** — miss those and every sound clicks), the 8-channel mixer with int32 accumulate
   and saturation, their generation-counter race guard, and self-pacing by blocking on the
   codec write. Adapt: mono instead of their stereo panning (our speaker connector is mono),
   our GPIO46 PA enable, our I²S pins. Uses `esp_codec_dev` + `es8311_codec`.
   **Requires a real `DOOM1.WAD` first** — the WAD currently on the board has every `DS*` and
   `D_*` lump stripped, so there is literally nothing to play.
4. **Dirty-row flush** — for both smear and speed.
5. **Patch scaling** for the title/menu screens.

---

## Hard-won facts worth not rediscovering

**Panel (ST7305):** 300×400 native portrait, addressed in that orientation; the landscape
rotation happens in the blit (`(x,y) → (y,x)`). `ST7305_Mapping = 1` is **structural, not a
preference** — a transpose alone gives a mirrored image; transpose + mirror-X is a true 90°
rotation. Setting it to 0 mirrors the screen.

**Packing:** `index = (y/2)*75 + (x/4)`, `bit = 7 - ((x%4)*2 + (y%2))`, 75×200 = 15000 bytes.

**Per-row flush is REQUIRED.** The controller does not auto-increment from the end of one row
into the start of the next; a flat dump smears the image into vertical striping. This was
tested twice, and the second time was wrongly declared working on the basis of a frame-rate
improvement. **Do not re-enable `ST7305_FlushMode = 0` without looking at the panel.**

**`CASET` starts at `0x01`, not `0x00`**, and is hoisted out of the row loop (it never
changes) — that alone took flush from 25.7 to 17.5 ms.

**Tone needs a levels stretch, not a gamma.** A single power curve cannot do it: Doom's
palette is bottom-heavy, so one global curve either crushes the dark third or lifts what
should stay black. Current calibration `black=8, white=255, gamma=0.8`. The white point was at
200, which clipped the top three of sixteen ramp bands.

**BLE:** the board is BLE-only, no BR/EDR, so Switch/X-input gamepad modes are impossible at
the silicon level. The 8BitDo Micro must be in **Keyboard mode** (`K` switch position).
Two config traps: `CONFIG_BT_NIMBLE_HID_SERVICE` must be set or `nimble_hidh.c` compiles to an
empty object and `esp_ble_hidh_init` is undefined at link — and that option lives in a menu
gated by `BT_NIMBLE_ROLE_PERIPHERAL`, so disabling the peripheral role (reasonable, since we
are a central) silently deletes it with nothing in the build output explaining why.

**Console keys** (over `screen /dev/cu.usbmodem101 115200`), intercepted before Doom sees them:
`t` grey ramp (toggles, holds on screen) · `b`/`B` black point · `w`/`W` white point ·
`[`/`]` curve · `\` Bayer↔threshold · `g` de-ghost · `o` orientation flips · `f` flush mode
(**flat is broken**) · `e` gate EQ · `r` frame pacing · `v` TE sync.

**Toolchain traps:** a stale `IDF_PATH` makes `export.sh` pick the wrong Python env — `unset
IDF_PATH` first. `sdkconfig` silently wins over `sdkconfig.defaults`; delete `sdkconfig` and
`idf.py reconfigure` after editing defaults. Arduino IDE's Serial Monitor and leftover `screen`
sessions grab the port — `lsof /dev/cu.usbmodem101` finds the holder.

---

## Working practices that mattered

**Build the fixed reference before guessing.** Tone was tuned by setting a single gamma and
comparing photographs of moving game frames; it oscillated past correct in both directions
across five values without converging. The grey-ramp screen settled it in one round trip and
identified both the wrong control and the wrong endpoint.

**A number moving is not evidence the thing works.** The flat-dump flush was declared working
because frame rate improved — frame rate cannot speak to whether pixels land in the right
place. It was striping the whole time. Any display change needs a photo before it is called
done.

**"I have run out of ideas" is not the same as "it is a hardware limit."** Motion drag was
declared physically unfixable after four failed experiments; reference photos of the same
board immediately disproved that.

**Read references for how they work, not just for the values wanted.** Four separate panel
bugs came from lifting a detail while leaving the surrounding mechanism behind — the manual CS
line, the display inversion bit, the gate voltage, and the row-addressed write were all in
files already open.
