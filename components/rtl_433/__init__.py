import os
from pathlib import Path

import esphome.codegen as cg
from esphome.components import esp32, sensor
from esphome.components.esp32 import (
    VARIANT_ESP32P4,
    add_idf_component,
    add_idf_sdkconfig_option,
    idf_version,
)
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.const import (
    CONF_ID,
    CONF_PORT,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_PERCENT,
)

DEPENDENCIES = ["network"]
AUTO_LOAD = ["sensor"]
CODEOWNERS = ["@davidcoulson"]

CONF_FREQUENCIES = "frequencies"
CONF_HOP_INTERVAL = "hop_interval"
CONF_SAMPLE_RATE = "sample_rate"
CONF_GAIN = "gain"
CONF_PPM_ERROR = "ppm_error"
CONF_EXTRA_ARGS = "extra_args"

# Pinned to a fork of esp_rtl_sdr v0.8.0-rc3 until upstream picks these up. Upstream's R820T2 path
# (Blog V3 / Nooelec SMArt v5) never programs the tuner's RF mux / tracking filter for the tuned band,
# so those dongles stream but never see any RF (hardcoreerik/esp-rtl-sdr#25); LOW-range rates
# (225k-300k) fail BAD_RATE (#24). The fork fixes both and adds: manual + auto gain and the measured
# 3.57 MHz IF on the Nooelec, in-place sample-rate change on a hop, a fast sync-read copy, control
# transfer bounds checks, a USB device layout check, and backports of upstream's bulk-pool leak,
# halted-endpoint recovery, repeater-before-retune and fault-guard fixes, a fix for a crash when the
# dongle is unplugged mid-stream, and a retry for a failed USB enumeration. The fork's own fixes are
# proposed upstream as hardcoreerik/esp-rtl-sdr#26.
ESP_RTL_SDR_REPO = "https://github.com/davidcoulson/esp-rtl-sdr.git"
ESP_RTL_SDR_REF = "74845faede0c8fb6bf2a003884a890616021fb27"  # branch nooelec-gain-cap-test
USB_REF = "1.4.1"  # same espressif/usb ESPHome's usb_host pins for IDF 6

# The ESP-IDF component that wraps the rtl_433 sources lives next to components/ in this repo. It is found
# relative to this file, which works for a local path and for a github:// source alike (ESPHome clones
# the whole repository).
RTL433_CORE = Path(__file__).resolve().parents[2] / "idf" / "rtl433_core"

rtl_433_ns = cg.esphome_ns.namespace("rtl_433")
Rtl433Component = rtl_433_ns.class_("Rtl433Component", cg.PollingComponent)


CONF_FREQUENCY = "frequency"
CONF_DECODERS = "decoders"
CONF_UNITS = "units"
CONF_TASK_CORE = "task_core"
CONF_TASK_PRIORITY = "task_priority"
CONF_ACQUIRE_PRIORITY = "acquire_priority"
CONF_USB_TASK_PRIORITY = "usb_task_priority"
CONF_USB_BUFFER = "usb_buffer"
CONF_DETECTOR = "detector"
CONF_FSK_DETECTOR = "fsk_detector"
CONF_LEVEL = "level"
CONF_MIN_LEVEL = "min_level"
CONF_MIN_SNR = "min_snr"
CONF_AUTO_LEVEL = "auto_level"
CONF_SQUELCH = "squelch"
CONF_LEVEL_ESTIMATOR = "level_estimator"
CONF_FM_FILTER = "fm_filter"
CONF_BIAS_TEE = "bias_tee"
CONF_DIGITAL_AGC = "digital_agc"
CONF_FLEX_DECODERS = "flex_decoders"
CONF_OUTPUTS = "outputs"
CONF_REPORT_NOISE = "report_noise"
CONF_HOP_ON_EVENT = "hop_on_event"
CONF_TAGS = "tags"
CONF_VERBOSITY = "verbosity"
CONF_REMOTE_CONTROL = "remote_control"
CONF_TIME_SYNC_TIMEOUT = "time_sync_timeout"
CONF_LOG_TASK_STATS = "log_task_stats"
CONF_DECODE_STACK_FREE = "decode_stack_free"
CONF_ACQUIRE_STACK_FREE = "acquire_stack_free"
CONF_HEAP_FREE = "heap_free"
CONF_PSRAM_FREE = "psram_free"
CONF_EFFECTIVE_SAMPLE_RATE = "effective_sample_rate"
CONF_USB_OVERRUNS = "usb_overruns"
CONF_DROPPED_SAMPLES = "dropped_samples"
CONF_DECODED_EVENTS = "decoded_events"
CONF_CPU_LOAD_CORE0 = "cpu_load_core0"
CONF_CPU_LOAD_CORE1 = "cpu_load_core1"


