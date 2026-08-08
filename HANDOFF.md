# Session handoff — DOOM on ESP32-S3-RLCD-4.2

Paste this into a new session to resume without re-deriving anything.
Repo: <https://github.com/URL42/doom-esp32s3-rlcd> — everything below is committed and pushed.

---

## Where the project is

Doom runs on the board: full 400×300 on the reflective panel, correct orientation,
calibrated tone, ~27 fps, savegames persisting, BLE controller pairing.

**The New Game crash is fixed** — see below. It was a status bar buffer allocated 320
bytes per row on a 400-wide screen, overrunning the zone heap on every frame the status
bar was visible. Verified on hardware: 40+ seconds of play past New Game, no panic, no
heap corruption reported, steady 27.97 fps.

Note the correction to the previous handoff: the demo did **not** play "indefinitely
without crashing". The heap was already corrupt roughly 250 frames into the first demo;
a reboot back into the demo loop is indistinguishable from the demo looping normally,
which is exactly how it hid.

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

## THE NEW GAME CRASH — SOLVED

**Cause:** `ST_Init` allocated `st_backing_screen` as `ST_WIDTH * ST_HEIGHT` =
320 × 32 = 10240 bytes. But `ST_refreshBackground` installs that buffer with
`V_UseBuffer` and then draws into it with `V_DrawPatch`, and **every writer in
`v_video.c` strides by `SCREENWIDTH`** regardless of which buffer is installed. At
400×300 the status bar needs 32 × 400 = 12800 bytes. It had 10240.

The `sbar` patch is 320 wide drawn at `ST_X` = 40, so it reaches offset 31 × 400 + 359 =
12759 — rows 26–31 entirely plus part of row 25, **2040 bytes past the end of the
allocation and straight through the header of the next zone block**, on every frame the
status bar was visible. (2560 is the shortfall in the allocation, not what was written.)
`V_CopyRect` on the way back out then read those same 2040 bytes of somebody else's
memory — the quieter half of the same bug, and one that would have produced garbage
pixels in the status bar rather than a crash.

`ST_WIDTH` is the width of the `sbar` graphic in the WAD. In vanilla that is also the
screen width, so the two are interchangeable and nobody ever had to decide which one that
line meant. It meant the screen. One-line fix in `st_stuff.c`.

**Why the crash surfaced where it did.** The damage happens during ordinary rendering.
The game only falls over later, when `P_SetupLevel` calls `Z_FreeTags` and the allocator
finally walks the list it has been handed. The stack trace pointed at the allocator
because the allocator is the only thing that reads those headers — it was the victim, not
the scene.

**Why `RANGECHECK` did not catch it.** `V_DrawPatch` validates its *coordinates* against
the screen, and the coordinates were entirely legal. It was the buffer that was the wrong
size, and nothing in the codebase knows how big `dest_screen` is. Do not expect
`RANGECHECK` to catch this class of bug.

### The previous handoff's prime suspect was wrong

`MAXSEGS` / `solidsegs` was the stated prime suspect. It is not, and the linker map says
why in about thirty seconds:

```
.bss.solidsegs  0x3fcac8c8  0x100
.bss.newend     0x3fcac9c8  0x4
.bss.ds_p       0x3fcac9cc  0x4
.bss.drawsegs   0x3fcac9d0  0x4
```

`solidsegs` is in **internal SRAM** (`0x3fc…`) and the zone is in **PSRAM** (`0x3c…`).
The values it spills are screen x-coordinates, which cannot be mistaken for a PSRAM
address, so it can never damage a zone block header. Worse for the theory: `newend`,
`ds_p` and `drawsegs` sit immediately behind the array, so the first overrun destroys the
pointer doing the overrunning and faults instantly rather than corrupting anything
quietly. It was also never actually overflowing — the guard added below did not fire once
across two full runs.

**Check the linker map before theorising about memory corruption.** Which region a symbol
lives in usually eliminates most of the candidate list for free.

`MAXSEGS` was fixed anyway (scaled from `SCREENWIDTH`, plus the bounds guard vanilla never
had) because it is a real unguarded overflow. Just not that one.

**Also ruled out, still true:** `MAXOPENINGS` (fixed separately — it was genuinely wrong),
`MAXDRAWSEGS` (guarded at `r_segs.c:383`), `MAXVISSPRITES` (guarded, returns an overflow
sprite), `MAXVISPLANES` (guarded, "no more visplanes"), `storedemo` (only set for Doom II
without MAP01).

### This is a recurring pattern — expect more of it

Six instances found so far of a bare 320×200 constant surviving the resolution change:

1. **Status bar coordinates** — absolute positions baked for a 200-tall screen; caused an
   `I_Error` on every boot at 400×300. Fixed with `ST_XSHIFT`/`ST_YSHIFT`.
2. **View size** — `scaledviewwidth = setblocks*32` where 32 is 320/10; pinned the 3D view at
   320×168 regardless of screen size. Fixed to derive from the real screen.
3. **`R_MapPlane` spans** — degenerate spans (x2 < x1) fire during normal play at 400×300;
   vanilla treats them as fatal. Now skipped with a one-time log.
4. **`MAXOPENINGS`** — was `SCREENWIDTH*64`, where 64 is tuned for a 200-tall screen;
   consumption scales with *height*. Now scaled by `SCREENHEIGHT/200`. (Latent bug, not the
   crash cause.)
