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
import esphome.final_validate as fv
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


CONF_FREQUENCY = "frequency"
CONF_DECODERS = "decoders"
CONF_UNITS = "units"


def _known_decoders():
    # Decoder names as rtl_433 declares them (DECL(name) in rtl_433_devices.h), e.g. scmplus, acurite_txr
    header = RTL433_CORE / "include" / "rtl_433_devices.h"
    import re

    return re.findall(r"DECL\((\w+)\)", header.read_text())


def _decoder(value):
    # A decoder name (see `rtl_433 -R help`'s source list) or its protocol number
    if isinstance(value, int) or (isinstance(value, str) and value.isdigit()):
        return str(cv.int_range(min=1, max=999)(int(value)))
    value = cv.string_strict(value)
    known = _known_decoders()
    if value not in known:
        raise cv.Invalid(f"unknown rtl_433 decoder '{value}' (use the name from rtl_433_devices.h, e.g. scmplus)")
    return value


def _sample_rate(value):
    # esp_rtl_sdr streams 225k-300k or 900k-3.2M S/s; accepts 250000, "250k", "1024k"
    if isinstance(value, str) and value.lower().endswith("k"):
        value = float(value[:-1]) * 1000
    value = cv.int_(value)
    if not (225001 <= value <= 300000 or 900000 <= value <= 3200000):
        raise cv.Invalid("sample_rate must be 225k-300k or 900k-3.2M S/s")
    return value


def _frequency_entry(value):
    if isinstance(value, dict):
        return cv.Schema(
            {
                cv.Required(CONF_FREQUENCY): cv.frequency,
                cv.Optional(CONF_SAMPLE_RATE): _sample_rate,
                cv.Optional(CONF_HOP_INTERVAL): cv.positive_time_period_seconds,
                # Only these decoders while on this frequency
                cv.Optional(CONF_DECODERS): cv.ensure_list(_decoder),
            }
        )(value)
    return {CONF_FREQUENCY: cv.frequency(value)}


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
            # Plain frequencies, or {frequency, sample_rate, hop_interval} to override the defaults below per band
            cv.Optional(CONF_FREQUENCIES, default=["433.92MHz"]): cv.All(
                cv.ensure_list(_frequency_entry), cv.Length(min=1, max=32)
            ),
            cv.Optional(CONF_HOP_INTERVAL, default="600s"): cv.positive_time_period_seconds,
            cv.Optional(CONF_SAMPLE_RATE, default=250000): _sample_rate,
            # Tuner gain in dB; omitted = automatic
            cv.Optional(CONF_GAIN): cv.float_range(min=0, max=50),
            cv.Optional(CONF_PPM_ERROR, default=0): cv.int_range(min=-200, max=200),
            # Decoders for every band that doesn't list its own; omitted = rtl_433's default set
            cv.Optional(CONF_DECODERS): cv.ensure_list(_decoder),
            # rtl_433 -C: native, si or customary units in the events
            cv.Optional(CONF_UNITS, default="si"): cv.one_of(
                "native", "si", "customary", lower=True
            ),
            # Anything else rtl_433 accepts on its command line, e.g. ["-R", "40", "-M", "level"]
            cv.Optional(CONF_EXTRA_ARGS, default=[]): cv.ensure_list(cv.string_strict),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    _only_p4,
)


def _final_validate(config):
    # rtl_433 keeps ~4 MB of sample buffers; the P4 boards this targets have 32 MB of PSRAM
    if "psram" not in fv.full_config.get():
        raise cv.Invalid("rtl_433 needs PSRAM: add a psram: block (mode: hex, speed: 200MHz on the P4)")
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


def _rtl433_args(config):
    # -D restart: rtl_433's stall watchdog reopens the dongle (e.g. after it is replugged) instead of quitting
    args = ["rtl_433", "-d", "esp", "-D", "restart", "-F", f"http:0.0.0.0:{config[CONF_PORT]}"]
    args += ["-M", "time:iso:usec:tz", "-M", "protocol", "-M", "level"]
    freqs = config[CONF_FREQUENCIES]
    for entry in freqs:
        args += ["-f", str(int(entry[CONF_FREQUENCY]))]
    # One -H/-s each: rtl_433 pairs the n-th with the n-th -f. A single value applies to every band.
    rates = [e.get(CONF_SAMPLE_RATE, config[CONF_SAMPLE_RATE]) for e in freqs]
    for rate in rates if len(set(rates)) > 1 else rates[:1]:
        args += ["-s", str(rate)]
    if len(freqs) > 1:
        hops = [
            e.get(CONF_HOP_INTERVAL, config[CONF_HOP_INTERVAL]).total_seconds for e in freqs
        ]
        for hop in hops if len(set(hops)) > 1 else hops[:1]:
            args += ["-H", str(hop)]
    if CONF_GAIN in config:
        args += ["-g", f"{config[CONF_GAIN]:g}"]
    if config[CONF_PPM_ERROR]:
        args += ["-p", str(config[CONF_PPM_ERROR])]
    args += ["-C", config[CONF_UNITS]]
    return args + config[CONF_EXTRA_ARGS]


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    for arg in _rtl433_args(config):
        cg.add(var.add_arg(arg))
    # Decoder sets go straight to the port layer rather than as -R, so they can change on each hop
    default = config.get(CONF_DECODERS)
    for index, entry in enumerate(config[CONF_FREQUENCIES]):
        decoders = entry.get(CONF_DECODERS, default)
        if decoders:
            cg.add(var.add_band_decoders(index, ",".join(decoders)))

    if idf_version() >= cv.Version(6, 0, 0):
        add_idf_component(name="espressif/usb", ref=USB_REF)
    add_idf_component(name="esp_rtl_sdr", repo=ESP_RTL_SDR_REPO, ref=ESP_RTL_SDR_REF)
    add_idf_component(name="rtl433_core", path=str(RTL433_CORE))
    esp32.include_builtin_idf_component("pthread")
    # rtl_433's acquire thread is a pthread; its decoders need more than the 3 KB default
    add_idf_sdkconfig_option("CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT", 16384)
    # Its sample buffers (15 x 256 KB) come from plain malloc(), so malloc must reach PSRAM
    add_idf_sdkconfig_option("CONFIG_SPIRAM_USE_MALLOC", True)
    # HTTP listener(s), Mongoose's internal socket pair, one WebSocket per Home Assistant hub, plus the API
    add_idf_sdkconfig_option("CONFIG_LWIP_MAX_SOCKETS", 16)
    # esp_rtl_sdr's descriptor and tuner transfers (its P4 reference config uses 1024)
    add_idf_sdkconfig_option("CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE", 1024)