def _bytes(value):
    # 1048576, "512KB", "1MB"
    if isinstance(value, str):
        v = value.strip().upper().replace(" ", "")
        for suffix, mult in (("MB", 1 << 20), ("KB", 1 << 10), ("B", 1)):
            if v.endswith(suffix):
                return int(float(v[: -len(suffix)]) * mult)
    return cv.int_(value)


DETECTOR_SCHEMA = cv.Schema(
    {
        # -Y auto|classic|minmax: FSK pulse detector
        cv.Optional(CONF_FSK_DETECTOR): cv.one_of("auto", "classic", "minmax", lower=True),
        # -Y level=: fixed detection level in dB (-30 to -1); 0 = automatic
        cv.Optional(CONF_LEVEL): cv.float_range(min=-30, max=0),
        # -Y minlevel=: lowest level the automatic detection may go to (dB)
        cv.Optional(CONF_MIN_LEVEL): cv.float_range(min=-99, max=-1),
        # -Y minsnr=: minimum signal-to-noise ratio for a pulse (dB)
        cv.Optional(CONF_MIN_SNR): cv.float_range(min=1, max=99),
        # -Y autolevel: set min_level from the estimated noise floor (also feeds HA's noise sensor)
        cv.Optional(CONF_AUTO_LEVEL): cv.boolean,
        # -Y squelch: skip frames below the noise estimate, saving CPU
        cv.Optional(CONF_SQUELCH): cv.boolean,
        # -Y ampest|magest: amplitude or magnitude level estimator
        cv.Optional(CONF_LEVEL_ESTIMATOR): cv.one_of("amplitude", "magnitude", lower=True),
        # -Y filter=: FM low-pass cutoff to separate simultaneous transmissions (us 1-9999, Hz 10000+, or ratio)
        cv.Optional(CONF_FM_FILTER): cv.string_strict,
    }
)

