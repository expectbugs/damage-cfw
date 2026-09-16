#!/usr/bin/env python3
"""Run the drawing vectors through the self-test path (image mode 16) on the host harness.

Context: host/README.md. Damage FIRMWARE.md §3: the self-test runs a drawing message
against a scratch shadow with presents suppressed and reports the scratch's CRC-32.
This script proves, on the PC, that the self-test path gives the same CRC and the same
return code per step as the normal path the vectors' expectations were written from —
and that the live shadow and the panel never change while it runs. The same files run
through Damage's simulator (`ConformanceVectorTest`, the self-test form).

    python3 host/run_self_test.py                      # every drawing vector, both lenses
    python3 host/run_self_test.py v1-batch v1-copy     # a subset

Vectors that release the lease (v1-lease, v2-cachesize, v2-flags) are skipped: the self-test
needs the lease held throughout (an acquire inside a vector is fine: the runner holds one). A cache write (mode
12 or 19) inside a vector is sent as a LIVE message, not a step — a step writing the cache
is refused by design, and the draws that follow read the live cache (13/14/17/18 do) — so
its return code is the normal path's and the scratch is untouched. The Phase 2 control ops
in a vector ({"flags": N}, {"cachesize": KiB}) go to the lens AT THEIR OWN POSITION, as they do
on the normal path: hoisting them all before the begin (as this did until 2026-09-16) is the same
sequence only while they all precede the first message, and a differential fuzz writes vectors
where they do not.
"""
import json, pathlib, subprocess, sys, zlib

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import run_vectors

SKIP = {"v1-lease"}          # the lapse inside it refuses the step itself, not the message: no self-test form
ZERO_CRC = f"{zlib.crc32(bytes(153600)) & 0xFFFFFFFF:08x}"
FC_ACQUIRE = bytes([ord('F'), ord('C'), 1, 5, 1, 0]).hex()

def run_lens(vec, lens):
    lines = [f"lens {lens}", "tick 1000", "control " + FC_ACQUIRE]
    # A release has no self-test form: the runner holds the lease throughout.
    for st in vec["steps"]:
        for op in st["ops"]:
            if "lease" in op and op["lease"] != "acquire": return None
    lines += ["msg 1000", "crc"]
    per_step = []
    for st in vec["steps"]:
        n = 0
        for op in st["ops"]:
            if "msg" in op:
                mode = int(op["msg"][:2], 16) & 0x7f
                lines.append(("msg " if mode in (12, 19) else "msg 1001") + op["msg"]); n += 1
            elif "tick" in op:
                lines.append(f"tick {int(op['tick'])}")
            elif "flags" in op:
                lines.append("settings " + run_vectors.damage_control(2, int(op["flags"])))
            elif "cachesize" in op:
                lines.append("settings " + run_vectors.damage_control(5, int(op["cachesize"])))
            elif "lease" in op:
                pass                                   # an acquire: the runner holds the lease throughout
            else:
                sys.exit(f"{vec['name']}: op {op} has no self-test form")
        lines += ["dmg", "crc"]
        per_step.append(n)
    r = subprocess.run([str(run_vectors.BIN)], input="\n".join(lines) + "\n", capture_output=True, text=True)
    if r.returncode != 0: sys.exit(f"{vec['name']} lens {lens}: harness exited {r.returncode}\n{r.stderr}")
    out = [l for l in r.stdout.splitlines() if l.strip() and not l.startswith("send ")]
    it = iter(out)
    begin_rc = next(it); assert begin_rc == "rc 0", begin_rc
    first = next(it).split(); assert first[0] == "crc"
    results = []
    for n in per_step:
        rcs = []
        for _ in range(n):
            l = next(it); assert l.startswith("rc "), l
            rcs.append(int(l.split()[1]))
        d = next(it).split(); assert d[0] == "dmg", d
        c = next(it).split(); assert c[0] == "crc", c
        ref = ([int(d[13]), int(d[14]), int(d[15])] if d[12] != "0" else [0, 0, 0]) + [int(d[2])]
        # Fields 21/22 are sent "both after a step" (Damage FIRMWARE.md §3), so before the first
        # one the record carries no CRC and the scratch is the zeroed shadow the begin allocated —
        # which is what the normal path's all-zero shadow hashes to. Every vector in the set draws
        # in its first step, so this only shows on a sequence that does not (2026-09-16 review).
        scratch = d[9] if d[7] != "0" else ZERO_CRC        # d[7] = steps since the begin
        results.append((scratch, rcs, c[1], c[4], ref))    # scratch crc, rcs, live shadow crc, presents, the refusal record
    return results

def main(argv):
    files = sorted(run_vectors.DEFAULT_DIR.glob("*.json"))
    if argv: files = [f for f in files if f.stem in argv]
    run_vectors.build()
    bad = 0
    ran = 0
    for f in files:
        vec = json.loads(f.read_text())
        if vec["name"] in SKIP:
            print(f"  skip  {vec['name']} (lease or cache vector)"); continue
        bad_before = bad
        skipped = False
        for lens in ("L", "R"):
            res = run_lens(vec, lens)
            if res is None:
                print(f"  skip  {vec['name']} (a lease release inside it)"); skipped = True; break
            for i, (crc, rcs, live, presents, ref) in enumerate(res):
                exp = vec["steps"][i]["expect"]
                want_crc, want_rc, want_ref = exp[lens], exp["rc"][lens], exp.get("ref", {}).get(lens)
                # the refusal's copy sequence (ref[2]) is the live count, 0 here where nothing presents: not compared
                if crc != want_crc or rcs != want_rc or (want_ref is not None and (ref[0], ref[1], ref[3]) != (want_ref[0], want_ref[1], want_ref[3])):
                    bad += 1; print(f"  FAIL  {vec['name']} step {i} lens {lens}: scratch {crc} rc {rcs} ref {ref}, the normal path gives {want_crc} rc {want_rc} ref {want_ref}")
                if live != ZERO_CRC or presents != "0":
                    bad += 1; print(f"  FAIL  {vec['name']} step {i} lens {lens}: the live shadow changed ({live}) or a present happened ({presents})")
        if skipped: continue                           # a skipped vector ran nothing: no PASS line (2026-09-15 review)
        ran += 1
        if bad == bad_before: print(f"  PASS  {vec['name']}: {len(vec['steps'])} steps through the self-test, both lenses, nothing presented")
    if ran == 0: bad += 1; print("  FAIL  no vector ran through the self-test")
    print(f"RESULT: {bad} mismatch(es)" if bad else f"RESULT: the self-test path agrees with the normal path ({ran} vectors)")
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
