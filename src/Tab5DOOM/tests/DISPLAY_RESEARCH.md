# Tab5 ST7121 display investigation — 2026-09-16

## Evidence on this device

The device reports ST712x touch FW 1 (ST7121). PSRAM bandwidth changes removed
the blue underrun symptom, but not the remaining brightness flicker. Neither
CPU scaling nor full-refresh double buffering eliminated it. Both immutable
STATIC REDRAW and STATIC HOLD patterns also flickered. The LCD reset electrical
configuration correction passed register readback but did not remove flicker.

Changing only ST7121's requested DPI clock from 60 to 70 MHz produced the first
clear visual improvement: the user reported a much brighter image and only
weak residual flicker in both static modes. The user also confirmed that the
residual flicker is visible directly to the eye, not just through a camera.
The 25-second log
`build/serial-20260916-141015-884709.log` confirms requested DPI=70, lane=965,
ST7121 detection and zero underruns; normal game output was 7.85–8.64 FPS.
This is not a same-scene performance benchmark. The improved application is
preserved as `build/m5doom-st7121-70mhz-965mbps.bin` (1,309,808 bytes).

STATIC HOLD stops application-triggered refreshes, not panel scanout or the
background Doom simulation. It strongly narrows the display-content hypothesis
but is not equivalent to a stand-alone LCD demo with no game running.

## Source comparisons (not additional on-device demo tests)

| Reference | ST7121 requested DPI | DSI lane | Relevant implementation / evidence |
| --- | --- | --- | --- |
| Espressif esp-bsp PR #804 + `examples/display` | 70 MHz | 965 Mbps | Reuses ST7123 component with ST7121 vendor table. Contributor reports physical ST7121/ST7123 animation and touch testing with IDF 5.4.4, no observed flicker. |
| M5Stack UserDemo | 70 MHz | 965 Mbps | Independent ST7121 wrapper; includes the LCD_RST input-release correction. Full hardware-evaluation demo. |
| M5GFX | 70 MHz | 900 Mbps | Independent `Panel_ST7121` over shared `Panel_DSI`; two DPI framebuffers. ST7121 payloads and sleep/display-on delays follow the same vendor sequence. |
| ESPP Tab5 example | 70 MHz | 900 Mbps | Independent ST7121 class, explicitly based on M5GFX; one scan framebuffer in the inspected path. Full LVGL/device demo, not independent evidence of panel timing correctness. |

Pinned sources inspected:

- [esp-bsp merged PR](https://github.com/espressif/esp-bsp/pull/804), merge
  `5518e88c4bc7261470f8aae4e4f323259d379ef0`, merged 2026-08-31.
  [Physical test report](https://github.com/espressif/esp-bsp/pull/804#issuecomment-5288906786).
  [Minimal display demo](https://github.com/espressif/esp-bsp/tree/5518e88c4bc7261470f8aae4e4f323259d379ef0/examples/display).
- [M5Stack UserDemo BSP](https://github.com/m5stack/M5Tab5-UserDemo/blob/b4e356bc491ca070d54004718dad789c07d5fc93/platforms/tab5/components/m5stack_tab5/m5stack_tab5.c).
- [M5GFX board profiles](https://github.com/m5stack/M5GFX/blob/641944bd5b123e42b6f4379ff16768e292e203d1/src/M5GFX.cpp),
  [panel sequence](https://github.com/m5stack/M5GFX/blob/641944bd5b123e42b6f4379ff16768e292e203d1/src/lgfx/v1/platforms/esp32p4/Panel_ST7121.hpp),
  [DSI implementation](https://github.com/m5stack/M5GFX/blob/641944bd5b123e42b6f4379ff16768e292e203d1/src/lgfx/v1/platforms/esp32p4/Panel_DSI.cpp).
- [ESPP video initialization](https://github.com/esp-cpp/espp/blob/d3501be6fbe2fbe3585fc768b769a64e57de02c8/components/m5stack-tab5/src/video.cpp),
  [demo](https://github.com/esp-cpp/espp/tree/d3501be6fbe2fbe3585fc768b769a64e57de02c8/components/m5stack-tab5/example).

The complete local ST7121 initialization table was compared against PR #804's
final head `1fa5a0ce41b5deed76ebbea4415c803bd5083bc0` via GitHub's content API:
identical after whitespace/comment removal (2,867 normalized characters).
Simply copying that table again is not a new fix. A driver's name alone does
not determine compatibility: the vendor table, timing, reset, transport and
initialization order all matter. Do not treat the older checked-in UserDemo
snapshot (which had a local 60 MHz setting) as a pristine upstream reference.

## Important clock qualification

In local IDF 5.5.1, `mipi_dsi_hal_host_dpi_calculate_divider()` uses integer
division: `div = source / requested; actual = source / div`. The P4 default
DPI source is PLL_F240M. Thus requested 60 implies 60 MHz, while requested 70
implies **80 MHz**, not exactly 70 MHz. IDF v5.4.4's source uses the same divider
algorithm. This is a source-derived calculation, not an oscilloscope reading.
Boot messages deliberately say **requested DPI**. Do not confuse panel scan
frequency with the game's 8 FPS presentation rate.

References: [IDF 5.5.1 HAL](https://github.com/espressif/esp-idf/blob/v5.5.1/components/hal/mipi_dsi_hal.c),
[P4 clock sources](https://github.com/espressif/esp-idf/blob/v5.5.1/components/soc/esp32p4/include/soc/clk_tree_defs.h),
[IDF 5.4.4 HAL](https://github.com/espressif/esp-idf/blob/v5.4.4/components/hal/mipi_dsi_hal.c).

## Controlled 900 Mbps comparison

Keep the improved requested DPI=70, CPU scaler, reset correction, full refresh,
two panel buffers, PSRAM=200 MHz, and backlight unchanged. Change **only ST7121**
lane rate 965 -> 900 Mbps, matching the M5GFX/ESPP profile. ST7123 stays at the
project's prior 60/965 profile; this device cannot validate other panel models.

This 1,309,792-byte image was built successfully with IDF 5.5.1 and flashed to
COM7 on 2026-09-16; every write passed hash verification. The 25-second capture
`build/serial-20260916-141855-151001.log` confirms ST7121, requested DPI=70 MHz,
DSI lane=900 Mbps, correct LCD_RST readback and zero LCD underruns. Normal-game
statistics were 7.81–8.64 FPS. The pre-existing unsupported `swap_xy` message
does not establish a new regression. Visual comparison with 70/965 is pending;
zero underruns alone is not evidence that residual flicker disappeared.

If residual flicker persists (already confirmed visible directly at 70/965),
test a stand-alone no-Doom/no-LVGL scanout pattern before a broad
library replacement. When trying esp-bsp's shared ST7123 implementation, account
for the old local `m5stack_tab5/esp_lcd_st7123.c` as well as the managed component;
renaming the constructor alone does not guarantee using the tested component.
Only consider an IDF-version A/B after panel configuration is held fixed.
