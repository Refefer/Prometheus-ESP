# prometheus-esp32

A generic **Prometheus metrics panel** for the **Waveshare ESP32-S3-Touch-LCD-7**
(plain, 800x480, capacitive touch). Point it at any metrics endpoint, pick the
metrics you care about on the touchscreen, hang it on a wall.

- Reads **raw `/metrics`** text exposition from any exporter, and a
  **Prometheus server's HTTP API** (`/api/v1/query`, `/api/v1/query_range`).
- Renders counters (as rates), gauges, histograms (bucket distribution and
  derived quantiles) and summaries.
- **Configured entirely on the glass** -- WiFi, endpoint URLs, metric
  selection. No companion web page, no serial console.
- Multiple screens with swipe and auto-rotate; settings and selections survive
  a power cut.

Board bring-up (`main/waveshare_rgb_lcd_port.*`, `main/lvgl_port.*`) comes from
[waveshare-ips-esp32](../waveshare-ips-esp32), which extracted it from
[theqkash/esp32flight](https://github.com/theqkash/esp32flight) (MIT).

## Status

| Milestone | |
|---|---|
| M0 scaffold: panel, partitions, storage, parser linked | done |
| M1 exposition parser + host tests | done |
| M2 first real number from a real exporter | done |
| M7 on-device WiFi setup (reordered ahead) | done |

## Build and flash

```sh
. ~/src/esp-idf/export.sh          # ESP-IDF 5.5.x
idf.py set-target esp32s3          # first time only
idf.py build
idf.py -p /dev/ttyACM0 flash
```

The console is the chip's **native USB**, which re-enumerates on reset -- so
anything printed during boot is gone before a host can attach. That is why the
app logs a heartbeat every 5 s instead of relying on the boot log:

```sh
idf.py -p /dev/ttyACM0 monitor     # needs a real TTY
```

## Host tests

The `components/prom/` component has **no ESP-IDF dependencies on purpose**, so
the exposition parser, the rate math and the quantile math build and run on a
development machine. The format has enough edge cases -- escaped label values,
`+Inf` bucket bounds, counter resets, chunk boundaries landing mid-token --
that debugging them by flashing a board and squinting at a 7" panel would
dominate the schedule.

```sh
make -C components/prom/test        # parser, identity, rate and quantile math
make -C components/prom/test asan   # the same under ASan + UBSan

make -C host                        # ui_fmt (formatting, unit inference)
make -C host asan
```

The headline test is **replay equivalence**: parsing a corpus file as one
buffer must produce a byte-identical event stream to parsing it one byte at a
time, and again in 7-byte chunks. A real HTTP body arrives in arbitrary chunks,
and a token split across two of them is the failure nobody reproduces by hand.

`host/` covers the pure-C parts of `main/`. Two invariants there are worth
knowing about, because both fail silently on a real panel rather than loudly:

- **Prefix hysteresis.** A value wobbling around 1000 must not flap between
  `999` and `1.00k`. Each flap is a repaint, and it reads as the *number*
  being unstable rather than the display.
- **The digits-only font charset.** The large display faces carry only
  `" !%+,-./0-9:"`. A glyph missing from an LVGL font draws as *nothing*, so a
  formatter that put a letter in the numeric field would make the value vanish
  from the tile. The test sweeps 112 value/mode combinations to hold it.

To add a real capture to the corpus:

```sh
curl -s localhost:9100/metrics > components/prom/test/corpus/node_exporter.txt
```

## Test exporter

`tools/fake_exporter.py` serves live-ish data -- counters that actually climb,
a gauge that wanders, a histogram whose buckets fill -- so `rate()` and
`histogram_quantile()` have something real to chew on. More usefully, it
injects the failure modes a real exporter will not produce on demand:

```sh
python3 tools/fake_exporter.py --port 9100

curl 'localhost:9100/mode?set=huge'   # 470KB, 261 distinct families
curl 'localhost:9100/mode?set=dup'    # the same series repeated 40x
curl 'localhost:9100/mode?set=html'   # a web page, not metrics
curl 'localhost:9100/mode?set=chunked'
curl localhost:9100/reset             # process-restart counter semantics
curl localhost:9100/down              # start refusing, /up to resume
```

Verified on hardware against it:

| | |
|---|---|
| 474KB body, 5481 samples | SRAM and PSRAM **unchanged** vs a 1.8KB body |
| counter at 1.2e13 | 8.01 MiB/s -- a float32 delta here is exactly zero |
| histogram p99 | 100 ms, matching the hand calculation |
| endpoint killed | classified "connection failed", backs off, recovers |
| gap in scrapes | rate re-baselines instead of averaging across the gap |
| 40 duplicate buckets | quantile stays correct |

## Memory baseline

Measured on-device at M0 (`idf.py monitor`, the heartbeat line):

```
SRAM 301K   PSRAM 6166K free (largest block 6016K)
```

Of the 8 MB of PSRAM, ~2 MB is already committed: two 800x480x2 framebuffers
(1.5 MB) plus the app's `.text`/`.rodata`, which live in PSRAM because of
`CONFIG_SPIRAM_FETCH_INSTRUCTIONS` / `CONFIG_SPIRAM_RODATA`. The planned series
store (384 series, 192 history rings of 480 points) needs ~900 KB, so there is
comfortable headroom -- but **watch this number as the binary grows**, since
`.text` in PSRAM scales with it.

**Internal SRAM is the scarce resource, not PSRAM.** FreeRTOS task stacks
cannot live in PSRAM, and `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096` sends
every allocation under 4 KB to the internal heap. Every data-layer allocation
must therefore call `heap_caps_malloc(n, MALLOC_CAP_SPIRAM)` explicitly rather
than bare `malloc`.

## Layout

```
components/prom/     the parser, identity and math -- host-testable, no IDF deps
  include/           prom_types.h prom_text.h prom_ident.h prom_math.h
  test/              host harness + corpus
main/                the application and the board port
partitions.csv       16 MB: dual OTA slots + config/data LittleFS
```

## Things to experiment with

- `LVGL_PORT_AVOID_TEAR_ENABLE` in `main/lvgl_port.h` -- the buffer-strategy
  knob: two PSRAM framebuffers with vsync flips (no tearing, +750 KB PSRAM) vs
  one framebuffer plus a small SRAM draw buffer (cheap, can tear).
- `EXAMPLE_LCD_PIXEL_CLOCK_HZ` in `main/waveshare_rgb_lcd_port.h` -- scanout
  bandwidth: 16 MHz (~31 Hz refresh, headroom) vs 21 MHz (~41 Hz, tighter).
- `CONFIG_SPIRAM_FETCH_INSTRUCTIONS` / `CONFIG_SPIRAM_RODATA` in
  `sdkconfig.defaults` -- the PSRAM-vs-internal-SRAM trade. Turning them off
  frees PSRAM and costs internal SRAM; the heartbeat readout shows both.
- `PROM_LINE_MAX` in `components/prom/include/prom_types.h` -- 4 KB is 2x the
  worst realistic exposition line. Lower it and watch `err_too_long` climb on a
  cAdvisor corpus.

## Hardware notes

- Backlight is a plain **on/off** line on the CH422G expander (EXIO2); there is
  no PWM pin and no PWM peripheral. Hence **themes rather than a brightness
  slider**, plus a true blackout schedule.
- **GPIO0 is wired as RGB `DATA6`**, so the BOOT button cannot be read at
  runtime. There is no hardware escape hatch -- Safe Mode is entered by holding
  the glass for 10 s within 20 s of boot.
