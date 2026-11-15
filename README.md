# esphome-rtl433

Get 433 MHz and 915 MHz wireless sensors into Home Assistant with one small board and a USB radio dongle. The author uses it for Acurite fridge and freezer thermometers and for water and gas meters; it can listen for any device that [rtl_433](https://github.com/merbanan/rtl_433) knows how to decode.

Normally rtl_433 needs a Linux computer with the dongle plugged into it. This [ESPHome](https://esphome.io) component runs rtl_433 on an ESP32-P4 board instead, and the Home Assistant [rtl_433 integration](https://github.com/rtl-433-hass/rtl_433) connects to the board directly. You do not need a Linux box or an MQTT broker.

## What you need

- An ESP32-P4 board with a high-speed USB host port and PSRAM. The one this was built on is the Waveshare ESP32-P4-WIFI6-POE-ETH (clones work too), which has a USB-A socket, Ethernet and PoE. The ESP32-P4-ETH also works, through its 4-pin USB header. Other ESP32 chips will not work, and the config check refuses them.
- An RTL-SDR USB dongle. The Nooelec NESDR SMArt v5 and the RTL-SDR Blog V3 are both tested. The RTL-SDR Blog V4 is supported by the USB driver but has not been tried with this project.
- Home Assistant with the [rtl_433 integration](https://github.com/rtl-433-hass/rtl_433) installed, and ESPHome to build the firmware.

Sensors on 433.92 MHz and 915 MHz have been received with real devices. 315, 345 and 868 MHz should work the same way but are untested.

## Wiring

On the Waveshare ESP32-P4-WIFI6-POE-ETH there is nothing to wire. Plug the dongle into the USB-A socket and plug in an Ethernet cable. A PoE switch powers the board over that cable.

On the ESP32-P4-ETH, the dongle connects to the board's 4-pin USB header, so you need a USB socket wired to it. Check the board's own documentation for the pin order.

## Configuration

This is a complete config for the Waveshare board, listening on 433.92 MHz. Copy it into a new ESPHome device and put the two secrets in your `secrets.yaml`.

```yaml
esphome:
  name: rtl433
  friendly_name: RTL433

esp32:
  variant: esp32p4
  engineering_sample: true   # for chip revision v1.x; remove this line on v3.x
  flash_size: 16MB
  framework:
    type: esp-idf

psram:                       # required
  mode: hex
  speed: 200MHz

logger:
  level: INFO
  hardware_uart: UART0

api:
  encryption:
    key: !secret rtl433_api_key
  reboot_timeout: 300s

ota:
  - platform: esphome
    password: !secret rtl433_ota_password

time:                        # required
  - platform: homeassistant

ethernet:                    # ESP32-P4-WIFI6-POE-ETH and ESP32-P4-ETH
  type: IP101
  mdc_pin: GPIO31
  mdio_pin: GPIO52
  power_pin: GPIO51
  clk:
    mode: CLK_EXT_IN
    pin: GPIO50
  phy_addr: 1

external_components:
  - source: github://davidcoulson/esphome-rtl433@v0.1.17
    components: [rtl_433]

rtl_433:
  frequencies:
    - 433.92MHz
  remote_control: false      # see Security below
```

Two things to check before you build:

- The chip revision. Run `esptool chip_id` with the board connected. Revision v1.x needs `engineering_sample: true`, and v3.x must not have it. Firmware built for one will not run on the other.
- The `psram:` and `time:` blocks. The component refuses to build without them. Every reading carries a timestamp that the Home Assistant integration compares with its own clock, so the board has to know the time.

The [full example](examples/p4-wifi6-poe-eth.yaml) is the config running in the author's house, with two bands and the diagnostic sensors.

## Flashing

Install it the way you install any ESPHome device: over USB the first time, and over the network after that.

## Adding it to Home Assistant

The board shows up as a normal ESPHome device. The radio sensors come through the rtl_433 integration, which you point at the board:

1. Go to Settings → Devices & services → rtl_433 and add a hub.
2. Enter the board's IP address, port `8433` and path `/ws`.
3. Open the hub's "Add discovered devices". Each sensor is listed there once the board has heard it transmit.

If you are moving from an existing rtl_433 hub, copy its "Device mappings" to the new hub first (they hold things like the unit conversion on utility meters), then add the same devices. The entity ids come back the same.

## Common options

All of these go under `rtl_433:`.

| Option | What it does |
|---|---|
| `frequencies` | The bands to listen on. With more than one, the radio takes turns on each. A band can set its own `sample_rate`, `hop_interval` (time spent on it) and `decoders`. |
| `decoders` | Listen only for these device types, for example `[acurite_txr, acurite_606]`. That means fewer false readings and less work for the board. Names come from [`rtl_433_devices.h`](idf/rtl433_core/include/rtl_433_devices.h). Leave it out to use rtl_433's default set. |
| `gain` | Tuner gain in dB. Leave it out for automatic gain, which is recommended. |
| `units` | `si` (the default), `customary` or `native`. |
| `remote_control` | `false` makes the board's API read-only. See Security below. |
| `detector:` `auto_level` | Follows the background noise level so weak signals are not missed. |
| `detector:` `squelch` | Skips stretches with no signal. Roughly halves the processor load on a 915 MHz band. |

A two-band setup looks like this:

```yaml
rtl_433:
  frequencies:
    - frequency: 433.92MHz
      sample_rate: 250k
      hop_interval: 30s
      decoders: [acurite_txr, acurite_606]
    - frequency: 915MHz
      sample_rate: 2048k
      hop_interval: 150s
      decoders: [scmplus]
  remote_control: false
  detector:
    auto_level: true
    squelch: true
```

You can also add diagnostic sensors such as `decoded_events` and `dropped_samples`. The [technical notes](docs/TECHNICAL.md#all-options) list every option.

## Troubleshooting

- The config will not validate. The error names what is missing: a `psram:` block, a `time:` block, or an ESP32-P4 board. The component also cannot be used together with ESPHome's `usb_host` component.
- The board will not boot after flashing. Check the chip revision and the `engineering_sample` line.
- Nothing shows up in Home Assistant. The radio does not start until the board has the time, which can take up to two minutes after boot. After that, a sensor only appears once it has transmitted. If you set `decoders`, check that your sensor's type is in the list.
- You want to know if the radio hears anything at all. Open `http://<board-ip>:8433/cmd?cmd=get_stats` in a browser. It shows whether signals are being detected and decoded.
- The log does not show the dongle being detected. Those lines are printed before the network is up, so the ESPHome dashboard can miss them. Use a serial console.
- `dropped_samples` keeps climbing. The board is not keeping up. Turn on `squelch`, or raise `usb_buffer` (up to `1MB`).
- Do not pass `-A` in `extra_args`. It crashes the decoder.

There is more in the [debugging notes](docs/TECHNICAL.md#debugging-notes) and [known issues](docs/TECHNICAL.md#known-issues).

## Security

The API on port 8433 has no password, the same as rtl_433 on a PC. Anyone who can reach it can read every sensor reading, and can retune the radio unless you set `remote_control: false`. Keep the board on a network you trust, and leave `remote_control` off unless you use the integration's radio controls. The [technical notes](docs/TECHNICAL.md#security) have the details.

## Technical details

[docs/TECHNICAL.md](docs/TECHNICAL.md) covers how it works inside, the full option reference, measured CPU and memory use, changes made to rtl_433, known issues, and testing.

## License

rtl_433 is GPL-2.0-or-later and esp_rtl_sdr is AGPL-3.0-only, so firmware built with this component is AGPL-3.0. The component's own code is under the same terms.
