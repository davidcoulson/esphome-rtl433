from pathlib import Path

import esphome.codegen as cg
from esphome.components import esp32
from esphome.components.esp32 import (
    VARIANT_ESP32P4,
    add_idf_component,
    add_idf_sdkconfig_option,
    idf_version,
)
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_PORT

DEPENDENCIES = ["network"]
CODEOWNERS = ["@davidcoulson"]

CONF_FREQUENCIES = "frequencies"
CONF_HOP_INTERVAL = "hop_interval"
CONF_SAMPLE_RATE = "sample_rate"
CONF_GAIN = "gain"
CONF_PPM_ERROR = "ppm_error"
CONF_EXTRA_ARGS = "extra_args"

ESP_RTL_SDR_REPO = "https://github.com/hardcoreerik/esp-rtl-sdr.git"
ESP_RTL_SDR_REF = "v0.8.0-rc3"
USB_REF = "1.4.1"  # same espressif/usb ESPHome's usb_host pins for IDF 6

# The ESP-IDF component that wraps the rtl_433 sources lives next to components/ in this repo. It is found
# relative to this file, which works for a local path and for a github:// source alike (ESPHome clones
# the whole repository).
RTL433_CORE = Path(__file__).resolve().parents[2] / "idf" / "rtl433_core"

rtl_433_ns = cg.esphome_ns.namespace("rtl_433")
Rtl433Component = rtl_433_ns.class_("Rtl433Component", cg.Component)


def _only_p4(config):
    if esp32.get_esp32_variant() != VARIANT_ESP32P4:
        raise cv.Invalid("rtl_433 needs an ESP32-P4 (high-speed USB host)")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(Rtl433Component),
            # rtl_433's HTTP/WebSocket server, which the Home Assistant rtl_433 integration connects to
            cv.Optional(CONF_PORT, default=8433): cv.port,
            cv.Optional(CONF_FREQUENCIES, default=["433.92MHz"]): cv.ensure_list(
                cv.frequency
            ),
            cv.Optional(CONF_HOP_INTERVAL, default="600s"): cv.positive_time_period_seconds,
            cv.Optional(CONF_SAMPLE_RATE, default=250000): cv.int_range(
                min=225001, max=3200000
            ),
            # Tuner gain in dB; omitted = automatic
            cv.Optional(CONF_GAIN): cv.float_range(min=0, max=50),
            cv.Optional(CONF_PPM_ERROR, default=0): cv.int_range(min=-200, max=200),
            # Anything else rtl_433 accepts on its command line, e.g. ["-R", "40", "-M", "level"]
            cv.Optional(CONF_EXTRA_ARGS, default=[]): cv.ensure_list(cv.string_strict),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    _only_p4,
)


def _rtl433_args(config):
    args = ["rtl_433", "-d", "esp", "-F", f"http:0.0.0.0:{config[CONF_PORT]}"]
    args += ["-M", "time:iso:usec:tz", "-M", "protocol", "-M", "level"]
    for freq in config[CONF_FREQUENCIES]:
        args += ["-f", str(int(freq))]
    if len(config[CONF_FREQUENCIES]) > 1:
        args += ["-H", str(config[CONF_HOP_INTERVAL].total_seconds)]
    args += ["-s", str(config[CONF_SAMPLE_RATE])]
    if CONF_GAIN in config:
        args += ["-g", f"{config[CONF_GAIN]:g}"]
    if config[CONF_PPM_ERROR]:
        args += ["-p", str(config[CONF_PPM_ERROR])]
    return args + config[CONF_EXTRA_ARGS]


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    for arg in _rtl433_args(config):
        cg.add(var.add_arg(arg))

    if idf_version() >= cv.Version(6, 0, 0):
        add_idf_component(name="espressif/usb", ref=USB_REF)
    add_idf_component(name="esp_rtl_sdr", repo=ESP_RTL_SDR_REPO, ref=ESP_RTL_SDR_REF)
    add_idf_component(name="rtl433_core", path=str(RTL433_CORE))
    esp32.include_builtin_idf_component("pthread")
    # rtl_433's acquire thread is a pthread; its decoders need more than the 3 KB default
    add_idf_sdkconfig_option("CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT", 16384)
