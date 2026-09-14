#!/usr/bin/env python3
"""The Damage settings extension (patches/damage_ext.c) against Damage FIRMWARE.md §0/§3.

Context: host/README.md. Every expected byte below is written by hand from the
contract text, not from the C. Runs the x86 host harness; no device involved.

    python3 host/test_damage_ext.py
"""
import pathlib, subprocess, sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import run_vectors                                      # the harness build

def varint(v):
    out = bytearray()
    while True:
        b = v & 0x7F; v >>= 7
        out.append(b | (0x80 if v else 0))
        if not v: return bytes(out)

def field(num, value): return varint(num << 3) + varint(value)
def ldelim(num, body): return varint((num << 3) | 2) + varint(len(body)) + body
def control(op, arg): return bytes([0x08, 1, 0x10, 0]) + ldelim(112, b"DM" + bytes([1, op, arg & 0xFF, arg >> 8]))
FC_ACQUIRE = bytes([0x08, 1, 0x10, 0]) + ldelim(101, b"FC" + bytes([1, 5, 1, 0]))
FC_RELEASE = bytes([0x08, 1, 0x10, 0]) + ldelim(101, b"FC" + bytes([1, 6, 1, 0]))

def telemetry(req, tick, flags, status, lease_left, lens):
    # §3: 1 id · 2 uptime · 3 flags · 4 the status register · 5 worker µs · 6 copy µs · (7–10 not
    # known on the host: heap arenas and the panel record are unmapped there) · 11 diagnostics ·
    # 12 lease ms left · (13 boot count: not sent by this build) · 14 lens
    body = (field(1, req) + field(2, tick) + field(3, flags) + field(4, status) + field(5, 0) + field(6, 0)
            + field(11, 0) + field(12, lease_left) + field(14, lens))
    return bytes([0x08, 3, 0x10, 0]) + ldelim(111, body)

def run(commands):
    r = subprocess.run([str(run_vectors.BIN)], input="\n".join(commands) + "\n", capture_output=True, text=True)
    if r.returncode != 0: sys.exit(f"harness exited {r.returncode}: {r.stderr}")
    return [l for l in r.stdout.splitlines() if l.startswith("send ")]

def main():
    run_vectors.build()
    fails = 0
    def check(label, got, want):
        nonlocal fails
        ok = got == want
        print(f"  {'PASS' if ok else 'FAIL'}  {label}" + ("" if ok else f"\n          got  {got}\n          want {want}"))
        fails += not ok

    caps = ldelim(110, b"\x0a\x03DMG" + field(2, 1) + field(3, 0b11))
    sends = run(["lens R", "respond 08021005"])
    check("a settings READ response ends with field 110 DamageCaps",
          len(sends) == 1 and sends[0].split()[3].endswith(caps.hex()), True)

    sends = run(["lens R", "tick 5000", "settings " + control(1, 42).hex()])
    check("TELEMETRY answers with the record (right lens, no lease)",
          sends, [f"send 1 9 {telemetry(42, 5000, 0, 0, 0, 1).hex()}"])

    sends = run(["lens L", "tick 7000", "settings " + FC_ACQUIRE.hex(), "settings " + control(2, 0x8000).hex(),
                 "settings " + control(2, 0x0001).hex(),
                 "tick 17000", "settings " + control(1, 7).hex(),
                 "tick 200000", "settings " + control(1, 8).hex(),
                 "settings " + control(3, 0).hex()])
    check("FLAGS_SET PROBE is armed and echoed (left lens, 90 s lease)",
          sends[0], f"send 1 9 {telemetry(0, 7000, 0x8000, 0, 90000, 2).hex()}")
    check("FLAGS_SET of an unimplemented bit changes nothing, status 2",
          sends[1], f"send 1 9 {telemetry(0, 7000, 0x8000, 2, 90000, 2).hex()}")
    check("flags hold while the lease holds; TELEMETRY reports the last recorded status",
          sends[2], f"send 1 9 {telemetry(7, 17000, 0x8000, 2, 80000, 2).hex()}")
    check("a lapsed lease clears the flags, not the status register",
          sends[3], f"send 1 9 {telemetry(8, 200000, 0, 2, 0, 2).hex()}")
    check("FLAGS_CLEAR records status 0", sends[4], f"send 1 9 {telemetry(0, 200000, 0, 0, 0, 2).hex()}")

    sends = run(["lens R", "tick 1000", "settings " + FC_ACQUIRE.hex(), "settings " + control(2, 0x8000).hex(),
                 "settings " + FC_RELEASE.hex(), "settings " + control(1, 9).hex()])
    check("FB_RELEASE clears the flags", sends[-1], f"send 1 9 {telemetry(9, 1000, 0, 0, 0, 1).hex()}")

    short = bytes([0x08, 1, 0x10, 0]) + ldelim(112, b"DM\x01\x01\x2a")
    marker = bytes([0x08, 1, 0x10, 0]) + ldelim(112, b"XM\x01\x01\x2a\x00")
    sends = run(["lens R", "settings " + short.hex(), "settings " + marker.hex()])
    check("a body of the wrong length or marker gets no answer", sends, [])
    sends = run(["lens R", "settings " + marker.hex(), "settings " + control(1, 11).hex()])
    check("an unanswered malformed body is recorded: the next TELEMETRY reports status 1",
          sends, [f"send 1 9 {telemetry(11, 0, 0, 1, 0, 1).hex()}"])
    sends = run(["lens R", "settings " + control(9, 0).hex()])
    check("an unknown op is answered, status 1", sends, [f"send 1 9 {telemetry(0, 0, 0, 1, 0, 1).hex()}"])

    print("RESULT: " + ("all pass" if not fails else f"{fails} failure(s)"))
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