_DIAG = {"entity_category": ENTITY_CATEGORY_DIAGNOSTIC}


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
    # The RTL2832U resampler covers 225k-300k and 900k-3.2M S/s (librtlsdr's limits: exactly 900000
    # aliases to 300k). Accepts 250000, "250k", "1024k". The low range needs the pinned esp_rtl_sdr
    # fork (upstream rc3 rejects it, hardcoreerik/esp-rtl-sdr#24).
    if isinstance(value, str) and value.lower().endswith("k"):
        value = float(value[:-1]) * 1000
    value = cv.int_(value)
    if not (225001 <= value <= 300000 or 900001 <= value <= 3200000):
        raise cv.Invalid("sample_rate must be 225k-300k or 900k-3.2M S/s (not exactly 900k)")
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
            # Core for rtl_433's decoding task; by default the one the ESPHome loop isn't on
            cv.Optional(CONF_TASK_CORE): cv.int_range(min=0, max=1),
            # rtl_433 -C: native, si or customary units in the events
            cv.Optional(CONF_UNITS, default="si"): cv.one_of(
                "native", "si", "customary", lower=True
            ),
            # Pulse detector (-Y) settings
            cv.Optional(CONF_DETECTOR, default={}): DETECTOR_SCHEMA,
            # -t biastee / digital_agc: power an active antenna / the RTL2832's own AGC
            cv.Optional(CONF_BIAS_TEE): cv.boolean,
            cv.Optional(CONF_DIGITAL_AGC): cv.boolean,
            # -X: flex decoders for devices rtl_433 doesn't know, e.g. "n=doorbell,m=OOK_PWM,s=400,l=1200,r=5000"
            cv.Optional(CONF_FLEX_DECODERS, default=[]): cv.ensure_list(cv.string_strict),
            # -F: more outputs besides the HTTP API, e.g. "mqtt://192.168.1.2:1883,retain=0" or "syslog:10.0.0.5:514"
            cv.Optional(CONF_OUTPUTS, default=[]): cv.ensure_list(cv.string_strict),
            # -M noise:<s>: report the noise level at this interval
            cv.Optional(CONF_REPORT_NOISE): cv.positive_time_period_seconds,
            # -E hop: move to the next frequency straight after a successful decode
            cv.Optional(CONF_HOP_ON_EVENT, default=False): cv.boolean,
            # -K key=value: extra fields on every event, e.g. {receiver: attic}
            cv.Optional(CONF_TAGS, default={}): cv.Schema({cv.string_strict: cv.string_strict}),
            # -v: rtl_433 log verbosity (0 = normal ... 4 = trace)
            cv.Optional(CONF_VERBOSITY, default=0): cv.int_range(min=0, max=4),
            # USB driver's sample ring (PSRAM). The default holds ~50-125 ms at 2 MS/s; a longer decode burst
            # than that drops samples (see dropped_samples)
            cv.Optional(CONF_USB_BUFFER): cv.All(_bytes, cv.int_range(min=64 << 10, max=8 << 20)),
            # FreeRTOS priorities: rtl_433's decoding task, its USB acquire thread, the driver's USB task
            # (0 = driver default)
            cv.Optional(CONF_TASK_PRIORITY, default=5): cv.int_range(min=1, max=22),
            cv.Optional(CONF_ACQUIRE_PRIORITY, default=6): cv.int_range(min=1, max=22),
            cv.Optional(CONF_USB_TASK_PRIORITY, default=0): cv.int_range(min=0, max=22),
            # false: rtl_433's HTTP API answers queries only, so nobody on the network can retune or
            # reconfigure the receiver through /cmd (Home Assistant's SDR controls stop working too)
            cv.Optional(CONF_REMOTE_CONTROL, default=True): cv.boolean,
            # Hold rtl_433 back until the clock is set, so the first events don't carry 1970 timestamps;
            # start anyway after this long (0 = don't wait)
            cv.Optional(CONF_TIME_SYNC_TIMEOUT, default="120s"): cv.positive_time_period_milliseconds,
            # Log every task's share of each core at each update (debug level): where the CPU goes
            cv.Optional(CONF_LOG_TASK_STATS, default=False): cv.boolean,
            # Diagnostics
            cv.Optional(CONF_DECODE_STACK_FREE): sensor.sensor_schema(
                unit_of_measurement="B",
                icon="mdi:layers-outline",
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
                **_DIAG,
            ),
            cv.Optional(CONF_ACQUIRE_STACK_FREE): sensor.sensor_schema(
                unit_of_measurement="B",
                icon="mdi:layers-outline",
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
                **_DIAG,
            ),
            cv.Optional(CONF_HEAP_FREE): sensor.sensor_schema(
                unit_of_measurement="B",
                icon="mdi:memory",
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
                **_DIAG,
            ),
            cv.Optional(CONF_PSRAM_FREE): sensor.sensor_schema(
                unit_of_measurement="B",
                icon="mdi:memory",
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
                **_DIAG,
            ),
            cv.Optional(CONF_EFFECTIVE_SAMPLE_RATE): sensor.sensor_schema(
                unit_of_measurement="S/s",
                icon="mdi:sine-wave",
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
                **_DIAG,
            ),
            cv.Optional(CONF_USB_OVERRUNS): sensor.sensor_schema(
                icon="mdi:usb",
                accuracy_decimals=0,
                state_class=STATE_CLASS_TOTAL_INCREASING,
                **_DIAG,
            ),
            cv.Optional(CONF_DROPPED_SAMPLES): sensor.sensor_schema(
                icon="mdi:package-variant-remove",
                accuracy_decimals=0,
                state_class=STATE_CLASS_TOTAL_INCREASING,
                **_DIAG,
            ),
            cv.Optional(CONF_DECODED_EVENTS): sensor.sensor_schema(
                icon="mdi:radio-tower",
                accuracy_decimals=0,
                state_class=STATE_CLASS_TOTAL_INCREASING,
                **_DIAG,
            ),
            cv.Optional(CONF_CPU_LOAD_CORE0): sensor.sensor_schema(
                unit_of_measurement=UNIT_PERCENT,
                icon="mdi:cpu-64-bit",
                accuracy_decimals=1,
                state_class=STATE_CLASS_MEASUREMENT,
                **_DIAG,
            ),
            cv.Optional(CONF_CPU_LOAD_CORE1): sensor.sensor_schema(
                unit_of_measurement=UNIT_PERCENT,
                icon="mdi:cpu-64-bit",
                accuracy_decimals=1,
                state_class=STATE_CLASS_MEASUREMENT,
                **_DIAG,
            ),
            # Anything else rtl_433 accepts on its command line, e.g. ["-R", "40", "-M", "level"]
            cv.Optional(CONF_EXTRA_ARGS, default=[]): cv.ensure_list(cv.string_strict),
        }
    ).extend(cv.polling_component_schema("60s")),
    cv.only_on_esp32,
    _only_p4,
)


