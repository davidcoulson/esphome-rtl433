#!/usr/bin/env python3
"""Config tests for the rtl_433 ESPHome component: no hardware, no compiler.

Valid cases run `esphome compile --only-generate` and check what the component generated (the rtl_433
command line and the setters it calls). Invalid cases run `esphome config` and check the error text.

    python3 tests/config_tests.py            # all cases
    python3 tests/config_tests.py -k squelch # cases whose name contains "squelch"
"""
import argparse
import copy
import pathlib
import re
import subprocess
import sys
import tempfile

import yaml

REPO = pathlib.Path(__file__).resolve().parents[1]

BASE = {
    "esphome": {"name": "rtl433-cfgtest"},
    "esp32": {"variant": "esp32p4", "engineering_sample": True, "flash_size": "16MB",
              "framework": {"type": "esp-idf"}},
    "psram": {"mode": "hex", "speed": "200MHz"},
    "logger": None,
    "api": None,
    "time": [{"platform": "sntp"}],
    "ethernet": {"type": "IP101", "mdc_pin": "GPIO31", "mdio_pin": "GPIO52", "power_pin": "GPIO51",
                 "clk": {"mode": "CLK_EXT_IN", "pin": "GPIO50"}, "phy_addr": 1},
    "external_components": [{"source": {"type": "local", "path": str(REPO / "components")},
                             "components": ["rtl_433"]}],
    "rtl_433": {},
}


def argv_of(main_cpp):
    return re.findall(r'add_arg\("([^"]*)"\)', main_cpp)


def has_seq(argv, seq):
    return any(argv[i:i + len(seq)] == seq for i in range(len(argv) - len(seq) + 1))


# (name, rtl_433 block or full-config mutator, expectation)
#   expectation for valid cases: list of checks, each ("args", [..seq..]) / ("no_args", [..]) /
#   ("code", "substring in main.cpp") / ("define", "USE_...")
#   expectation for invalid cases: ("error", "substring of the validation error")
CASES = [
    ("minimal defaults", {}, [
        ("args", ["-F", "http:0.0.0.0:8433"]),
        ("args", ["-f", "433920000"]),
        ("args", ["-s", "250000"]),
        ("args", ["-D", "restart"]),
        ("code", "set_remote_control(true)"),
        ("code", "set_time_sync_timeout(120000)"),
    ]),
    ("two bands, per-band rate/hop/decoders", {
        "frequencies": [
            {"frequency": "433.92MHz", "sample_rate": "250k", "hop_interval": "30s",
             "decoders": ["acurite_txr", "acurite_606"]},
            {"frequency": "915MHz", "sample_rate": "2048k", "hop_interval": "150s", "decoders": ["scmplus"]},
        ]}, [
        ("args", ["-f", "433920000", "-f", "915000000"]),
        ("args", ["-s", "250000", "-s", "2048000"]),
        ("args", ["-H", "30", "-H", "150"]),
        ("code", 'add_band_decoders(0, "acurite_txr,acurite_606")'),
        ("code", 'add_band_decoders(1, "scmplus")'),
    ]),
    ("shared rate is passed once", {
        "sample_rate": "1024k", "frequencies": ["433.92MHz", "315MHz"]}, [
        ("args", ["-s", "1024000"]),
        ("no_args", ["-s", "1024000", "-s"]),
    ]),
    ("low-range rates accepted", {
        "frequencies": [{"frequency": "433.92MHz", "sample_rate": 225001},
                        {"frequency": "315MHz", "sample_rate": "300k"},
                        {"frequency": "868MHz", "sample_rate": 900001}]}, [
        ("args", ["-s", "225001", "-s", "300000", "-s", "900001"]),
    ]),
    ("read-only API", {"remote_control": False}, [
        ("code", "set_remote_control(false)"),
    ]),
    ("detector squelch + autolevel", {"detector": {"auto_level": True, "squelch": True}}, [
        ("args", ["-Y", "autolevel"]),
        ("args", ["-Y", "squelch"]),
    ]),
    ("gain, ppm, units", {"gain": 40, "ppm_error": -3, "units": "customary"}, [
        ("args", ["-g", "40"]),
        ("args", ["-p", "-3"]),
        ("args", ["-C", "customary"]),
    ]),
    ("health sensors + task stats", {
        "log_task_stats": True, "time_sync_timeout": "0s",
        "decode_stack_free": {"name": "Decode Stack Free"},
        "acquire_stack_free": {"name": "Acquire Stack Free"},
        "heap_free": {"name": "Heap Free"}, "psram_free": {"name": "PSRAM Free"},
        "cpu_load_core0": {"name": "CPU 0"}, "cpu_load_core1": {"name": "CPU 1"},
        "decoded_events": {"name": "Decoded Events"}}, [
        ("code", "set_decode_stack_free_sensor("),
        ("code", "set_acquire_stack_free_sensor("),
        ("code", "set_heap_free_sensor("),
        ("code", "set_psram_free_sensor("),
        ("code", "set_time_sync_timeout(0)"),
        ("define", "USE_RTL433_TASK_STATS"),
        ("define", "USE_RTL433_CPU_LOAD"),
    ]),
    ("extra args passed through last", {"extra_args": ["-M", "noise"]}, [
        ("args", ["-M", "noise"]),
    ]),
    ("rate 900000 exactly is rejected", {"sample_rate": 900000}, ("error", "not exactly 900k")),
    ("rate in the 300k-900k gap is rejected", {"sample_rate": "500k"}, ("error", "sample_rate must be")),
    ("rate above 3.2M is rejected", {"sample_rate": "3300k"}, ("error", "sample_rate must be")),
    ("unknown decoder is rejected", {"decoders": ["not_a_decoder"]}, ("error", "unknown rtl_433 decoder")),
    ("gain out of range is rejected", {"gain": 60}, ("error", "")),
    ("missing psram is rejected", lambda c: c.pop("psram"), ("error", "needs PSRAM")),
    ("missing time source is rejected", lambda c: c.pop("time"), ("error", "needs a time source")),
]


