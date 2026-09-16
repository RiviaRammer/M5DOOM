# Tab5 host regression tests

## Current Flash-save validation

The former T-key static-image diagnostic has been removed from production.
The display investigations below are historical records. The final 70/900
ST7121 profile was confirmed flicker-free by the user and is unchanged here.

`tests/test_save_io.ps1` extracts the actual `M_WriteFile`/`M_ReadFile` functions
and tests creation, overwrite, round-trip binary data, missing/empty files,
short writes, close failures, failed promotion rollback and short-read cleanup.
Run with a host GCC, using an absolute output path in a temporary directory:

```powershell
./tests/test_save_io.ps1 -Compiler gcc -Output "$env:TEMP/tab5_save_io_test.exe"
```

`test_save_layout.py` checks non-overlapping partitions, the 16 MiB limit,
unchanged WAD placement, full-flash reset policy and removal of T diagnostics.

For an explicitly destructive hardware regression, build with
`idf.py -B build -D TAB5_SAVE_SELFTEST=ON build`. It starts E1M1, saves a real
engine state into slot 8, restarts, loads it and verifies health, armor,
ammo, position and the menu slot name. It then overwrites and reloads the slot.
This opt-in test is not part of normal firmware; it consumes slot 8 and leaves
test data for checking app-flash persistence. It does not simulate physical
power removal or guarantee filesystem recovery from every power-loss timing.

Always finish with `idf.py -B build -D TAB5_SAVE_SELFTEST=OFF build` and a full
`flash`. This removes the automatic test and clears test saves. SPIFFS mount
failure never automatically formats the user's partition. Its flat VFS mount
is selected with `DOOMSAVEDIR`, avoiding PrBoom's desktop `-save` directory test.

Hardware results on 2026-09-16 (IDF 5.5.1, COM7):

- `build/serial-20260916-145009-346631.log`: engine wrote a 33,946-byte save,
  rebooted, restored health 73 / armor 19 / ammo 37 and exact player position;
  the menu read the slot name correctly. Overwrite/load then restored
  health 51 / armor 29 / ammo 47. Both phases logged PASS; zero LCD underruns.
- `build/serial-20260916-145137-641494.log`: app-only reflash retained that save
  and loaded the overwritten state successfully. Both load phases passed.
- Final normal build: `TAB5_SAVE_SELFTEST=OFF`, application 1,340,800 bytes,
  no self-test source/define in compile commands and no T-diagnostic/self-test
  entry-point symbols in the linked ELF. Host save-I/O fault tests, LCD reset
  mock, helper regressions and eight Python tests passed.
- Final full flash passed all write hash checks. The 25-second capture
  `build/serial-20260916-145349-539486.log` reports save filesystem `used=0`,
  confirming the test saves were cleared; requested DPI=70 / lane=900 is
  unchanged, no automatic self-test ran, and zero LCD underruns were observed.
  Normal-game statistics were 7.98–8.78 FPS, not a controlled benchmark.

The initial hardware attempt caught the SPIFFS directory-check mismatch before
release. The successful logs above are from the corrected implementation.

These tests compile the same dependency-free helpers used by the ESP32-P4
firmware. They do not require ESP-IDF or a connected Tab5.

From the repository root, with a C11 compiler on `PATH`:

```powershell
New-Item -ItemType Directory -Force .codex_tmp | Out-Null
gcc -std=c11 -O2 -Wall -Wextra -Werror -I src/Tab5DOOM/components/prboom-esp32-compat/include src/Tab5DOOM/tests/tab5_helpers_test.c -o .codex_tmp/tab5_helpers_test.exe
if ($LASTEXITCODE -eq 0) { & .\.codex_tmp\tab5_helpers_test.exe }
```

Do not define `NDEBUG`: checks use C assertions.

Coverage:

- Physical-key aliases, overlapping actions, repeated presses/releases, invalid
  matrix coordinates, and 10,000 deterministic randomized events.
- Every pixel of the 720x960 RGB565 CPU fallback for three input patterns, with
  output boundary canaries and a buffer requiring only 16-bit alignment.
- 20,000 frame-deadline cases compared with iterative advancement, 32-bit tick
  wraparound (including deadline zero), a long stall, and a simulated 35 Hz game
  loop maintaining the 20 Hz presentation grid.

## Hardware verification still required

