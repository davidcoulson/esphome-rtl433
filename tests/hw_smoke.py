#!/usr/bin/env python3
"""Hardware smoke test for a running rtl_433-on-ESP32 receiver, over its HTTP/WebSocket API.

Checks what can only be seen on real hardware: the dongle is open, rtl_433 hops through every
configured band at the configured rate, the pulse detector sees frames, the WebSocket feed the Home
Assistant integration uses delivers JSON, and (optionally) that the API refuses remote control.

    python3 tests/hw_smoke.py 192.168.1.50 --band 433920000:250000 --band 915000000:2048000
    python3 tests/hw_smoke.py 192.168.1.50 --band 433920000:250000 --read-only --timeout 300

Exit status 0 means every check passed. Needs only the Python standard library.
"""
import argparse
import base64
import json
import os
import socket
import sys
import time
import urllib.parse
import urllib.request


def cmd(host, port, name, **params):
    q = urllib.parse.urlencode({"cmd": name, **params})
    with urllib.request.urlopen(f"http://{host}:{port}/cmd?{q}", timeout=5) as r:
        return json.loads(r.read().decode())


def ws_first_messages(host, port, count=2, timeout=10):
    """Connect to /ws and return the first `count` text frames."""
    s = socket.create_connection((host, port), timeout=timeout)
    key = base64.b64encode(os.urandom(16)).decode()
    s.sendall((f"GET /ws HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n").encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        buf += s.recv(4096)
    head, buf = buf.split(b"\r\n\r\n", 1)
    if b" 101 " not in head.split(b"\r\n")[0]:
        raise RuntimeError(f"WebSocket upgrade refused: {head.splitlines()[0]!r}")
    msgs = []
    end = time.time() + timeout
    while len(msgs) < count and time.time() < end:
        while len(buf) >= 2:
            ln = buf[1] & 0x7F
            off = 2
            if ln == 126:
                if len(buf) < 4:
                    break
                ln, off = int.from_bytes(buf[2:4], "big"), 4
            elif ln == 127:
                if len(buf) < 10:
                    break
                ln, off = int.from_bytes(buf[2:10], "big"), 10
            if len(buf) < off + ln:
                break
            if buf[0] & 0x0F == 1:
                msgs.append(buf[off:off + ln].decode(errors="replace"))
            buf = buf[off + ln:]
        if len(msgs) >= count:
            break
        try:
            buf += s.recv(65536)
        except socket.timeout:
            break
    s.close()
    return msgs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host")
    ap.add_argument("--port", type=int, default=8433)
    ap.add_argument("--band", action="append", default=[], metavar="FREQ_HZ:RATE",
                    help="a configured band and its sample rate; repeat for each band")
    ap.add_argument("--timeout", type=int, default=240, help="seconds to wait to see every band")
    ap.add_argument("--read-only", action="store_true", help="expect remote_control: false")
    ap.add_argument("--expect-events", action="store_true", help="also require at least one decoded event")
    a = ap.parse_args()
    bands = {int(f): int(r) for f, r in (b.split(":") for b in a.band)}
    failures = []

    def check(ok, what):
        print(f"{'PASS' if ok else 'FAIL'}  {what}")
        if not ok:
            failures.append(what)

    info = cmd(a.host, a.port, "get_dev_info").get("result")
    check(isinstance(info, dict) and info.get("product"), f"dongle open: {info}")
    stats0 = cmd(a.host, a.port, "get_stats")["result"]
    check("frames" in stats0, "get_stats answers")
    since = stats0.get("since", "")
    check(not since.startswith("1969") and not since.startswith("1970"),
          f"rtl_433 started after the clock was set (since={since})")

    msgs = ws_first_messages(a.host, a.port)
    check(bool(msgs) and msgs[0].lstrip().startswith("{"), f"WebSocket delivers JSON ({len(msgs)} frame(s))")

    if a.read_only:
        r = cmd(a.host, a.port, "center_frequency", val=868000000)
        check("error" in r and "Read-only" in json.dumps(r), "remote control refused (read-only)")
        r = cmd(a.host, a.port, "get_gain")
        check("result" in r, "queries still answered in read-only mode")

    if bands:
        seen = {}
        end = time.time() + a.timeout
        while time.time() < end and set(seen) != set(bands):
            f = cmd(a.host, a.port, "get_center_frequency")["result"]
            r = cmd(a.host, a.port, "get_sample_rate")["result"]
            seen.setdefault(f, r)
            time.sleep(2)
        for f, r in bands.items():
            check(f in seen, f"visited {f / 1e6:.3f} MHz")
            if f in seen:
                check(seen[f] == r, f"{f / 1e6:.3f} MHz at {r} S/s (saw {seen[f]})")
        extra = set(seen) - set(bands)
        check(not extra, f"no unexpected frequencies ({sorted(extra)})")

    stats1 = cmd(a.host, a.port, "get_stats")["result"]
    check(stats1["frames"]["count"] >= stats0["frames"]["count"], "frame counter moves forward")
    check(stats1["frames"]["count"] > 0, f"pulse detector sees frames ({stats1['frames']['count']})")
    if a.expect_events:
        check(stats1["frames"]["events"] > 0, f"decoded events ({stats1['frames']['events']})")

    print(f"\n{'OK' if not failures else f'{len(failures)} check(s) failed'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
