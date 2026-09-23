# esphome-rtl433

An [ESPHome](https://esphome.io) external component that runs **[rtl_433](https://github.com/merbanan/rtl_433) on an ESP32-P4**, with an RTL-SDR dongle on the P4's high-speed USB host port. It serves rtl_433's own HTTP/WebSocket API, so the Home Assistant [rtl_433 integration](https://github.com/rtl-433-hass/rtl_433) connects to it exactly as it would to rtl_433 on a Linux box.

> **Status: experimental — builds and has been through a pre-boot review (stack budget, buffer sizes, USB restart path), not yet run on hardware.**

## How it fits together

```
RTL-SDR dongle ──USB 2.0 HS──► esp_rtl_sdr (USB host driver, CU8 I/Q)
                                   │  "esp" input backend in rtl_433's sdr.c
                                   ▼
                          rtl_433 (all decoders) ──► HTTP/WebSocket :8433 ◄── HA rtl_433 integration
```

- `idf/rtl433_core/`: rtl_433 (pinned upstream commit in `UPSTREAM`) built as an ESP-IDF component. The changes to upstream are in `rtl_433-esp.patch`: an `esp` input backend in `src/sdr.c` and a log hook in `src/logger.c`. Everything else is shimmed from `port/` (`exit()` ends the task instead of rebooting, no TTY, a few POSIX calls lwIP/picolibc lack).
- [`esp_rtl_sdr`](https://github.com/hardcoreerik/esp-rtl-sdr) (pinned tag) is fetched as an IDF component. It is clean-room, experimental, and treats the Nooelec NESDR SMArt v5 as provisional.
- `components/rtl_433/`: the ESPHome component. It starts rtl_433 in its own task (64 KB stack) once the network is up, routes its log into the ESPHome logger, and flags the component as errored if rtl_433 ever exits.

Changes to upstream worth knowing about: the demodulator's four internal buffers are sized to 256 K samples instead of 4 M (36 MB, which no ESP32 has); `-D restart` really restarts (upstream exits after the first watchdog restart); the USB driver stays installed for the life of the firmware, so a replugged dongle is picked up by the driver's own rescan; a missing dongle at boot is retried instead of fatal.

## Hardware

- ESP32-P4 with the USB 2.0 High-Speed OTG port brought out (e.g. Waveshare ESP32-P4-WIFI6-POE-ETH, or ESP32-P4-ETH via its 4-pin USB header) and PSRAM.
- RTL2832U dongle supported by esp_rtl_sdr: RTL-SDR Blog V4, or (provisional) Blog V3 / Nooelec NESDR SMArt v5.

## Usage

```yaml
psram:
  mode: hex
  speed: 200MHz

time:                     # required: events carry a timestamp the HA integration checks against its clock
  - platform: sntp

external_components:
  - source: github://davidcoulson/esphome-rtl433@main
    components: [rtl_433]

rtl_433:
  hop_interval: 60s                  # default for every band
  sample_rate: 250k                  # default: 225k-300k or 900k-3.2M S/s
  units: si                          # rtl_433 -C: native, si (default) or customary
  # decoders: [...]                  # default decoder set for every band; omitted = rtl_433's defaults
  frequencies:                       # more than one: rtl_433 hops between them
    - frequency: 433.92MHz
      decoders: [acurite_txr, acurite_606]   # only these while on this band
    - frequency: 915MHz
      sample_rate: 2048k
      hop_interval: 30s
      decoders: [scmplus]
  # gain: 40                         # dB; omit for automatic
  # ppm_error: 0
  # extra_args: ["-M", "noise"]      # anything else rtl_433 accepts
```

Then add an rtl_433 hub in Home Assistant pointing at `<device>:8433`, path `/ws`.

Per-band sample rates are an addition to rtl_433 (upstream uses one rate for every hop): each `-s` pairs with the `-f` in the same position, as `-H` already does. The backend restarts the USB stream for a rate change, which takes a moment at each hop.

### All options

| Key | rtl_433 | Default | Meaning |
|---|---|---|---|
| `port` | `-F http` | `8433` | HTTP/WebSocket API for the HA integration |
| `frequencies` | `-f` | `433.92MHz` | One or more bands; each may set `sample_rate`, `hop_interval`, `decoders` |
| `sample_rate` | `-s` | `250k` | 225k-300k or 900k-3.2M S/s |
| `hop_interval` | `-H` | `600s` | Time on each band |
| `hop_on_event` | `-E hop` | `false` | Move on as soon as something decodes |
| `gain` | `-g` | auto | Tuner gain, dB |
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
| `extra_args` | anything | | Passed through verbatim |

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

## License

rtl_433 is GPL-2.0-or-later and esp_rtl_sdr is AGPL-3.0-only, so firmware built with this component is AGPL-3.0. The component's own code is under the same terms.