def _final_validate(config):
    # rtl_433 keeps ~6 MB of sample buffers; the P4 boards this targets have 32 MB of PSRAM
    full = fv.full_config.get()
    if "psram" not in full:
        raise cv.Invalid("rtl_433 needs PSRAM: add a psram: block (mode: hex, speed: 200MHz on the P4)")
    # Events carry wall-clock timestamps that the Home Assistant integration checks against its own clock
    if "time" not in full:
        raise cv.Invalid("rtl_433 needs a time source: add a time: block (e.g. platform: sntp or homeassistant)")
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


def _rtl433_args(config):
    # -D restart: rtl_433's stall watchdog reopens the dongle (e.g. after it is replugged) instead of quitting
    args = ["rtl_433", "-d", "esp", "-D", "restart", "-F", f"http:0.0.0.0:{config[CONF_PORT]}"]
    # utc: ESP-IDF's strftime("%z") reports +0000 even with TZ set, so local times went out labelled Z
    args += ["-M", "time:iso:usec:tz:utc", "-M", "protocol", "-M", "level"]
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

    det = config[CONF_DETECTOR]
    y = []
    if CONF_FSK_DETECTOR in det:
        y.append(det[CONF_FSK_DETECTOR])
    if CONF_LEVEL in det:
        y.append(f"level={det[CONF_LEVEL]:g}")
    if CONF_MIN_LEVEL in det:
        y.append(f"minlevel={det[CONF_MIN_LEVEL]:g}")
    if CONF_MIN_SNR in det:
        y.append(f"minsnr={det[CONF_MIN_SNR]:g}")
    if det.get(CONF_AUTO_LEVEL):
        y.append("autolevel")
    if det.get(CONF_SQUELCH):
        y.append("squelch")
    if CONF_LEVEL_ESTIMATOR in det:
        y.append("ampest" if det[CONF_LEVEL_ESTIMATOR] == "amplitude" else "magest")
    if CONF_FM_FILTER in det:
        y.append(f"filter={det[CONF_FM_FILTER]}")
    for opt in y:
        args += ["-Y", opt]

    settings = []
    if CONF_BIAS_TEE in config:
        settings.append(f"biastee={int(config[CONF_BIAS_TEE])}")
    if CONF_DIGITAL_AGC in config:
        settings.append(f"digital_agc={int(config[CONF_DIGITAL_AGC])}")
    if settings:
        args += ["-t", ",".join(settings)]
    for spec in config[CONF_FLEX_DECODERS]:
        args += ["-X", spec]
    for out in config[CONF_OUTPUTS]:
        args += ["-F", out]
    if CONF_REPORT_NOISE in config:
        args += ["-M", f"noise:{config[CONF_REPORT_NOISE].total_seconds}"]
    if config[CONF_HOP_ON_EVENT]:
        args += ["-E", "hop"]
    for key, value in config[CONF_TAGS].items():
        args += ["-K", f"{key}={value}"]
    if config[CONF_VERBOSITY]:
        args += ["-" + "v" * config[CONF_VERBOSITY]]
    return args + config[CONF_EXTRA_ARGS]


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    for arg in _rtl433_args(config):
        cg.add(var.add_arg(arg))
    if CONF_TASK_CORE in config:
        cg.add(var.set_task_core(config[CONF_TASK_CORE]))
    cg.add(var.set_task_priority(config[CONF_TASK_PRIORITY]))
    cg.add(var.set_acquire_priority(config[CONF_ACQUIRE_PRIORITY]))
    cg.add(var.set_usb_task_priority(config[CONF_USB_TASK_PRIORITY]))
    if CONF_USB_BUFFER in config:
        cg.add(var.set_usb_buffer(config[CONF_USB_BUFFER]))
    cg.add(var.set_remote_control(config[CONF_REMOTE_CONTROL]))
    cg.add(var.set_time_sync_timeout(config[CONF_TIME_SYNC_TIMEOUT].total_milliseconds))
    for key, setter in (
        (CONF_EFFECTIVE_SAMPLE_RATE, "set_effective_sample_rate_sensor"),
        (CONF_USB_OVERRUNS, "set_usb_overruns_sensor"),
        (CONF_DROPPED_SAMPLES, "set_dropped_samples_sensor"),
        (CONF_DECODED_EVENTS, "set_decoded_events_sensor"),
        (CONF_CPU_LOAD_CORE0, "set_cpu_load_core0_sensor"),
        (CONF_CPU_LOAD_CORE1, "set_cpu_load_core1_sensor"),
        (CONF_DECODE_STACK_FREE, "set_decode_stack_free_sensor"),
        (CONF_ACQUIRE_STACK_FREE, "set_acquire_stack_free_sensor"),
        (CONF_HEAP_FREE, "set_heap_free_sensor"),
        (CONF_PSRAM_FREE, "set_psram_free_sensor"),
    ):
        if conf := config.get(key):
            sens = await sensor.new_sensor(conf)
            cg.add(getattr(var, setter)(sens))
    if CONF_CPU_LOAD_CORE0 in config or CONF_CPU_LOAD_CORE1 in config or config[CONF_LOG_TASK_STATS]:
        # Per-task run-time counters: CPU load is 100% minus each core's idle task share
        add_idf_sdkconfig_option("CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS", True)
        cg.add_define("USE_RTL433_CPU_LOAD")
    if config[CONF_LOG_TASK_STATS]:
        add_idf_sdkconfig_option("CONFIG_FREERTOS_USE_TRACE_FACILITY", True)
        cg.add_define("USE_RTL433_TASK_STATS")
    # Decoder sets go straight to the port layer rather than as -R, so they can change on each hop
    default = config.get(CONF_DECODERS)
    for index, entry in enumerate(config[CONF_FREQUENCIES]):
        decoders = entry.get(CONF_DECODERS, default)
        if decoders:
            cg.add(var.add_band_decoders(index, ",".join(decoders)))

    if idf_version() >= cv.Version(6, 0, 0):
        add_idf_component(name="espressif/usb", ref=USB_REF)
    # Developing the driver alongside: ESP_RTL_SDR_LOCAL_PATH=/path/to/esp_rtl_sdr (the directory must be
    # named esp_rtl_sdr, ESP-IDF names components after their directory) builds that checkout instead
    if local := os.environ.get("ESP_RTL_SDR_LOCAL_PATH"):
        add_idf_component(name="esp_rtl_sdr", path=local)
    else:
        add_idf_component(name="esp_rtl_sdr", repo=ESP_RTL_SDR_REPO, ref=ESP_RTL_SDR_REF)
    add_idf_component(name="rtl433_core", path=str(RTL433_CORE))
    esp32.include_builtin_idf_component("pthread")
    # Its sample buffers (15 x 256 KB) come from plain malloc(), so malloc must reach PSRAM
    add_idf_sdkconfig_option("CONFIG_SPIRAM_USE_MALLOC", True)
    # HTTP listener(s), Mongoose's internal socket pair, one WebSocket per Home Assistant hub, plus the API
    add_idf_sdkconfig_option("CONFIG_LWIP_MAX_SOCKETS", 16)
    # esp_rtl_sdr's descriptor and tuner transfers (its P4 reference config uses 1024)
    add_idf_sdkconfig_option("CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE", 1024)
