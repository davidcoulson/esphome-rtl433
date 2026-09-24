# esphome-rtl433

An [ESPHome](https://esphome.io) external component that runs **[rtl_433](https://github.com/merbanan/rtl_433) on an ESP32-P4**, with an RTL-SDR dongle on the P4's high-speed USB host port. It serves rtl_433's own HTTP/WebSocket API, so the Home Assistant [rtl_433 integration](https://github.com/rtl-433-hass/rtl_433) connects to it exactly as it would to rtl_433 on a Linux box. No Linux box, no rtl_tcp, no MQTT bridge: the whole thing is one PoE-powered board with a dongle in it.

> **Status (2026-09-24): working on real hardware.** One ESP32-P4-WIFI6-POE-ETH with a Nooelec NESDR SMArt v5 has replaced a two-dongle Linux rtl_433 install in the author's house: it hops between 433.92 MHz (Acurite fridge/freezer sensors) and 915 MHz (SCMplus water and gas meters), and the Home Assistant integration is pointed straight at it. Tested for about a day, not months; the [example config](examples/p4-wifi6-poe-eth.yaml) is the one that is running. See [Known issues](#known-issues) before buying a dongle.

## How it fits together

```
RTL-SDR dongle ──USB 2.0 HS──► esp_rtl_sdr (USB host driver, CU8 I/Q)
                                   │  "esp" input backend in rtl_433's sdr.c
                                   ▼
                          rtl_433 (all decoders) ──► HTTP/WebSocket :8433 ◄── HA rtl_433 integration
```

- `idf/rtl433_core/`: rtl_433 (pinned upstream commit in [`UPSTREAM`](idf/rtl433_core/UPSTREAM)) built as an ESP-IDF component. The changes to upstream are in [`rtl_433-esp.patch`](idf/rtl433_core/rtl_433-esp.patch): an `esp` input backend in `src/sdr.c`, a log hook in `src/logger.c`, and the fixes listed below. Everything else is shimmed from `port/` (`exit()` ends the task instead of rebooting, no TTY, a few POSIX calls lwIP/picolibc lack).
- [`esp_rtl_sdr`](https://github.com/hardcoreerik/esp-rtl-sdr) is the USB host driver for the dongle, fetched as an IDF component. It is clean-room and experimental. **This component currently pins a [fork](https://github.com/davidcoulson/esp-rtl-sdr/tree/nooelec-gain-cap-test)** carrying the fixes in [Known issues](#known-issues) until they land upstream.
- `components/rtl_433/`: the ESPHome component. It starts rtl_433 in its own task (64 KB stack) once the network is up and the clock is set, routes its log into the ESPHome logger, and flags the component as errored if rtl_433 ever exits.

Changes to upstream rtl_433 worth knowing about: the demodulator's four internal buffers are sized to 256 K samples instead of 4 M (36 MB, which no ESP32 has); `-D restart` really restarts (upstream exits after the first watchdog restart); the USB driver stays installed for the life of the firmware, so a replugged dongle is picked up by the driver's own rescan; a missing dongle at boot is retried instead of fatal; the SDR setters no longer go through `pthread_self()`, which aborts on ESP-IDF when called from a plain FreeRTOS task; getopt is reset the way newlib needs (upstream's reset printed a stray `invalid option -- '--'`); and the HTTP API can be made read-only (`remote_control: false`).

## Hardware

- **Board:** an ESP32-P4 with the USB 2.0 High-Speed OTG port brought out, plus PSRAM. Verified: Waveshare ESP32-P4-WIFI6-POE-ETH (and its clones): USB-A host port already wired, built-in Ethernet, PoE. The ESP32-P4-ETH works too via its 4-pin USB header. Check your silicon revision with `esptool chip_id`: v1.x needs `engineering_sample: true`, v3.x must not set it.
- **Dongle:** an RTL2832U dongle that esp_rtl_sdr supports.
  - Nooelec NESDR SMArt v5 (R820T2): **verified**, with the driver fork: both bands, manual and automatic gain, hot-plug. Side by side with rtl_433 on Linux and the same dongle model it decodes the same 915 MHz utility meters (see [Known issues](#known-issues) for the fixes that took).
  - RTL-SDR Blog V3 (R820T2): same tuner and code path as the Nooelec, so expected to work; being tested next.
  - RTL-SDR Blog V4 (R828D): esp_rtl_sdr's primary, measured profile; not tested by this project yet.

What it costs at runtime, measured on the P4 at 360 MHz with zero dropped samples:

| Band | Decoding core | USB core |
|---|---|---|
| 915 MHz, 2048 kS/s | 89 % (33 % with `squelch`) | 45 % |
| 433.92 MHz, 250 kS/s | not measured on its own (an eighth of the samples) | |

Free internal heap ~290 KB, PSRAM ~26 MB, decoding task peak stack ~27 KB of 64 KB, chip at ~36 °C in a case. A band hop that changes sample rate takes about a quarter of a second.

## Usage

The short version (the [full example](examples/p4-wifi6-poe-eth.yaml) has the board-specific parts):

```yaml
psram:
  mode: hex
  speed: 200MHz

time:                     # required: events carry a timestamp the HA integration checks against its clock
  - platform: homeassistant

external_components:
  - source: github://davidcoulson/esphome-rtl433@v0.1.5
    components: [rtl_433]

rtl_433:
  frequencies:                       # more than one: rtl_433 hops between them
    - frequency: 433.92MHz
      sample_rate: 250k
      hop_interval: 30s
      decoders: [acurite_txr, acurite_606]   # only these while on this band
    - frequency: 915MHz
      sample_rate: 2048k
      hop_interval: 150s
      decoders: [scmplus]
  gain: 40                           # dB; leave out for the tuner's automatic gain
  remote_control: false              # read-only HTTP API (see Security)
  units: si                          # rtl_433 -C: native, si (default) or customary
  # ppm_error: 0
  # extra_args: ["-M", "noise"]      # anything else rtl_433 accepts
```

Then in Home Assistant: Settings → Devices & services → rtl_433 → add a hub with the device's IP, port `8433`, path `/ws`. Devices appear under the hub's *Add discovered devices* once they have been heard. If you are moving from an existing rtl_433 hub, copy its *Device mappings* over first (they hold things like the ×0.01 / CCF conversion on utility meters), then add the same devices under the new hub; the entity ids come back the same.

Per-band sample rates are an addition to rtl_433 (upstream uses one rate for every hop): each `-s` pairs with the `-f` in the same position, as `-H` already does. The driver changes the rate in place between USB transfers, so mixing rates costs nothing beyond the hop itself.

### All options

| Key | rtl_433 | Default | Meaning |
|---|---|---|---|
| `port` | `-F http` | `8433` | HTTP/WebSocket API for the HA integration |
| `frequencies` | `-f` | `433.92MHz` | One or more bands; each may set `sample_rate`, `hop_interval`, `decoders` |
| `sample_rate` | `-s` | `250k` | 225k-300k or 900k-3.2M S/s (exactly 900k aliases to 300k and is refused) |
| `hop_interval` | `-H` | `600s` | Time on each band |
| `hop_on_event` | `-E hop` | `false` | Move on as soon as something decodes |
| `gain` | `-g` | automatic | Tuner gain, dB. Omit for the tuner's AGC (recommended) |
| `ppm_error` | `-p` | `0` | Frequency correction |
| `bias_tee` / `digital_agc` | `-t` | unchanged | Antenna power / RTL2832 digital AGC |
| `decoders` | (per band) | rtl_433 defaults | Decoder names or numbers |
| `flex_decoders` | `-X` | | General-purpose decoder specs |
| `units` | `-C` | `si` | `native`, `si`, `customary` |
| `detector:` `fsk_detector` | `-Y auto/classic/minmax` | | FSK pulse detector |
| `detector:` `level` / `min_level` / `min_snr` | `-Y level= / minlevel= / minsnr=` | | Detection thresholds, dB |
| `detector:` `auto_level` | `-Y autolevel` | | Track the noise floor (also feeds HA's noise sensor) |
| `detector:` `squelch` | `-Y squelch` | | Skip sample blocks below the noise estimate: halves the decoding CPU on a 2 MS/s band. Use with `extra_args: ["-b", "32768"]` (8 ms blocks); this port keeps one previous block so bursts straddling a block boundary survive |
| `detector:` `level_estimator` | `-Y ampest/magest` | | `amplitude` or `magnitude` |
| `detector:` `fm_filter` | `-Y filter=` | | FM low-pass cutoff |
| `report_noise` | `-M noise:` | | Noise report interval |
| `outputs` | `-F` | | Extra outputs, e.g. `mqtt://host:1883,retain=0` |
| `tags` | `-K` | | Extra fields on every event |
| `verbosity` | `-v` | `0` | rtl_433 log detail, 0-4 |
| `extra_args` | anything | | Passed through verbatim (but not `-A`, see Known issues) |
| `remote_control` | | `true` | `false`: the HTTP API answers queries only; every command that would change the receiver is refused |
| `time_sync_timeout` | | `120s` | Hold rtl_433 back until the clock is set (so the first events don't carry 1970 timestamps), at most this long; `0s` to not wait |

Tuning (ESP32 side):

| Key | Default | Meaning |
|---|---|---|
| `task_core` | the core the ESPHome loop isn't on | Core for rtl_433's decoding task. Its USB acquire thread and the driver's USB task run on the other core. |
| `task_priority` | `5` | Decoding task priority |
| `acquire_priority` | `6` | USB acquire thread priority |
| `usb_task_priority` | `0` (driver default) | esp_rtl_sdr's USB task priority |
| `usb_buffer` | driver default (~192-512 KB) | Sample ring between USB and rtl_433. At 2 MS/s the default rides out only ~50-125 ms of decoding; `1MB` or more if `dropped_samples` climbs. |
| `log_task_stats` | `false` | Log each task's share of each core at every update (debug level) |

The rtl_433 sources are compiled with `-O2` (ESPHome builds everything else for size).

Diagnostic sensors (all optional, published every `update_interval`): `effective_sample_rate`, `usb_overruns`, `dropped_samples` (rtl_433 fell behind), `decoded_events`, `cpu_load_core0`, `cpu_load_core1`, `decode_stack_free` / `acquire_stack_free` (the least free stack each rtl_433 task has ever had), `heap_free`, `psram_free`.

### Decoders per band

`decoders` takes rtl_433's decoder names, as declared in [`rtl_433_devices.h`](idf/rtl433_core/include/rtl_433_devices.h) (e.g. `scmplus`, `acurite_txr`, `acurite_606`), or protocol numbers as for `-R`. Unknown names are rejected when the config is validated. The set is swapped on every hop, so each band runs only the decoders it needs: fewer false decodes, less work per pulse. A band without a list uses the top-level `decoders`, or rtl_433's defaults.

### Managing the radio from Home Assistant

With `remote_control: true` (the default), the integration's SDR controls (frequency, sample rate, gain, ppm, hop interval) work as they do against rtl_433 on a PC: they go through rtl_433's `/cmd` endpoint to the dongle, so automations can retune it. As upstream, setting a frequency this way stops hopping and stays on that frequency until the next reboot, which restores the YAML configuration. While tuned manually, every band's decoders are active (or rtl_433's defaults, if any band uses them), since the new frequency may not be one of the bands. The integration itself (devices, events, WebSocket) works the same with `remote_control: false`; only these controls stop working.

## Security

- **rtl_433's HTTP/WebSocket API has no authentication**, the same as rtl_433 on a PC. Anyone who can reach port 8433 can read every decoded event, and, unless `remote_control: false`, retune or reconfigure the receiver through `/cmd`. Set `remote_control: false` unless you use the integration's SDR controls, and keep the device on a network you trust.
- The web server behind that API is the Mongoose 6.16 that rtl_433 bundles; it parses untrusted network input. This project tracks rtl_433's upstream for updates to it.
- `outputs` and `extra_args` go straight to rtl_433's command line, so they can enable anything rtl_433 can do (MQTT, UDP, file outputs). They only come from your YAML.
- Protect the ESPHome side as usual: API encryption and an OTA password (the example shows both).
- The USB driver checks a new device's descriptors (interface 0, bulk IN endpoint 0x81, sane packet size) before claiming it, and bounds every control transfer.

## Known issues

These are in the USB driver, esp_rtl_sdr v0.8.0-rc3, and reported upstream. The pinned fork fixes them.

- **R820T2 dongles (Blog V3, Nooelec) streamed but never decoded anything** ([esp-rtl-sdr#25](https://github.com/hardcoreerik/esp-rtl-sdr/issues/25)). The driver's R820T2 path replays a USB capture from a Blog V4 and only retunes the PLL per frequency; the tuner's RF mux and tracking filter stayed on the FM band the capture was taken in, so the ADC saw nothing at 433 or 915 MHz. The fork programs the front end for the tuned band (librtlsdr's R820T band table) after every tune.
- **Sample rates of 225k-300k failed with `ESP_RTL_SDR_ERR_BAD_RATE`** ([esp-rtl-sdr#24](https://github.com/hardcoreerik/esp-rtl-sdr/issues/24)): the rate quantizer left out librtlsdr's bit-27-into-bit-28 step, so every low-range rate turned into a bogus 450k-900k one. Fixed in the fork; 250k works again.
- **No manual or automatic gain on the Nooelec profile** (`ESP_RTL_SDR_ERR_UNSUPPORTED`). The fork enables manual gain (the same R820T2 register path as the Blog V3) and adds automatic gain for R820T2 tuners (librtlsdr's recipe: LNA and mixer AGC, VGA 26.5 dB).
- **The 915 MHz band was nearly deaf, and more gain made it worse.** The replayed init leaves the R820T2's IF filter at librtlsdr's 2.2 MHz setting, which librtlsdr pairs with a 1.75 MHz IF, while the driver tuned and demodulated at a 3.57 MHz IF: the wanted signal sat at the edge of the filter and the image 7 MHz away was barely rejected. Harmless on a quiet band, fatal on the 915 MHz ISM band once the AGC amplified the neighbours. The fork sets the IF filter and IF per sample rate exactly as librtlsdr's `r82xx_set_bandwidth()` does, and retunes. Measured side by side with rtl_433 on a Linux box and the same dongle model: from zero utility-meter decodes in ten minutes to half of Linux's count with 80% of the dwell, including neighbours' meters.
- **Automatic gain heard nothing** on R820T2 tuners: the replayed init leaves the LNA's power detectors off, so the AGC had nothing to measure and parked at minimum gain. The fork turns them on as librtlsdr's init does. Auto gain is now the recommended setting; it decodes the 433 MHz sensors at ~15 dB better SNR than a fixed 40 dB.
- **A dongle unplugged mid-stream could crash the board**, and **a dongle whose USB enumeration failed at boot stayed dead** until replugged (about one boot in four here). The fork retires the in-flight transfers on disconnect instead of touching the closing device, and power-cycles the USB port when no device has enumerated for ten seconds.
- The fork also backports four fixes from upstream's in-progress branch: a bulk transfer pool leaked on every stream restart, a halted bulk endpoint killed the stream instead of recovering, the I2C repeater wasn't opened before a hot retune, and the fault guard's timer lived in RTC_NOINIT memory.
- Do not pass `-A` (rtl_433's pulse analyzer) in `extra_args`: it puts two full pulse buffers on the stack and overflows the decoding task the moment the first burst arrives.

## Testing

- **Config tests** (no hardware, no compiler): `python3 tests/config_tests.py` runs a set of YAML cases through ESPHome. Valid ones are generated with `esphome compile --only-generate` and the rtl_433 command line and setters are checked; invalid ones must fail validation with the right message.
- **Hardware smoke test**: `python3 tests/hw_smoke.py <ip> --band 433920000:250000 --band 915000000:2048000 [--read-only] [--expect-events]` checks a running receiver over its API: dongle open, clock set before start, every band visited at its rate, frames detected, WebSocket delivering JSON, and read-only mode if expected.
- **Compile test**: `esphome compile tests/p4-wifi6-poe-eth.yaml` builds every option.
- **Driver**: the fork's host unit tests (`tests/scripts/run_host_tests.sh` in [esp-rtl-sdr](https://github.com/davidcoulson/esp-rtl-sdr/tree/nooelec-gain-cap-test)) check the rate quantizer against librtlsdr's arithmetic, the R820T2 band table, and the dongle profiles.
- **Driver development**: `ESP_RTL_SDR_LOCAL_PATH=/path/to/esp_rtl_sdr esphome compile ...` builds a local driver checkout instead of the pinned one (the directory must be named `esp_rtl_sdr`).

## Debugging notes

The dongle and tuner setup log lines come before Ethernet is up on a cold boot, so the ESPHome dashboard's log viewer can miss them; use a serial console, or restart the device while a log client is attached (rtl_433 waits for the clock, which only arrives once Home Assistant connects). On the P4 the USB-serial/JTAG console can hang after the ROM banner while the USB host stack starts, so a real UART is more reliable. The driver's own log lines are at INFO level and need `CONFIG_LOG_DEFAULT_LEVEL_INFO` in `sdkconfig_options` to appear. rtl_433's `http://<device>:8433/cmd?cmd=get_stats` shows whether frames are being detected and decoded at all; the WebSocket at `/ws` replays recent events on connect; `log_task_stats: true` shows where the CPU goes.

## License

rtl_433 is GPL-2.0-or-later and esp_rtl_sdr is AGPL-3.0-only, so firmware built with this component is AGPL-3.0. The component's own code is under the same terms.