def run_case(tmp, name, spec, expect):
    cfg = copy.deepcopy(BASE)
    if callable(spec):
        spec(cfg)
    else:
        cfg["rtl_433"] = spec
    path = tmp / "cfg.yaml"
    path.write_text(yaml.safe_dump(cfg, sort_keys=False))
    if isinstance(expect, tuple):  # invalid config
        r = subprocess.run(["esphome", "config", str(path)], capture_output=True, text=True)
        out = r.stdout + r.stderr
        if r.returncode == 0:
            return "expected a validation error, config was accepted"
        if expect[1] and expect[1] not in out:
            return f"error did not mention {expect[1]!r}:\n{out[-800:]}"
        return None
    r = subprocess.run(["esphome", "compile", "--only-generate", str(path)], capture_output=True, text=True)
    if r.returncode != 0:
        return f"generate failed:\n{(r.stdout + r.stderr)[-1500:]}"
    src = tmp / ".esphome" / "build" / "rtl433-cfgtest" / "src"
    main_cpp = (src / "main.cpp").read_text()
    defines = (src / "esphome" / "core" / "defines.h").read_text()
    argv = argv_of(main_cpp)
    for kind, want in expect:
        if kind == "args" and not has_seq(argv, want):
            return f"argv lacks {want}: {' '.join(argv)}"
        if kind == "no_args" and has_seq(argv, want):
            return f"argv unexpectedly has {want}: {' '.join(argv)}"
        if kind == "code" and want not in main_cpp:
            return f"main.cpp lacks {want!r}"
        if kind == "define" and want not in defines:
            return f"defines.h lacks {want}"
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-k", default="", help="only run cases whose name contains this")
    args = ap.parse_args()
    failed = 0
    ran = 0
    with tempfile.TemporaryDirectory() as d:
        tmp = pathlib.Path(d)
        for name, spec, expect in CASES:
            if args.k and args.k not in name:
                continue
            ran += 1
            err = run_case(tmp, name, spec, expect)
            print(f"{'PASS' if err is None else 'FAIL'}  {name}")
            if err:
                failed += 1
                print("      " + err.replace("\n", "\n      "))
    print(f"\n{ran - failed}/{ran} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