Host tests do not execute GPIO/I2C, PPA, LVGL, the display DMA pipeline, or the
PrBoom engine. After an authorized flash, verify:

1. Hold `W` and Up together, release either, then release the other. Repeat with
   `F`, Ctrl, and Enter; movement/fire should stop only after the final alias is
   released.
2. Exercise more than four queued keyboard events, including releases. The
   remaining queue must be processed on later polls even when INT is high.
3. Check touch alignment, palette effects, screen wipes, and gameplay during
   sustained input. Compare the CPU fallback's orientation with PPA output.
4. Capture the five-second `tab5_lcd` statistics on identical scenes. Compare
   `scale` and `refresh` average/maximum timings and memory stability. Host tests
   demonstrate correctness of the helper logic, not an on-device FPS gain.

The 2026-09-06 local validation passed these host tests and the ESP-IDF 5.5.1
`build_codex` firmware build. The resulting application is 1,317,376 bytes;
58% of its 3 MiB partition remains free. The dependency Kconfig warning about
an unset `ESP_IDF_VERSION` environment variable remains. No device was flashed.

## 2026-09-16 display-underrun investigation

The old 80 MHz PSRAM / 128-byte PPA-burst firmware on COM7 produced 514 LCD
underrun messages in a 12-second passive capture. Its statistics showed roughly
28-29 ms scaling and 68 ms refresh time. This is a **pre-change baseline**, not a
measurement of the new build.

The new test configuration uses 200 MHz PSRAM (as in the repository's Tab5
reference demo) and a 32-byte PPA burst. ESP-IDF 5.5.1 marks 200 MHz as
experimental, so `CONFIG_IDF_EXPERIMENTAL_FEATURES=y` is required; the boot PSRAM
memory test remains enabled. L2 stays at 128 KiB: increasing it to 256 KiB would
take another 128 KiB from internal SRAM, but the old firmware had only about
33 KiB free during gameplay. XiP and the panel timings are unchanged.

The [ESP-IDF PPA documentation](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32p4/api-reference/peripherals/ppa.html)
describes the smaller-burst tradeoff: reduced PPA throughput in exchange for
bandwidth opportunities for other peripherals. This is not a guarantee that
32-byte bursts alone eliminate LCD underruns.

Firmware outputs now always go to `src/Tab5DOOM/build`. Confirm the boot log
reports `PSRAM=200MHz L2=128KB PPA_burst=32B`. Then inspect `window`, `fps`,
`scale`, `refresh`, and the underrun count during the same game scene. Neither
a successful host test nor a successful build proves the flicker is fixed.

Local validation on 2026-09-16 passed the C helper regression tests, four Python
parser tests, and both CPU-only and PPA-enabled ESP-IDF 5.5.1 builds. The first
200 MHz single-scan-buffer image was the PPA-enabled variant, 1,317,632 bytes (58% partition space
free). Existing PrBoom compiler warnings remain.

With user approval, this image, its bootloader/partition table, and the bundled
WAD were flashed to COM7 on 2026-09-16. Esptool identified ESP32-P4 revision 1.3
and verified every write. The application ran after reset. Two passive capture
windows (20 seconds and 30 seconds, with a gap between them) recorded zero LCD
underrun messages and zero PPA failures in the reported statistics. Complete
statistics lines showed 11.57-12.35 FPS, approximately 14 ms average scaling and
46 ms average refresh time. Logs are `build/serial-20260916-130234-850764.log`
and `build/serial-20260916-130318-067304.log`.

This is a short post-flash smoke test, not a controlled same-scene benchmark or
a long-duration stability test. Startup output was incomplete, and the display
has not been visually confirmed after flashing. The observed runtime is
consistent with the new image; no claim is made that every possible scene is
now free of flicker.

### Double-scan-buffer comparison

The second user video still shows content/brightness alternation without the
blue underrun screen. Camera exposure/reflections prevent a definitive cause
from video alone. The next test keeps the 200 MHz PSRAM, 32-byte PPA burst,
pixel clock, palette conversion and rotation unchanged, but uses two DPI panel
framebuffers plus LVGL full refresh and frame-boundary handoff. Both local BSP
panel constructors now honor `CONFIG_BSP_LCD_DPI_BUFFER_NUMS` rather than
hard-coding one framebuffer. The LVGL port uses the panel-owned buffers instead
of an independent 20-line scratch buffer. This costs one additional 720x1280x2
byte PSRAM scan buffer (1,843,200 bytes), while removing the 28,800-byte internal
LVGL scratch buffer.

