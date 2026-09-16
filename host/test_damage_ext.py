#!/usr/bin/env python3
"""The Damage settings extension (patches/damage_ext.c, damage_draw.c) against Damage FIRMWARE.md §0/§3/§4.

Context: host/README.md. Every expected byte below is written by hand from the
contract text, not from the C. Runs the x86 host harness; no device involved.

    python3 host/test_damage_ext.py

What the harness models that matters here: only the RIGHT lens sends (the stock
senders refuse on the left lens — FUN_00475b14 -> FUN_0046f258; h_send does the same),
so a left-lens check reads the context through `dmg` instead; the display task's
refresh call advances the cycle counter by 1,234 µs, so the F1.3 stamp is 1,234 here;
the worker's own stamp brackets the whole dispatch, which on the host includes the
synchronous refresh, so a presented frame's worker figure reads 1,234 too (on the
glasses the display task runs on its own; the figure there is the real worker time);
`panel 0|1` is the display task's panel-on word (a type-3 refresh skips the refresh
call while it is 0) and `refresh` one stock type-3 refresh with no Damage job pending.
"""
import json, pathlib, subprocess, sys, zlib

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
OP_TELEMETRY, OP_FLAGS_SET, OP_FLAGS_CLEAR, OP_CACHE_INFO, OP_CACHE_SIZE = 1, 2, 3, 4, 5
FLAG_PRESENTED, FLAG_CACHE_KEEP, FLAG_DRAW2, FLAG_PROBE = 0x0001, 0x0002, 0x0004, 0x8000
FEATURES = 0b1111111                                    # telemetry, flags, presented, cache-keep, self-test, v2 drawing, the link
CONTRACT = 2
CACHE_SIZE = 65536

def telemetry(req, tick, flags, status, lease_left, lens, worker=0, transfer=0, presents=0, gen=0,
              cache=False, crc=None, st=None, size=CACHE_SIZE, ref=None, path=0, panel=0x70b024):
    # §3: 1 id · 2 uptime · 3 flags · 4 the status register · 5 worker µs · 6 copy µs · 10 the panel
    # record (the host's modeled JBD4010 one) · 11 diagnostics ·
    # 12 lease ms left · (13 boot count: not sent by this build) · 14 lens · 15 last transfer µs ·
    # 16 direct presents · 17 cache generation · 18 cache size when allocated · 19 cache CRC-32
    # (CACHE_INFO only, when allocated) · 20 self-test steps · 21/22 the last step's refusal and
    # scratch CRC-32 (after a step) · 23/24/25 the last image-lane refusal's mode byte, reason and copy
    # sequence (once one was recorded) · 26 the last Damage transfer's path (0 full, 1 the partial rows)
    # (7-9 are not known on the host: the heap arenas are unmapped there; 10 is the modeled JBD4010 record)
    body = (field(1, req) + field(2, tick) + field(3, flags) + field(4, status) + field(5, worker) + field(6, 0)
            + (field(10, panel) if panel else b"")
            + field(11, 0) + field(12, lease_left) + field(14, lens) + field(15, transfer) + field(16, presents)
            + field(17, gen))
    if cache: body += field(18, size)
    if cache and crc is not None: body += field(19, crc)
    body += field(20, st[0] if st else 0)
    if st: body += field(21, st[1]) + field(22, st[2])
    if ref: body += field(23, ref[0]) + field(24, ref[1]) + field(25, ref[2])
    body += field(26, path)
    return bytes([0x08, 3, 0x10, 0]) + ldelim(111, body)

def presented(seq, worker, transfer, lens=1, path=0):
    # field 113 DamagePresented { 1 sequence, 2 worker µs, 3 copy µs, 4 transfer µs, 5 lens, 6 path }
    return bytes([0x08, 3, 0x10, 0]) + ldelim(113, field(1, seq) + field(2, worker) + field(3, 0) + field(4, transfer) + field(5, lens) + field(6, path))

def keyframe_hex():
    """A real mode-6 keyframe: the first message of the v1-keyframe vector (Damage's inputs)."""
    vec = json.loads((run_vectors.DEFAULT_DIR / "v1-keyframe.json").read_text())
    return next(op["msg"] for st in vec["steps"] for op in st["ops"] if "msg" in op)

def run(commands):
    r = subprocess.run([str(run_vectors.BIN)], input="\n".join(commands) + "\n", capture_output=True, text=True)
    if r.returncode != 0: sys.exit(f"harness exited {r.returncode}: {r.stderr}")
    return [l for l in r.stdout.splitlines() if l.strip()]

def sends(lines): return [l for l in lines if l.startswith("send ")]
def dmg(lines):
    """The last `dmg` line as a dict (host_damage_state's order)."""
    l = [x for x in lines if x.startswith("dmg ")][-1].split()
    keys = ["flags", "status", "gen", "latched", "presents", "transferUs", "stSeq", "stRefused", "stCrc", "cache", "directActive",
            "refSeen", "refMode", "refReason", "refSeq", "cacheBytes", "lastPath", "partials", "slotBytes"]
    return {k: (int(v, 16) if k == "stCrc" else int(v)) for k, v in zip(keys, l[1:])}
def rcs(lines): return [int(l.split()[1]) for l in lines if l.startswith("rc ")]

