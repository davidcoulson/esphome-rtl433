# esphome-rtl433

An [ESPHome](https://esphome.io) external component that runs **[rtl_433](https://github.com/merbanan/rtl_433) on an ESP32-P4**, with an RTL-SDR dongle on the P4's high-speed USB host port. It serves rtl_433's own HTTP/WebSocket API, so the Home Assistant [rtl_433 integration](https://github.com/rtl-433-hass/rtl_433) connects to it exactly as it would to rtl_433 on a Linux box. No Linux box, no rtl_tcp, no MQTT bridge: the whole thing is one PoE-powered board with a dongle in it.

> **Status (2026-09-23): working on real hardware.** One ESP32-P4-WIFI6-POE-ETH with a Nooelec NESDR SMArt v5 has replaced a two-dongle Linux rtl_433 install in the author's house: it hops between 433.92 MHz (Acurite fridge/freezer sensors) and 915 MHz (SCMplus water and gas meters), and the Home Assistant integration is pointed straight at it. Tested for hours, not months; the [example config](examples/p4-wifi6-poe-eth.yaml) is the one that is running. See [Known issues](#known-issues) before buying a dongle.

## How it fits together

```
RTL-SDR dongle ──USB 2.0 HS──► esp_rtl_sdr (USB host driver, CU8 I/Q)
                                   │  "esp" input backend in rtl_433's sdr.c
                                   ▼
                          rtl_433 (all decoders) ──► HTTP/WebSocket :8433 ◄── HA rtl_433 integration
```

- `idf/rtl433_core/`: rtl_433 (pinned upstream commit in [`UPSTREAM`](idf/rtl433_core/UPSTREAM)) built as an ESP-IDF component. The changes to upstream are in [`rtl_433-esp.patch`](idf/rtl433_core/rtl_433-esp.patch): an `esp` input backend in `src/sdr.c`, a log hook in `src/logger.c`, and the fixes listed below. Everything else is shimmed from `port/` (`exit()` ends the task instead of rebooting, no TTY, a few POSIX calls lwIP/picolibc lack).
- [`esp_rtl_sdr`](https://github.com/hardcoreerik/esp-rtl-sdr) is the USB host driver for the dongle, fetched as an IDF component. It is clean-room and experimental. **This component currently pins a [fork](https://github.com/davidcoulson/esp-rtl-sdr/tree/nooelec-gain-cap-test)** carrying the fixes in [Known issues](#known-issues) until they land upstream.
- `components/rtl_433/`: the ESPHome component. It starts rtl_433 in its own task (64 KB stack) once the network is up, routes its log into the ESPHome logger, and flags the component as errored if rtl_433 ever exits.

Changes to upstream rtl_433 worth knowing about: the demodulator's four internal buffers are sized to 256 K samples instead of 4 M (36 MB, which no ESP32 has); `-D restart` really restarts (upstream exits after the first watchdog restart); the USB driver stays installed for the life of the firmware, so a replugged dongle is picked up by the driver's own rescan; a missing dongle at boot is retried instead of fatal; and the SDR setters no longer go through `pthread_self()`, which aborts on ESP-IDF when called from a plain FreeRTOS task.

## Hardware

- **Board:** an ESP32-P4 with the USB 2.0 High-Speed OTG port brought out, plus PSRAM. Verified: Waveshare ESP32-P4-WIFI6-POE-ETH (and its clones): USB-A host port already wired, built-in Ethernet, PoE. The ESP32-P4-ETH works too via its 4-pin USB header. Check your silicon revision with `esptool chip_id`: v1.x needs `engineering_sample: true`, v3.x must not set it.
- **Dongle:** an RTL2832U dongle that esp_rtl_sdr supports.
  - Nooelec NESDR SMArt v5 (R820T2): **verified**, with the driver fork.
  - RTL-SDR Blog V3 (R820T2): same tuner and code path as the Nooelec, so expected to work; being tested next.
  - RTL-SDR Blog V4 (R828D): esp_rtl_sdr's primary, measured profile; not tested by this project yet.

What it costs at runtime, measured on the P4 at 360 MHz: about 40 % of one core for decoding and 35 % of the other for USB at 1024 kS/s, 88 % / 73 % at 2048 kS/s, zero dropped samples either way, chip at ~36 °C in a case.

## Usage

The short version (the [full example](examples/p4-wifi6-poe-eth.yaml) has the board-specific parts):

```yaml
psram:
  mode: hex
  speed: 200MHz

time:                     # required: events carry a timestamp the HA integration checks against its clock
  - platform: homeassistant

external_components:
  - source: github://davidcoulson/esphome-rtl433@main
    components: [rtl_433]

rtl_433:
  frequencies:                       # more than one: rtl_433 hops between them
    - frequency: 433.92MHz
      sample_rate: 1024k             # not 250k, see Known issues
      hop_interval: 60s
      decoders: [acurite_txr, acurite_606]   # only these while on this band
    - frequency: 915MHz
      sample_rate: 2048k
      hop_interval: 60s
      decoders: [scmplus]
  gain: 40                           # dB; automatic gain is not available on R820T2 dongles yet
  units: si                          # rtl_433 -C: native, si (default) or customary
  # ppm_error: 0
  # extra_args: ["-M", "noise"]      # anything else rtl_433 accepts
```

Then in Home Assistant: Settings → Devices & services → rtl_433 → add a hub with the device's IP, port `8433`, path `/ws`. Devices appear under the hub's *Add discovered devices* once they have been heard. If you are moving from an existing rtl_433 hub, copy its *Device mappings* over first (they hold things like the ×0.01 / CCF conversion on utility meters), then add the same devices under the new hub; the entity ids come back the same.

Per-band sample rates are an addition to rtl_433 (upstream uses one rate for every hop): each `-s` pairs with the `-f` in the same position, as `-H` already does. The backend restarts the USB stream for a rate change, which takes about a second at each hop.

### All options

| Key | rtl_433 | Default | Meaning |
|---|---|---|---|
| `port` | `-F http` | `8433` | HTTP/WebSocket API for the HA integration |
| `frequencies` | `-f` | `433.92MHz` | One or more bands; each may set `sample_rate`, `hop_interval`, `decoders` |
| `sample_rate` | `-s` | `1024k` | 900k-3.2M S/s (see Known issues for why not 225k-300k) |
| `hop_interval` | `-H` | `600s` | Time on each band |
| `hop_on_event` | `-E hop` | `false` | Move on as soon as something decodes |
| `gain` | `-g` | tuner default | Tuner gain, dB. Set it: auto gain is not implemented for R820T2 dongles |
| `ppm_error` | `-p` | `0` | Frequency correction |
| `bias_tee` / `digital_agc` | `-t` | unchanged | Antenna power / RTL2832 digital AGC |
| `decoders` | (per band) | rtl_433 defaults | Decoder names or numbers |
| `flex_decoders` | `-X` | | General-purpose decoder specs |
| `units` | `-C` | `si` | `native`, `si`, `customary` |
| `detector:` `fsk_detector` | `-Y auto/classic/minmax` | | FSK pulse detector |
| `detector:` `level` / `min_level` / `min_snr` | `-Y level= / minlevel= / minsnr=` | | Detection thresholds, dB |
| `detector:` `auto_level` | `-Y autolevel` | | Track the noise floor (also feeds HA's noise sensor) |
| `detector:` `squelch` | `-Y squelch` | | Skip frames below the noise estimate: saves CPU |
| `detector:` `level_estimator` | `-Y ampest/magest` | | `amplitude` or `magnitude` |
| `detector:` `fm_filter` | `-Y filter=` | | FM low-pass cutoff |
| `report_noise` | `-M noise:` | | Noise report interval |
| `outputs` | `-F` | | Extra outputs, e.g. `mqtt://host:1883,retain=0` |
| `tags` | `-K` | | Extra fields on every event |
| `verbosity` | `-v` | `0` | rtl_433 log detail, 0-4 |
| `extra_args` | anything | | Passed through verbatim (but not `-A`, see Known issues) |

Tuning (ESP32 side):

| Key | Default | Meaning |
|---|---|---|
| `task_core` | the core the ESPHome loop isn't on | Core for rtl_433's decoding task. Its USB acquire thread and the driver's USB task run on the other core. |
| `task_priority` | `5` | Decoding task priority |
| `acquire_priority` | `6` | USB acquire thread priority |
| `usb_task_priority` | `0` (driver default) | esp_rtl_sdr's USB task priority |
| `usb_buffer` | driver default (~192-512 KB) | Sample ring between USB and rtl_433. At 2 MS/s the default rides out only ~50-125 ms of decoding; `1MB` or more if `dropped_samples` climbs. |

The rtl_433 sources are compiled with `-O2` (ESPHome builds everything else for size).

Diagnostic sensors (all optional, published every `update_interval`): `effective_sample_rate`, `usb_overruns`, `dropped_samples` (rtl_433 fell behind), `decoded_events`, `cpu_load_core0`, `cpu_load_core1`.

### Decoders per band

`decoders` takes rtl_433's decoder names, as declared in [`rtl_433_devices.h`](idf/rtl433_core/include/rtl_433_devices.h) (e.g. `scmplus`, `acurite_txr`, `acurite_606`), or protocol numbers as for `-R`. Unknown names are rejected when the config is validated. The set is swapped on every hop, so each band runs only the decoders it needs: fewer false decodes, less work per pulse. A band without a list uses the top-level `decoders`, or rtl_433's defaults.

### Managing the radio from Home Assistant

The integration's SDR controls (frequency, sample rate, gain, ppm, hop interval) work as they do against rtl_433 on a PC: they go through rtl_433's `/cmd` endpoint to the dongle, so automations can retune it. As upstream, setting a frequency this way stops hopping and stays on that frequency until the next reboot, which restores the YAML configuration. While tuned manually, every band's decoders are active (or rtl_433's defaults, if any band uses them), since the new frequency may not be one of the bands.

## Known issues

These are all in the USB driver, esp_rtl_sdr v0.8.0-rc3, and reported upstream. The pinned fork fixes the ones that can be fixed here.

- **R820T2 dongles (Blog V3, Nooelec) streamed but never decoded anything** ([esp-rtl-sdr#25](https://github.com/hardcoreerik/esp-rtl-sdr/issues/25)). The driver's R820T2 path replays a USB capture from a Blog V4 and only retunes the PLL per frequency; the tuner's RF mux and tracking filter stayed on the FM band the capture was taken in, so the ADC saw nothing at 433 or 915 MHz. The fork programs the front-end for the tuned band (librtlsdr's R820T band table) after every tune. Fixed in the fork; verified.
- **Sample rates of 225k-300k fail with `ESP_RTL_SDR_ERR_BAD_RATE`** ([esp-rtl-sdr#24](https://github.com/hardcoreerik/esp-rtl-sdr/issues/24)): the rate quantizer masks off a bit it needs, so every low-range rate lands outside the hardware's windows. The component rejects those rates at validation time until this is fixed; `1024k` works fine for 433 MHz OOK sensors and costs a little more CPU.
- **Manual gain was unavailable on the Nooelec profile** (`ESP_RTL_SDR_ERR_UNSUPPORTED`): the capability flag was simply not set, although the R820T2 gain code path is shared with the Blog V3 profile. Enabled in the fork. Automatic gain is not implemented for the R820T2 family at all, so always set `gain`.
- Do not pass `-A` (rtl_433's pulse analyzer) in `extra_args`: it puts two full pulse buffers on the stack and overflows the decoding task the moment the first burst arrives.

## Debugging notes

The boot log (dongle enumeration, tuner setup, gain) goes by before Ethernet is up, so the ESPHome dashboard's log viewer never shows it; use a serial console. On the P4 the USB-serial/JTAG console can hang after the ROM banner while the USB host stack starts, so a real UART is more reliable. The driver's own log lines are at INFO level and need `CONFIG_LOG_DEFAULT_LEVEL_INFO` in `sdkconfig_options` to appear. rtl_433's `http://<device>:8433/cmd?cmd=get_stats` shows whether frames are being detected and decoded at all; the WebSocket at `/ws` replays recent events on connect.

## License

rtl_433 is GPL-2.0-or-later and esp_rtl_sdr is AGPL-3.0-only, so firmware built with this component is AGPL-3.0. The component's own code is under the same terms.