This new comparison image is built to `build/m5doom.bin`; the previously flashed
image is retained as `build/m5doom-single-buffer-200m.bin`. Its expected startup marker is:

```text
Presentation: LCD buffers=2, frame-boundary handoff enabled
```

The double-buffer image (1,317,760 bytes) was flashed to COM7 with user approval
on 2026-09-16; all writes passed esptool hash verification. A subsequent
30-second capture recorded zero LCD underruns and zero reported PPA failures,
but only 8.61-9.18 FPS, with average scaling about 14 ms and average refresh
74.6-79.6 ms. Log: `build/serial-20260916-131347-044317.log`. This is a measurable
performance regression versus the earlier single-buffer runtime samples, not a
speed improvement. The user confirmed that flicker persists, including while
the game is paused. Double buffering did not resolve the visible symptom.

### CPU-only comparison

The current test configuration disables `CONFIG_HW_TAB5_PPA_ENA` in both
`sdkconfig` and `sdkconfig.defaults`. LCD double buffering, full refresh,
200 MHz PSRAM, palette conversion and panel timing remain unchanged. This
isolates the PPA scaling path rather than simultaneously changing scanout.
The CPU helper's pixel mapping is covered by the host regression tests.
The ESP-IDF 5.5.1 build passed on 2026-09-16, producing `build/m5doom.bin`
(1,307,728 bytes). C helper tests and all four Python tests passed; existing
PrBoom compiler warnings remain. The previous PPA/double-buffer application is
backed up as `build/m5doom-double-buffer-ppa-200m.bin`.

With user approval, the CPU variant was flashed to COM7 on 2026-09-16; every
write passed esptool hash verification and the device was reset. A 20-second
passive capture confirmed CPU scaling (`cpu_scale` equaled `frames sent` in
each complete statistics line), zero LCD underrun messages, 8.11-9.58 FPS,
approximately 21.8 ms average scaling and 73.4-78.2 ms average refresh. Log:
`build/serial-20260916-132348-763697.log`. Startup output was incomplete.
The user confirmed that the visible flicker is unchanged with CPU scaling.

### Static-image diagnostic

The next image adds the Tab5 keyboard `T` key to cycle three presentation modes:

1. `TEST 1: STATIC REDRAW`: generate color bars and a gray checkerboard once,
   then repeatedly refresh that same immutable image through LVGL. This removes
   Doom rendering, palette conversion and scaling from the displayed image.
2. `TEST 2: STATIC HOLD`: keep the same image and stop application-triggered
   refreshes after updating the mode label once. The panel continues scanning;
   this does not disable the backlight or panel refresh.
3. `T: display test`: return to normal game presentation.

The game simulation continues in the background. Pause the game with `P` before
the test if desired. Each key press advances once (held keys do not repeat).
Each mode is shown in the black margin and logged as `Display diagnostic`;
statistics include `diag=0/1/2`. In settled HOLD windows `frames sent=0` is
expected, not a rendering failure. Compare both static modes for at least five
seconds and report whether each flickers. Neither a clean build nor zero
underruns confirms a visual fix.

The static diagnostic build passed ESP-IDF 5.5.1 compilation, C helper tests
(including every test-pattern pixel and boundary canaries) and five Python
tests. The 1,308,448-byte `build/m5doom.bin` was flashed to COM7 with every write
hash verified. A 30-second capture recorded zero underruns and 8.08-8.18 FPS
in normal mode (`diag=0`): `build/serial-20260916-132959-967600.log`. No static
mode transition occurred in that capture; keyboard mode switching and visual
results still need on-device confirmation. The prior CPU comparison image is
preserved as `build/m5doom-double-buffer-cpu-200m.bin`.

The user subsequently confirmed that both STATIC REDRAW and STATIC HOLD
still flicker. This shifts investigation toward panel scanout/configuration
and backlight, rather than attributing the symptom to PPA or changing images.

### LCD reset electrical configuration

The current M5Stack upstream BSP releases LCD_RST (expander 0x43, P4) as an
input with pull-up, keeping the output latch low. Its comments explicitly
warn against driving 3.3 V push-pull high into the panel reset input. Our old
copy still drove P4 high in both expander initialization and `bsp_reset_tp`.
Both paths now follow the upstream low-output/input-release sequence, with
I2C error checks on the new reset operations and final register readback.
Pixel clock remained 60 MHz for this comparison; upstream's separate 70 MHz
ST712x timing difference had not yet been applied. CPU scaling, PSRAM, backlight,
panel command tables and double buffering are unchanged.

