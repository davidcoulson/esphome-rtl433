# esphome-rtl433

An [ESPHome](https://esphome.io) external component that runs **[rtl_433](https://github.com/merbanan/rtl_433) on an ESP32-P4**, with an RTL-SDR dongle on the P4's high-speed USB host port. It serves rtl_433's own HTTP/WebSocket API, so the Home Assistant [rtl_433 integration](https://github.com/rtl-433-hass/rtl_433) connects to it exactly as it would to rtl_433 on a Linux box.

> **Status: experimental — builds, not yet run on hardware.**

## How it fits together

```
RTL-SDR dongle ──USB 2.0 HS──► esp_rtl_sdr (USB host driver, CU8 I/Q)
                                   │  "esp" input backend in rtl_433's sdr.c
                                   ▼
                          rtl_433 (all decoders) ──► HTTP/WebSocket :8433 ◄── HA rtl_433 integration
```

- `idf/rtl433_core/`: rtl_433 (pinned upstream commit in `UPSTREAM`) built as an ESP-IDF component. The changes to upstream are in `rtl_433-esp.patch`: an `esp` input backend in `src/sdr.c` and a log hook in `src/logger.c`. Everything else is shimmed from `port/` (`exit()` ends the task instead of rebooting, no TTY, a few POSIX calls lwIP/picolibc lack).
- [`esp_rtl_sdr`](https://github.com/hardcoreerik/esp-rtl-sdr) (pinned tag) is fetched as an IDF component. It is clean-room, experimental, and treats the Nooelec NESDR SMArt v5 as provisional.
- `components/rtl_433/`: the ESPHome component. It starts rtl_433 in its own task once the network is up and routes its log into the ESPHome logger.

## Hardware

- ESP32-P4 with the USB 2.0 High-Speed OTG port brought out (e.g. Waveshare ESP32-P4-WIFI6-POE-ETH, or ESP32-P4-ETH via its 4-pin USB header) and PSRAM.
- RTL2832U dongle supported by esp_rtl_sdr: RTL-SDR Blog V4, or (provisional) Blog V3 / Nooelec NESDR SMArt v5.

## Usage

```yaml
psram:
  mode: hex
  speed: 200MHz

time:                     # events carry a timestamp; the HA integration drops ones that look stale
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

### Decoders per band

`decoders` takes rtl_433's decoder names, as declared in [`rtl_433_devices.h`](idf/rtl433_core/include/rtl_433_devices.h) (e.g. `scmplus`, `acurite_txr`, `acurite_606`), or protocol numbers as for `-R`. Unknown names are rejected when the config is validated. The set is swapped on every hop, so each band runs only the decoders it needs: fewer false decodes, less work per pulse. A band without a list uses the top-level `decoders`, or rtl_433's defaults.

### Managing the radio from Home Assistant

The integration's SDR controls (frequency, sample rate, gain, ppm, hop interval) work as they do against rtl_433 on a PC: they go through rtl_433's `/cmd` endpoint to the dongle, so automations can retune it. As upstream, setting a frequency this way stops hopping and stays on that frequency until the next reboot, which restores the YAML configuration. While tuned manually, every band's decoders are active (or rtl_433's defaults, if any band uses them), since the new frequency may not be one of the bands.

## License

rtl_433 is GPL-2.0-or-later and esp_rtl_sdr is AGPL-3.0-only, so firmware built with this component is AGPL-3.0. The component's own code is under the same terms.