5. **`MAXSEGS`** — 32 was a 320-wide number. Now `SCREENWIDTH/2 + 2`, with the bounds
   guard vanilla never had. Latent; was not overflowing in practice.
6. **`st_backing_screen`** — allocated `ST_WIDTH`-strided, written `SCREENWIDTH`-strided.
   **This was the New Game crash.**

When something breaks at 400×300, look for a hardcoded constant before anything else.
Note that #6 is a nastier variant than #1–#5: the constant was not a limit or a
coordinate, it was a **buffer size that had to agree with a stride computed somewhere
else**. Grep for other allocations sized from anything other than `SCREENWIDTH` that are
then written through `v_video.c`.

That grep has been done once: `st_backing_screen` and `background_buffer` (`r_draw.c`)
are the only two buffers ever handed to `V_UseBuffer`, and every other screen-sized
allocation already uses `SCREENWIDTH`. **`background_buffer` is safe, but only just, and
not for a good reason.** It is 400 × 268, and the deepest write is the `brdr_b` patch at
`viewwindowy + viewheight`, 8 rows tall. At `setblocks` = 9 that is rows 254–261 against
268 available — six rows of margin. At `setblocks` = 10 it would be rows 266–273 into a
268-row buffer, and the only thing preventing that is `R_FillBackScreen` early-returning
when `scaledviewwidth == SCREENWIDTH`. Anything that changes the view-size logic needs to
re-check this.

---

## The heap validator — use this, do not re-derive it

`ZONE_DEBUG` in `z_zone.h` (currently **on**) buys two things:

- **Per-block provenance.** Every `memblock_t` carries the return address of whoever
  called `Z_Malloc` (via `__builtin_return_address(0)`, so no macro-wrapping of ~200 call
  sites) plus a monotonic allocation sequence number. Header grows 24 → 32 bytes.
- **`Z_ValidateHeap(const char *when)`.** Walks the block list range-checking every
  pointer *before* dereferencing it, so a corrupt heap produces a report instead of a
  second crash on top of the first. Reports once, then goes quiet, so it is safe to leave
  in a per-frame path. Vanilla's `Z_CheckHeap` cannot do this — it trusts the list it is
  checking and dies on the way to telling you anything.

Called from `D_DoomLoop` after tics and again after display (which half of the frame did
it), and from `P_SetupLevel` before `Z_FreeTags`.

**Reading a report.** If the damaged header is trashed from its first word onwards, the
block in front of it overran its buffer — decode that block's `caller`:

```bash
xtensa-esp32s3-elf-addr2line -pfiaC -e build/esp_doom_rlcd.elf 0x42017bf1
```

If instead only one field is wrong and the rest of the header is intact, nothing overran
anything: a stray pointer wrote four bytes, and the preceding block is innocent. Different
hunt entirely.

Measured cost: none detectable — 27.97 fps with it on, against 27.83 before. **Leave it
on.** The 320×200 constant problem is not finished, and this turns the next instance from
a session of guessing into one flash cycle.

### Driving the board from a script

The menu can be walked over USB-Serial-JTAG without touching the controller, which makes
crash reproduction deterministic and repeatable. `0x1b` is Escape and `\r` is Enter
(`doomgeneric_rlcd.c`), and neither is intercepted by the tuning-key layer. Reset via
pyserial — on USB-Serial-JTAG, RTS drives CHIP_PU and DTR drives GPIO0, so hold DTR low so
it boots the app rather than the ROM loader:

```python
ser.dtr = False; ser.rts = True; time.sleep(0.2); ser.rts = False
```

Then: log for 60 s of demo, send `\x1b`, `\r`, `\r`, `\r` with ~2 s between, keep logging.
That is the whole New Game reproduction.

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

1. **Play it.** The New Game crash is fixed and verified over serial, but "no panic and a
   steady frame rate" is not the same as "the status bar draws correctly and the player
   responds". Look at the panel. This is also the outstanding end-to-end check on the BLE
   controller — pairing works and keys arrive, but nobody has actually played it since the
   input race and lag fixes landed.
2. **`R_MapPlane: degenerate span 374,332 at 255`** still fires once per run, still
   mitigated by skipping, still undiagnosed. Instance #3 above. Now that the heap is clean
   it is the loudest remaining "something still thinks the screen is 320 wide" signal.
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

**The IDF this project actually builds with is `~/.espressif/v5.5.2/esp-idf`, not
`~/esp/esp-idf`** (which on this machine is v5.4). Sourcing the wrong one gives "…
idf5.4_py3.14_env is currently active while the project was configured with …
idf5.5_py3.14_env … Run 'idf.py fullclean' to start again." That is *not* the stale
`IDF_PATH` trap above and `unset IDF_PATH` will not fix it — and do not run `fullclean` as
the message suggests. Source the matching version.

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

**Build the instrument, not the next hypothesis.** The New Game crash had survived a
prime suspect and two "fixed anyway" constants. What killed it was ~150 lines that made
the heap report who damaged it — one build, one flash, one run, and the answer was a
filename and a line number rather than another candidate. Two guesses cost more than the
tool did. When a bug has already survived one hypothesis, stop generating hypotheses.

**A confident handoff is still a hypothesis.** The previous session's prime suspect was
written up with a mechanism, a suggested fix, and a ruled-out list — and it was wrong, in
a way one look at the linker map would have shown. Inherited diagnoses deserve the same
scepticism as fresh ones; the confident prose is not evidence.