Source checked on 2026-09-16:
[M5Stack BSP](https://github.com/m5stack/M5Tab5-UserDemo/blob/main/platforms/tab5/components/m5stack_tab5/m5stack_tab5.c).
This is a concrete configuration correction, not proof that it causes all
observed flicker or that hardware has been damaged. Older firmware backups
predate this correction and should not be used as the default rollback.

`tests/test_lcd_reset.ps1` extracts the actual two BSP functions and compiles
them with a mock I2C expander. It checks every intermediate register write
against push-pull-high on P4, the reset delays, final pull-up/input state, and
preservation of unrelated pins over 256 input states. Run with a host GCC:

```powershell
./tests/test_lcd_reset.ps1 -Compiler gcc
```

Expected boot readback: `LCD_RST released as input-pull-up: dir=6f out=66
pull_en=7f pull_sel=7f`. A mismatched readback stops initialization explicitly.

Validation on 2026-09-16: reset-sequence mock, C helpers, five Python tests and
ESP-IDF 5.5.1 build passed (existing BSP warnings remain). The 1,309,664-byte
firmware was flashed to COM7 and all writes were hash-verified. The 25-second
boot/runtime capture `build/serial-20260916-135304-500609.log` contains the
expected `dir=6f out=66 pull_en=7f pull_sel=7f` readback and ST7121 detection,
with zero underrun messages. Normal-mode statistics are 8.13-9.62 FPS.
The user subsequently confirmed that flicker remained; the reset correction
alone did not resolve the visible symptom.

### ST7121 clock and lane-rate comparisons

Changing only the ST7121 requested DPI clock from 60 to 70 MHz substantially
reduced visible flicker according to the user, but weak flicker remained in
both static modes and was visible directly, not only through a camera.
Under IDF 5.5.1's integer divider and default 240 MHz source, the 70 MHz request
calculates to an 80 MHz generated clock; this is not a physical measurement.
The improved 70/965 application is backed up as
`build/m5doom-st7121-70mhz-965mbps.bin`.

The next controlled comparison changes only ST7121's DSI lane rate from 965
to 900 Mbps, matching M5GFX and ESPP. See [DISPLAY_RESEARCH.md](DISPLAY_RESEARCH.md)
for the four upstream comparisons, pinned sources, exact logs and results.
Keep visual results separate from serial underrun counts.

Expected startup markers are `PPA disabled by configuration; using CPU scaler`
and `scaler=CPU`. Runtime statistics should report `cpu_scale` equal to the
number of processed frames. Compare the same paused scene first, then gameplay.
If this version still flickers, PPA is not sufficient to explain the symptom;
continue isolating LVGL/panel presentation and static image output.

After an authorized flash, check menu, static/paused gameplay, moving gameplay,
screen wipes, touch, and serial underruns. A successful build is not a visual
flicker test. If the flicker persists with zero underruns, compare CPU scaling
before attributing the symptom solely to LCD scanout.

For a CPU-only comparison, disable **Use PPA for M5Stack Tab5 scaling** under
**ESP32-Doom platform-specific configuration** in `idf.py menuconfig`, then
rebuild. The option is `CONFIG_HW_TAB5_PPA_ENA`; the current comparison default
is disabled. Re-enable it in both configuration files to restore PPA scaling.
If 200 MHz fails PSRAM initialization or its boot memory test on the device,
restore **80MHz clock speed** under **Component config > PSRAM**, rebuild and
flash the complete firmware before trying further changes.

With the ESP-IDF Python environment active, capture logs without intentionally
toggling DTR/RTS, sending serial data, flashing, or requesting a reboot:

```powershell
cd D:\gits\M5DOOM\src\Tab5DOOM
python tools/capture_display.py --port COM7 --seconds 15
```

Close other serial monitors first. The script closes the port after capture,
writes a timestamped raw log under `build`, and prints the underrun count and
display statistics. As with all serial software, operating-system/USB-driver
behavior on port open cannot be controlled completely.

Run its parser tests from the repository root:

```powershell
python -m unittest discover -s src/Tab5DOOM/tests -p "test_*.py"
```
