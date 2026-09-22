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
  frequencies: [433.92MHz, 915MHz]   # more than one: rtl_433 hops between them
  hop_interval: 60s
  sample_rate: 1024000               # 225k-300k or 900k-3.2M S/s
  # gain: 40                         # dB; omit for automatic
  # ppm_error: 0
  # extra_args: ["-R", "40"]         # anything else rtl_433 accepts
```

Then add an rtl_433 hub in Home Assistant pointing at `<device>:8433`, path `/ws`.

## License

rtl_433 is GPL-2.0-or-later and esp_rtl_sdr is AGPL-3.0-only, so firmware built with this component is AGPL-3.0. The component's own code is under the same terms.