def main():
    run_vectors.build()
    fails = 0
    def check(label, got, want):
        nonlocal fails
        ok = got == want
        print(f"  {'PASS' if ok else 'FAIL'}  {label}" + ("" if ok else f"\n          got  {got}\n          want {want}"))
        fails += not ok

    print("-- §0 the capability field")
    caps = ldelim(110, b"\x0a\x03DMG" + field(2, CONTRACT) + field(3, FEATURES))
    out = sends(run(["lens R", "respond 08021005"]))
    check("a settings READ response ends with field 110 DamageCaps (contract 2, seven features)",
          len(out) == 1 and out[0].split()[3].endswith(caps.hex()), True)
    check("the left lens appends the field too, but its response does not leave the glasses",
          sends(run(["lens L", "respond 08021005"])), [])

    print("-- §3 telemetry, the status register, the flags")
    out = sends(run(["lens R", "tick 5000", "settings " + control(OP_TELEMETRY, 42).hex()]))
    check("TELEMETRY answers with the record (right lens, no lease)",
          out, [f"send 1 9 {telemetry(42, 5000, 0, 0, 0, 1).hex()}"])

    lines = run(["lens R", "tick 7000", "settings " + FC_ACQUIRE.hex(), "settings " + control(OP_FLAGS_SET, FLAG_PROBE).hex(),
                 "settings " + control(OP_FLAGS_SET, 0x0100).hex(),
                 "tick 17000", "settings " + control(OP_TELEMETRY, 7).hex(),
                 "tick 200000", "settings " + control(OP_TELEMETRY, 8).hex(),
                 "settings " + control(OP_FLAGS_CLEAR, 0).hex()])
    out = sends(lines)
    check("FLAGS_SET PROBE is armed and echoed (90 s lease)",
          out[0], f"send 1 9 {telemetry(0, 7000, FLAG_PROBE, 0, 90000, 1).hex()}")
    check("FLAGS_SET of an unimplemented bit changes nothing, status 2",
          out[1], f"send 1 9 {telemetry(0, 7000, FLAG_PROBE, 2, 90000, 1).hex()}")
    check("flags hold while the lease holds; TELEMETRY reports the last recorded status",
          out[2], f"send 1 9 {telemetry(7, 17000, FLAG_PROBE, 2, 80000, 1).hex()}")
    check("a lapsed lease clears the flags, not the status register",
          out[3], f"send 1 9 {telemetry(8, 200000, 0, 2, 0, 1).hex()}")
    check("FLAGS_CLEAR records status 0", out[4], f"send 1 9 {telemetry(0, 200000, 0, 0, 0, 1).hex()}")

    lines = run(["lens L", "tick 7000", "settings " + FC_ACQUIRE.hex(), "settings " + control(OP_FLAGS_SET, FLAG_PROBE).hex(),
                 "dmg", "settings " + control(OP_FLAGS_SET, 0x0100).hex(), "dmg", "tick 200000",
                 "settings " + control(OP_TELEMETRY, 8).hex(), "dmg"])
    check("the left lens answers nothing", sends(lines), [])
    d = [x for x in lines if x.startswith("dmg ")]
    check("the left lens still arms the flag it was written (read from its context)",
          (d[0].split()[1], d[0].split()[2]), (str(FLAG_PROBE), "0"))
    check("and records the refusal of an unimplemented bit", (d[1].split()[1], d[1].split()[2]), (str(FLAG_PROBE), "2"))
    check("and clears its flags at the lapse, keeping the register", (d[2].split()[1], d[2].split()[2]), ("0", "2"))

    out = sends(run(["lens R", "tick 1000", "settings " + FC_ACQUIRE.hex(), "settings " + control(OP_FLAGS_SET, FLAG_PROBE).hex(),
                     "settings " + FC_RELEASE.hex(), "settings " + control(OP_TELEMETRY, 9).hex()]))
    check("FB_RELEASE clears the flags", out[-1], f"send 1 9 {telemetry(9, 1000, 0, 0, 0, 1).hex()}")

    short = bytes([0x08, 1, 0x10, 0]) + ldelim(112, b"DM\x01\x01\x2a")
    marker = bytes([0x08, 1, 0x10, 0]) + ldelim(112, b"XM\x01\x01\x2a\x00")
    out = sends(run(["lens R", "settings " + short.hex(), "settings " + marker.hex()]))
    check("a body of the wrong length or marker gets no answer", out, [])
    out = sends(run(["lens R", "settings " + marker.hex(), "settings " + control(OP_TELEMETRY, 11).hex()]))
    check("an unanswered malformed body is recorded: the next TELEMETRY reports status 1",
          out, [f"send 1 9 {telemetry(11, 0, 0, 1, 0, 1).hex()}"])
    out = sends(run(["lens R", "settings " + control(9, 0).hex()]))
    check("an unknown op is answered, status 1", out, [f"send 1 9 {telemetry(0, 0, 0, 1, 0, 1).hex()}"])

    print("-- F1.3 the panel-transfer stamp and the presented notify")
    kf = keyframe_hex()
    lines = run(["lens R", "tick 1000", "settings " + FC_ACQUIRE.hex(), "msg " + kf, "dmg",
                 "settings " + control(OP_TELEMETRY, 1).hex()])
    d = dmg(lines)
    check("a presented keyframe is counted and its transfer stamped, with no notify while unarmed",
          (d["presents"], d["transferUs"], len(sends(lines))), (1, 1234, 1))
    check("TELEMETRY carries the stamp and the count (fields 15/16; the worker figure is the host's, see the docstring)",
          sends(lines)[-1], f"send 1 9 {telemetry(1, 1000, 0, 0, 90000, 1, worker=1234, transfer=1234, presents=1).hex()}")
    lines = run(["lens R", "tick 1000", "settings " + FC_ACQUIRE.hex(), "settings " + control(OP_FLAGS_SET, FLAG_PRESENTED).hex(),
                 "msg " + kf, "msg " + kf])
    out = sends(lines)
    check("PRESENTED armed: a field-113 notify follows each transfer (the worker figure lags one frame)",
          out[1:], [f"send 1 9 {presented(1, 0, 1234).hex()}", f"send 1 9 {presented(2, 1234, 1234).hex()}"])
    lines = run(["lens L", "tick 1000", "settings " + FC_ACQUIRE.hex(), "settings " + control(OP_FLAGS_SET, FLAG_PRESENTED).hex(),
                 "msg " + kf, "dmg"])
    check("the left lens stamps its own transfer but sends nothing", (dmg(lines)["transferUs"], sends(lines)), (1234, []))

    print("-- F1.5 cache generation, CRC and cache-keep")
    data = bytes(range(256)) * 4
    write = bytes([12]) + (0).to_bytes(2, "little") + len(data).to_bytes(2, "little") + data
    cache_crc = zlib.crc32(data + bytes(CACHE_SIZE - len(data))) & 0xFFFFFFFF
    lines = run(["lens R", "tick 1000", "settings " + FC_ACQUIRE.hex(), "msg " + write.hex(),
                 "settings " + control(OP_TELEMETRY, 3).hex(), "settings " + control(OP_CACHE_INFO, 4).hex()])
    out = sends(lines)
    check("a cache write bumps the generation; TELEMETRY reports size, not the CRC",
          out[0], f"send 1 9 {telemetry(3, 1000, 0, 0, 90000, 1, gen=1, cache=True).hex()}")
    check("CACHE_INFO adds the CRC-32 of the whole cache (zlib polynomial)",
          out[1], f"send 1 9 {telemetry(4, 1000, 0, 0, 90000, 1, gen=1, cache=True, crc=cache_crc).hex()}")
    lines = run(["lens R", "tick 1000", "settings " + FC_ACQUIRE.hex(), "msg " + write.hex(),
                 "settings " + control(OP_FLAGS_SET, FLAG_CACHE_KEEP).hex(),
                 "tick 200000", "settings " + control(OP_TELEMETRY, 5).hex(), "dmg",
                 "settings " + FC_ACQUIRE.hex(), "settings " + control(OP_CACHE_INFO, 6).hex(), "dmg",
                 "tick 400000", "settings " + control(OP_TELEMETRY, 7).hex()])
    out = sends(lines); d = [x for x in lines if x.startswith("dmg ")]
    check("CACHE_KEEP armed: the lapse clears the flags and keeps the cache (the keep is latched)",
          (out[1], d[0].split()[4]), (f"send 1 9 {telemetry(5, 200000, 0, 0, 0, 1, gen=1, cache=True).hex()}", "1"))
    check("the fresh acquire after the lapse carries the cache over, same generation and CRC; the latch is spent",
          (out[2], d[1].split()[4]), (f"send 1 9 {telemetry(6, 200000, 0, 0, 90000, 1, gen=1, cache=True, crc=cache_crc).hex()}", "0"))
    check("not re-armed: the next lapse frees the cache (upstream's rule)",
          out[3], f"send 1 9 {telemetry(7, 400000, 0, 0, 0, 1, gen=1).hex()}")
    lines = run(["lens R", "tick 1000", "settings " + FC_ACQUIRE.hex(), "msg " + write.hex(),
                 "settings " + control(OP_FLAGS_SET, FLAG_CACHE_KEEP).hex(), "tick 200000",
                 "settings " + FC_ACQUIRE.hex(), "settings " + control(OP_CACHE_INFO, 8).hex()])
    check("a lapse nobody noticed before the fresh acquire is settled the same way",
          sends(lines)[-1], f"send 1 9 {telemetry(8, 200000, 0, 0, 90000, 1, gen=1, cache=True, crc=cache_crc).hex()}")
    lines = run(["lens R", "tick 1000", "settings " + FC_ACQUIRE.hex(), "msg " + write.hex(),
                 "settings " + control(OP_FLAGS_SET, FLAG_CACHE_KEEP).hex(),
                 "settings " + FC_RELEASE.hex(), "settings " + FC_ACQUIRE.hex(), "settings " + control(OP_TELEMETRY, 9).hex()])
    check("FB_RELEASE under CACHE_KEEP keeps the cache for the next acquire too",
          sends(lines)[-1], f"send 1 9 {telemetry(9, 1000, 0, 0, 90000, 1, gen=1, cache=True).hex()}")
    lines = run(["lens R", "tick 1000", "settings " + FC_ACQUIRE.hex(), "msg " + write.hex(),
                 "settings " + control(OP_FLAGS_SET, FLAG_CACHE_KEEP).hex(), "msg 0b",
                 "settings " + control(OP_TELEMETRY, 10).hex()])
    check("mode 11 (the hand-back to stock) frees the cache regardless",
          sends(lines)[-1], f"send 1 9 {telemetry(10, 1000, 0, 0, 0, 1, gen=1).hex()}")
    lines = run(["lens R", "tick 1000", "settings " + FC_ACQUIRE.hex(), "msg " + write.hex(),
                 "settings " + control(OP_FLAGS_SET, FLAG_CACHE_KEEP).hex(),
                 "tick 200000", "settings " + control(OP_TELEMETRY, 21).hex(),     # the lapse is noticed and settled
                 "settings " + FC_RELEASE.hex(),                                    # a release after it: settled once, the latch stands
                 "settings " + FC_ACQUIRE.hex(), "settings " + control(OP_CACHE_INFO, 22).hex()])
    check("a release after a lapse already settled does not drop the latch: the cache still carries over",
          sends(lines)[-1], f"send 1 9 {telemetry(22, 200000, 0, 0, 90000, 1, gen=1, cache=True, crc=cache_crc).hex()}")

    print("-- the self-test (mode 16)")
    zero_crc = zlib.crc32(bytes(153600)) & 0xFFFFFFFF
    lines = run(["lens R", "tick 1000", "msg 1000", "settings " + FC_ACQUIRE.hex(), "msg 1000", "dmg",
                 "msg 1001" + "0c" + "0000" + "0100" + "aa", "dmg",          # a step carrying a mode-12 write: refused
                 "msg 1001" + kf, "dmg", "crc",
                 "msg 1002", "msg 1001" + kf, "dmg"])
    d = [dmg(lines[:i + 1]) for i, l in enumerate(lines) if l.startswith("dmg ")]
    crc_line = [l for l in lines if l.startswith("crc ")][0].split()
    check("begin needs the lease", rcs(lines)[:2], [-1, 0])
    check("a step with a non-drawing message (a cache write) is refused and counted",
          (rcs(lines)[2], d[1]["stSeq"], d[1]["stRefused"], d[1]["stCrc"], d[1]["cache"]), (-1, 1, 1, zero_crc, 0))
    check("a keyframe step lands in the scratch shadow: the live shadow is untouched and nothing is presented",
          (rcs(lines)[3], d[2]["stSeq"], d[2]["stRefused"], d[2]["stCrc"] != zero_crc, crc_line[1], crc_line[4], d[2]["presents"]),
          (0, 2, 0, True, f"{zero_crc:08x}", "0", 0))
    check("after end, a step is refused until the next begin", (rcs(lines)[4:], d[3]["stSeq"]), ([0, -1], 2))
    check("a refused step is also the last image-lane refusal: fields 23-25 name the step's mode and reason (mode 12 as a step: 8); after the end, the scratch (7)",
          ((d[0]["refMode"], d[0]["refReason"]), (d[1]["refMode"], d[1]["refReason"], d[1]["refSeq"]), (d[3]["refMode"], d[3]["refReason"])), ((16, 3), (12, 8, 0), (16, 7)))
    lines = run(["lens R", "tick 1000", "settings " + FC_ACQUIRE.hex(), "msg 1000", "msg 1001" + kf,
                 "settings " + control(OP_TELEMETRY, 12).hex(), "dmg", "tick 200000", "msg 1001" + kf, "dmg"])
    d = [dmg(lines[:i + 1]) for i, l in enumerate(lines) if l.startswith("dmg ")]
    check("TELEMETRY carries the step count, the refusal and the scratch CRC (fields 20-22)",
          sends(lines)[-1], f"send 1 9 {telemetry(12, 1000, 0, 0, 90000, 1, st=(1, 0, d[0]['stCrc'])).hex()}")
    check("the lease's lapse releases the scratch: the next step is refused and not counted", (rcs(lines)[-1], d[1]["stSeq"]), (-1, 1))

    print("-- the self-test: what a step may not reach (2026-09-14 review)")
    lines = run(["lens R", "tick 1000", "settings " + FC_ACQUIRE.hex(), "msg " + kf, "dmg", "crc", "msg 1000",
                 "msg 100103", "dmg",                                  # a mode-3 message of one byte
                 "msg 100106", "dmg",                                  # a mode-6 message of one byte
                 "msg 1001" + "08" + "01" + "0100" + "03", "dmg",      # a batch whose one sub-message is [03]
                 "crc"])
    d = [dmg(lines[:i + 1]) for i, l in enumerate(lines) if l.startswith("dmg ")]
    c0, c1 = [l.split() for l in lines if l.startswith("crc ")]
    check("a mode-3 or mode-6 message too short for its header, alone or inside a batch, is refused and counted, the scratch untouched",
          (rcs(lines)[-3:], [(x["stSeq"], x["stRefused"], x["stCrc"]) for x in d[1:]]),
          ([-1, -1, -1], [(1, 1, zero_crc), (2, 1, zero_crc), (3, 1, zero_crc)]))
    check("the live direct frame stays active and the stock BMP loader is never reached from a step (the normal path's fallback is closed there)",
          (d[0]["directActive"], [x["directActive"] for x in d[1:]], c0[8], c1[8], c1[1] == c0[1], c1[4]),
          (1, [1, 1, 1], "0", "0", True, "1"))

    print("-- F1.3: the mark across the display task's panel-off path (2026-09-14 review)")
    lines = run(["lens R", "tick 1000", "settings " + FC_ACQUIRE.hex(), "settings " + control(OP_FLAGS_SET, FLAG_PRESENTED).hex(),
                 "panel 0", "msg " + kf, "dmg",                        # the copy runs, the refresh does not
                 "panel 1", "refresh", "dmg", "refresh", "dmg"])       # the first refresh after transfers the preserved frame
    d = [dmg(lines[:i + 1]) for i, l in enumerate(lines) if l.startswith("dmg ")]
    first_dmg = next(i for i, l in enumerate(lines) if l.startswith("dmg "))
    check("a direct copy while the panel is off is counted, not stamped, not reported",
          (d[0]["presents"], d[0]["transferUs"], len(sends(lines[:first_dmg]))), (1, 0, 1))  # the one send so far: the FLAGS_SET reply
    check("the first refresh once the panel is back transfers the preserved frame: stamped and reported once, with the copy's sequence",
          (d[1]["transferUs"], sends(lines)[1:]), (1234, [f"send 1 9 {presented(1, 0, 1234).hex()}"]))
    check("a second stock refresh carries no mark: nothing more is stamped or sent", (d[2]["transferUs"], len(sends(lines))), (1234, 2))
    lines = run(["lens R", "tick 1000", "settings " + FC_ACQUIRE.hex(), "settings " + control(OP_FLAGS_SET, FLAG_PRESENTED).hex(),
                 "panel 0", "msg " + kf, "tick 200000", "panel 1", "refresh", "dmg"])
    check("after a lapse the stock copy clears the mark: the stock refresh that follows is neither stamped nor reported",
          (dmg(lines)["transferUs"], dmg(lines)["directActive"], len(sends(lines))), (0, 0, 1))

    print("-- the flags at every release point (2026-09-14, second review)")
    lines = run(["lens R", "tick 1000", "settings " + FC_ACQUIRE.hex(), "settings " + control(OP_FLAGS_SET, FLAG_PROBE).hex(),
                 "tick 200000", "settings " + control(OP_TELEMETRY, 30).hex(),      # the lapse is noticed and settled: flags 0
                 "settings " + control(OP_FLAGS_SET, FLAG_PROBE).hex(), "dmg",       # armed again, no lease held
                 "settings " + FC_RELEASE.hex(), "dmg",                              # FB_RELEASE after the settled lapse
                 "settings " + control(OP_TELEMETRY, 31).hex()])
    d = [dmg(lines[:i + 1]) for i, l in enumerate(lines) if l.startswith("dmg ")]
    check("contract 2: a FLAGS_SET after a settled lapse (no lease) is refused with status 3 and arms nothing",
          (sends(lines)[1], d[0]["flags"], d[0]["status"]), (f"send 1 9 {telemetry(30, 200000, 0, 0, 0, 1).hex()}", 0, 3))
    check("FB_RELEASE after a settled lapse leaves the flags clear; the register keeps the refusal",
          (d[1]["flags"], sends(lines)[-1]), (0, f"send 1 9 {telemetry(31, 200000, 0, 3, 0, 1).hex()}"))
    lines = run(["lens R", "tick 5000", "settings " + control(OP_FLAGS_SET, FLAG_PROBE).hex(), "dmg",
                 "settings " + FC_ACQUIRE.hex(), "dmg", "settings " + control(OP_TELEMETRY, 32).hex()])
    d = [dmg(lines[:i + 1]) for i, l in enumerate(lines) if l.startswith("dmg ")]
    check("contract 2: a FLAGS_SET with no lease held is refused with status 3 and arms nothing (Adam's ruling, 2026-09-15)",
          (d[0]["flags"], d[0]["status"], sends(lines)[0], d[1]["flags"], sends(lines)[-1]),
          (0, 3, f"send 1 9 {telemetry(0, 5000, 0, 3, 0, 1).hex()}", 0, f"send 1 9 {telemetry(32, 5000, 0, 3, 90000, 1).hex()}"))
    lines = run(["lens R", "tick 5000", "settings " + FC_ACQUIRE.hex(), "settings " + control(OP_FLAGS_SET, FLAG_PROBE).hex(),
                 "tick 200000", "settings " + control(OP_FLAGS_SET, FLAG_PROBE).hex(), "dmg"])
    check("a FLAGS_SET after an unnoticed lapse is refused the same way (the check settles the lapse first)",
          (dmg(lines)["flags"], dmg(lines)["status"]), (0, 3))

    print("-- §4 op 5 CACHE_SIZE, the v2 cache write, the refusal record, the partial path (Phase 2)")
    def v2write(off4, data): return (bytes([19]) + off4.to_bytes(2, "little") + len(data).to_bytes(2, "little") + data).hex()
    rec = bytes([8, 0, 4, 0]) + bytes([0xFF] * 2)                   # a 8x4 v2 record: 32 pixels of 15 as two 16-runs? no: [cnt|color]: 0xFF = 15 pixels of 15
    rec = bytes([8, 0, 4, 0]) + bytes([0xFF, 0xFF, 0x2F])            # 15 + 15 + 2 = 32 pixels of level 15
    lines = run(["lens R", "tick 1000", "settings " + control(OP_CACHE_SIZE, 128).hex(), "dmg",   # no lease: 3
                 "settings " + FC_ACQUIRE.hex(), "settings " + control(OP_CACHE_SIZE, 0).hex(), "dmg",   # zero: 5
                 "settings " + control(OP_CACHE_SIZE, 161).hex(), "dmg",                           # over budget: 5
                 "settings " + control(OP_CACHE_SIZE, 128).hex(), "dmg",                           # ok
                 "settings " + control(OP_FLAGS_SET, FLAG_DRAW2).hex(),
                 "msg " + v2write(20000, rec), "dmg",                                              # 80,000: inside the 128 KiB
                 "settings " + control(OP_TELEMETRY, 40).hex(),
                 "settings " + control(OP_CACHE_SIZE, 64).hex(),                                   # allocated: 4
                 "settings " + control(OP_CACHE_INFO, 41).hex()])
    out = sends(lines); ds = [dmg(lines[:i + 1]) for i, l in enumerate(lines) if l.startswith("dmg ")]; d = ds[-1]
    crc128 = zlib.crc32(bytes(80000) + rec + bytes(128 * 1024 - 80000 - len(rec))) & 0xFFFFFFFF
    check("CACHE_SIZE: status 3 with no lease, 5 for zero and for 161 KiB, 0 for 128 KiB",
          [x["status"] for x in ds[:4]], [3, 5, 5, 0])
    check("the first v2 write allocates the 128 KiB and lands at 80,000: TELEMETRY reports the size in field 18",
          (rcs(lines), d["cacheBytes"], d["gen"], out[5]), ([0], 131072, 1, f"send 1 9 {telemetry(40, 1000, FLAG_DRAW2, 0, 90000, 1, gen=1, cache=True, size=131072).hex()}"))
    check("CACHE_SIZE once allocated: status 4, the size stands; CACHE_INFO's CRC covers the whole 128 KiB",
          out[7], f"send 1 9 {telemetry(41, 1000, FLAG_DRAW2, 4, 90000, 1, gen=1, cache=True, size=131072, crc=crc128).hex()}")
    lines = run(["lens R", "tick 1000", "settings " + FC_ACQUIRE.hex(), "settings " + control(OP_FLAGS_SET, FLAG_DRAW2).hex(),
                 "msg " + v2write(20000, rec), "dmg"])
    check("without op 5 the first write allocates the 64 KiB and a write at 80,000 is refused (reason 5)",
          (rcs(lines), dmg(lines)["cache"], dmg(lines)["refMode"], dmg(lines)["refReason"]), ([-1], 0, 19, 5))
    lines = run(["lens R", "tick 1000", "settings " + FC_ACQUIRE.hex(), "msg " + v2write(16, rec), "dmg",
                 "settings " + control(OP_FLAGS_SET, FLAG_DRAW2).hex(), "msg " + v2write(16, rec), "dmg",
                 "msg " + kf, "msg 15" + "0000" + "0000" + "0800" + "0800" + "0f", "dmg",                 # a fill at (0,0 8x8) level 15
                 "msg 15" + "0000" + "0000" + "0800" + "0800" + "10", "dmg",                              # level 16: refused 12, seq 2
                 "settings " + control(OP_TELEMETRY, 42).hex(),
                 "msg 0700", "dmg"])                                                                     # mode 7 sub 0 clears the record
    d = [dmg(lines[:i + 1]) for i, l in enumerate(lines) if l.startswith("dmg ")]
    check("a v2 op with DRAW2 unarmed is refused with reason 9; armed, it runs",
          ((d[0]["refMode"], d[0]["refReason"]), rcs(lines)[:2], d[1]["gen"]), ((19, 9), [-1, 0], 1))
    check("a fill presents (a direct frame); a bad level is refused with reason 12 and the copy sequence at the time",
          (rcs(lines)[2:5], d[2]["presents"], (d[3]["refMode"], d[3]["refReason"], d[3]["refSeq"])), ([0, 0, -1], 2, (21, 12, 2)))
    check("TELEMETRY carries the refusal (fields 23-25) until mode 7 sub 0 clears it",
          (sends(lines)[-1], d[4]["refSeen"]),
          (f"send 1 9 {telemetry(42, 1000, FLAG_DRAW2, 0, 90000, 1, worker=0, transfer=1234, presents=2, gen=1, cache=True, ref=(21, 12, 2)).hex()}", 0))   # worker 0: the refused fill's own dispatch
    def batch(*subs): return (bytes([8, len(subs)]) + b"".join(len(s).to_bytes(2, "little") + s for s in subs)).hex()
    hint = lambda y0, y1: bytes([24]) + y0.to_bytes(2, "little") + y1.to_bytes(2, "little")
    fill = bytes([21]) + bytes([0, 0, 100, 0, 0x80, 2, 40, 0, 6])      # (0,100 640x40) level 6
    lines = run(["lens R", "tick 1000", "settings " + FC_ACQUIRE.hex(), "settings " + control(OP_FLAGS_SET, FLAG_DRAW2 | FLAG_PRESENTED).hex(),
                 "msg " + kf, "dmg",
                 "msg " + batch(hint(100, 139), fill), "dmg",              # rows 100..139 through the partial entry
                 "msg " + batch(fill), "dmg",                              # no hint: the full refresh
                 "ops a6ng", "msg " + batch(hint(100, 139), fill), "dmg",  # an A6N-G pair: the full refresh, the entry never called
                 "ops none", "msg " + batch(hint(100, 139), fill), "dmg",
                 "settings " + control(OP_TELEMETRY, 43).hex()])
    d = [dmg(lines[:i + 1]) for i, l in enumerate(lines) if l.startswith("dmg ")]
    p = [x for x in sends(lines) if "8a07" in x]
    check("mode 24 in a batch on a JBD4010 record: the present goes through the partial entry (40 rows: 4,300 µs), path 1; without a hint the full refresh, path 0",
          ([(x["lastPath"], x["partials"], x["transferUs"]) for x in d[1:3]], rcs(lines)),
          ([(1, 1, 4300), (0, 1, 1234)], [0, 0, 0, 0, 0]))
    check("on an A6N-G record, or none, the hint is accepted and the full refresh runs (the record's partial entry is never called)",
          [(x["lastPath"], x["partials"], x["transferUs"]) for x in d[3:5]], [(0, 1, 1234), (0, 1, 1234)])
    check("the presented notify carries the path as field 6; TELEMETRY the last one as field 26",
          (p[1], p[2], sends(lines)[-1].endswith(field(26, 0).hex())),
          (f"send 1 9 {presented(2, 1234, 4300, path=1).hex()}", f"send 1 9 {presented(3, 4300, 1234).hex()}", True))
    lines = run(["lens R", "tick 1000", "settings " + FC_ACQUIRE.hex(), "settings " + control(OP_FLAGS_SET, FLAG_DRAW2).hex(),
                 "msg " + kf, "msg " + (bytes([23, 0, 0]) + bytes([0, 0, 100, 0, 0x80, 2, 40, 0])).hex(), "dmg",   # capture slot 0
                 "tick 200000", "settings " + control(OP_TELEMETRY, 44).hex(), "dmg",                              # the lapse frees it
                 "settings " + FC_ACQUIRE.hex(), "settings " + control(OP_FLAGS_SET, FLAG_DRAW2).hex(),
                 "msg " + (bytes([23, 1, 0])).hex(), "dmg"])                                                        # restore: empty (7)
    d = [dmg(lines[:i + 1]) for i, l in enumerate(lines) if l.startswith("dmg ")]
    check("a save-under slot holds its rect's bytes and is freed at the lease's lapse; a restore after it is refused (7)",
          (d[0]["slotBytes"], d[1]["slotBytes"], rcs(lines)[-1], (d[2]["refMode"], d[2]["refReason"])), (12800, 0, -1, (23, 7)))

    print("-- the Phase 2 review (2026-09-15)")
    def partial(lines):
        l = [x for x in lines if x.startswith("partial ")][-1].split()
        return [int(v) for v in l[1:]]
    fill_at = lambda y: bytes([21]) + bytes([0, 0, y, 0, 0x80, 2, 10, 0, 6])    # (0,y 640x10) level 6
    lines = run(["lens R", "tick 1000", "settings " + FC_ACQUIRE.hex(), "settings " + control(OP_FLAGS_SET, FLAG_DRAW2).hex(),
                 "msg " + kf,
                 "panel 0", "msg " + batch(hint(0, 9), fill_at(0)), "msg " + batch(hint(100, 109), fill_at(100)), "dmg",
                 "panel 1", "msg " + batch(hint(200, 209), fill_at(200)), "dmg", "partial"])
    d = [dmg(lines[:i + 1]) for i, l in enumerate(lines) if l.startswith("dmg ")]
    check("a hinted frame after frames whose refresh the panel-off path skipped is sent whole: rows 0..9 and 100..109 were never transferred",
          (d[0]["partials"], d[1]["partials"], d[1]["lastPath"]), (0, 0, 0))
    lines = run(["lens R", "tick 1000", "settings " + FC_ACQUIRE.hex(), "settings " + control(OP_FLAGS_SET, FLAG_DRAW2).hex(),
                 "msg " + kf, "panel 0", "msg " + batch(hint(300, 309), fill_at(44)), "panel 1", "refresh", "dmg", "partial"])
    check("the first refresh after the panel is back sends the preserved frame whole, not the skipped frame's hinted rows",
          (dmg(lines)["partials"], dmg(lines)["lastPath"], dmg(lines)["transferUs"]), (0, 0, 1234))
    lines = run(["lens R", "tick 1000", "settings " + FC_ACQUIRE.hex(), "settings " + control(OP_FLAGS_SET, FLAG_DRAW2).hex(),
                 "msg " + kf, "msg " + batch(hint(100, 109), fill_at(100)), "partial",
                 "msg 0702", "msg " + batch(hint(100, 109), fill_at(100)), "partial", "dmg"])
    p = [[int(v) for v in l.split()[1:]] for l in lines if l.startswith("partial ")]
    check("the partial entry is called with a Damage job's own arguments (0, 0, 0, y0, 640, y1)", p[0], [1, 0, 0, 0, 100, 640, 109])
    check("while the diagnostic overlay is drawn into the framebuffer the refresh is full", (p[1][0], dmg(lines)["lastPath"]), (1, 0))
    lines = run(["lens R", "tick 1000", "settings " + FC_ACQUIRE.hex(), "settings " + control(OP_FLAGS_SET, FLAG_DRAW2).hex(),
                 "msg " + kf, "hold 1", "msg " + batch(hint(100, 109), fill_at(100)), "refresh", "partial", "hold 0", "dmg"])
    check("a stock job whose copy takes the pending Damage frame still sends whole 640-column rows (its own arguments are 576x288)",
          partial(lines), [1, 0, 0, 0, 100, 640, 109])
    lines = run(["lens R", "tick 1000", "settings " + FC_ACQUIRE.hex(), "settings " + control(OP_FLAGS_SET, FLAG_DRAW2).hex(),
                 "msg " + kf, "msg 0702", "msg " + batch(hint(100, 109), fill_at(100)), "msg 0701",
                 "msg " + batch(hint(100, 109), fill_at(100)), "partial", "msg " + batch(hint(100, 109), fill_at(100)), "partial"])
    p = [[int(v) for v in l.split()[1:]] for l in lines if l.startswith("partial ")]
    check("the first refresh after the overlay is hidden is full (its rows still show it); the one after is partial again",
          (p[0][0], p[1][0]), (0, 1))
    lines = run(["lens R", "tick 1000", "settings " + FC_ACQUIRE.hex(),
                 "settings " + control(OP_CACHE_SIZE, 63).hex(), "dmg", "settings " + control(OP_CACHE_SIZE, 64).hex(), "dmg",
                 "settings " + control(OP_FLAGS_SET, FLAG_DRAW2).hex(), "msg " + v2write(16000, rec), "dmg"])
    ds = [dmg(lines[:i + 1]) for i, l in enumerate(lines) if l.startswith("dmg ")]
    check("CACHE_SIZE below 64 KiB is refused (status 5): modes 12/13/14 bound their records by the first 64 KiB of any cache",
          (ds[0]["status"], ds[1]["status"], ds[2]["cacheBytes"]), (5, 0, 65536))
    lines = run(["lens R", "tick 1000", "settings " + FC_ACQUIRE.hex(), "settings " + control(OP_FLAGS_SET, FLAG_CACHE_KEEP).hex(),
                 "settings " + control(OP_CACHE_SIZE, 128).hex(), "settings " + FC_RELEASE.hex(), "settings " + FC_ACQUIRE.hex(),
                 "settings " + control(OP_FLAGS_SET, FLAG_DRAW2).hex(), "msg " + v2write(4, rec), "dmg"])
    check("the size asked for goes at a release point even when CACHE_KEEP is latched and no cache was up",
          dmg(lines)["cacheBytes"], 65536)
    lines = run(["lens R", "tick 1000", "msg " + v2write(0xFFFF, rec), "dmg",
                 "settings " + FC_ACQUIRE.hex(), "msg " + v2write(0xFFFF, rec), "dmg", "msg 13", "dmg"])
    d = [dmg(lines[:i + 1]) for i, l in enumerate(lines) if l.startswith("dmg ")]
    check("mode 19 checks the lease (3) and DRAW2 (9) before an entry past the cache (5); an empty list needs them too",
          [(x["refMode"], x["refReason"]) for x in d], [(19, 3), (19, 9), (19, 9)])
    for lens in ("L", "R"):
        lines = run([f"lens {lens}", "tick 1000", "settings " + FC_ACQUIRE.hex(), "settings " + control(OP_FLAGS_SET, FLAG_DRAW2).hex(),
                     "msg " + kf,
                     "msg " + (bytes([0x95]) + bytes([0, 0, 0, 0, 10, 0, 10, 0]) + bytes([0x7b, 2, 0, 0, 10, 0, 10, 0]) + bytes([7])).hex(), "dmg",
                     "msg " + (bytes([0x95]) + bytes([0, 0, 0, 0, 10, 0, 10, 0]) + bytes([0, 0, 0, 0, 20, 0, 10, 0]) + bytes([7])).hex(), "dmg"])
        d = [dmg(lines[:i + 1]) for i, l in enumerate(lines) if l.startswith("dmg ")]
        check(f"lens {lens}: a per-lens pair is checked whole on both lenses — the right rect past the panel, or two sizes, is refused (2) on this lens too",
              (rcs(lines)[1:], (d[0]["refMode"], d[0]["refReason"]), (d[1]["refMode"], d[1]["refReason"])), ([-1, -1], (0x95, 2), (0x95, 2)))
    lines = run(["lens R", "tick 1000", "settings " + FC_ACQUIRE.hex(), "msg 10", "dmg"])
    check("a mode-16 message with no sub-op records its refusal (1)", (dmg(lines)["refMode"], dmg(lines)["refReason"]), (16, 1))

    print("-- the second Phase 2 review (2026-09-15): the panel behind a partial refresh")
    def panel_crc(lines):
        l = [x for x in lines if x.startswith("crc ")][-1].split()
        return {"shadow": l[1], "fb": l[2], "panel": l[10]}
    bad_draw = bytes([17]) + (0xFFFF).to_bytes(2, "little") + bytes([0, 0, 0, 0, 0x0F])   # a record past any cache: refused 5
    # A batch that is refused part-way has already changed the shadow and presents nothing. The
    # rows it changed are not on the panel, so the next hinted present must send the whole frame
    # (FIRMWARE.md §4, mode 24) — otherwise those rows stay stale until some later full refresh.
    lines = run(["lens R", "tick 1000", "settings " + FC_ACQUIRE.hex(), "settings " + control(OP_FLAGS_SET, FLAG_DRAW2).hex(),
                 "msg " + kf, "crc",
                 "msg " + batch(fill_at(0), bad_draw), "dmg", "crc",
                 "msg " + batch(hint(200, 209), fill_at(200)), "dmg", "crc", "partial"])
    d = [dmg(lines[:i + 1]) for i, l in enumerate(lines) if l.startswith("dmg ")]
    c = [panel_crc(lines[:i + 1]) for i, l in enumerate(lines) if l.startswith("crc ")]
    check("a batch refused part-way changes the shadow and presents nothing: the hinted frame after it is sent whole",
          (rcs(lines), d[1]["lastPath"], d[1]["partials"], c[2]["panel"] == c[2]["fb"]), ([0, -1, 0], 0, 0, True))
    # The same rule for stock content: after the lease lapses the stock compositor repaints, so
    # the panel is no longer the previous Damage frame.
    lines = run(["lens R", "tick 1000", "settings " + FC_ACQUIRE.hex(), "settings " + control(OP_FLAGS_SET, FLAG_DRAW2).hex(),
                 "msg " + kf, "msg " + batch(hint(100, 109), fill_at(100)), "partial",
                 "tick 200000", "refresh",
                 "settings " + FC_ACQUIRE.hex(), "settings " + control(OP_FLAGS_SET, FLAG_DRAW2).hex(),
                 "msg " + batch(hint(220, 229), fill_at(220)), "dmg", "crc", "partial"])
    pcalls = [[int(v) for v in l.split()[1:]] for l in lines if l.startswith("partial ")]
    check("a stock repaint puts stock content on the panel: the first hinted frame after it is sent whole",
          (pcalls[0][0], pcalls[1][0], dmg(lines)["lastPath"], panel_crc(lines)["panel"] == panel_crc(lines)["fb"]),
          (1, 1, 0, True))
    # And after an explicit release, whose fresh acquire follows.
    lines = run(["lens R", "tick 1000", "settings " + FC_ACQUIRE.hex(), "settings " + control(OP_FLAGS_SET, FLAG_DRAW2).hex(),
                 "msg " + kf, "settings " + FC_RELEASE.hex(), "settings " + FC_ACQUIRE.hex(),
                 "settings " + control(OP_FLAGS_SET, FLAG_DRAW2).hex(),
                 "msg " + batch(hint(100, 109), fill_at(100)), "dmg", "partial"])
    check("the first hinted frame of a session that follows a release is sent whole",
          (partial(lines)[0], dmg(lines)["lastPath"]), (0, 0))
    # The gate came free with the previous frame still pending: the message is dropped whole and
    # recorded (reason 14), and nothing of it reaches the shadow.
    lines = run(["lens R", "tick 1000", "settings " + FC_ACQUIRE.hex(), "settings " + control(OP_FLAGS_SET, FLAG_DRAW2).hex(),
                 "msg " + kf, "crc", "hold 1", "msg " + batch(hint(100, 109), fill_at(100)),
                 "msg " + batch(fill_at(220)), "dmg", "crc", "hold 0"])
    c = [panel_crc(lines[:i + 1]) for i, l in enumerate(lines) if l.startswith("crc ")]
    check("a gated message that finds the previous frame still pending is dropped whole and recorded (14)",
          (rcs(lines)[-1], (dmg(lines)["refMode"], dmg(lines)["refReason"]), c[0]["shadow"] != c[1]["shadow"]),
          (-1, (8, 14), True))
    # One job's hint belongs to that job's refresh: the worker queues the next frame in the window
    # between a copy and its refresh, and the hint the copy latched is the one that runs.
    lines = run(["lens R", "tick 1000", "settings " + FC_ACQUIRE.hex(), "settings " + control(OP_FLAGS_SET, FLAG_DRAW2).hex(),
                 "msg " + kf, "split 1",
                 "msg " + batch(hint(100, 109), fill_at(100)),      # copied; its refresh waits
                 "msg " + batch(hint(220, 229), fill_at(220)),      # queued in that window
                 "split 0", "partial", "dmg", "crc"])
    pcalls = [[int(v) for v in l.split()[1:]] for l in lines if l.startswith("partial ")]
    check("the hint a copy latched runs with that copy's refresh, not with a later frame's",
          (pcalls[-1][0], panel_crc(lines)["panel"] == panel_crc(lines)["fb"]), (2, True))
    # The panel model itself: a partial refresh transfers its rows and nothing else, so a hint
    # that misses a row the same batch changed leaves that row stale — this is what the phone's
    # own model has to reproduce, and why a short hint fails the oracle before it reaches glass.
    lines = run(["lens R", "tick 1000", "settings " + FC_ACQUIRE.hex(), "settings " + control(OP_FLAGS_SET, FLAG_DRAW2).hex(),
                 "msg " + kf, "crc", "msg " + batch(hint(100, 109), fill_at(100), fill_at(240)), "crc", "dmg",
                 "msg " + batch(fill_at(120)), "crc"])
    c = [panel_crc(lines[:i + 1]) for i, l in enumerate(lines) if l.startswith("crc ")]
    check("a hint that misses a row the batch changed leaves it off the panel; the next full refresh brings the panel back to the framebuffer",
          (dmg(lines)["lastPath"], c[1]["panel"] == c[1]["fb"], c[2]["panel"] == c[2]["fb"]), (1, False, True))

    print("RESULT: " + ("all pass" if not fails else f"{fails} failure(s)"))
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
