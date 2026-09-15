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

Vectors that drive the lease or the texture cache (v1-lease, v1-cache) are skipped: the
self-test needs the lease held throughout, and a step writing the live cache is refused
by design (only 3/6/8/9/13/14/15 run; 13/14 would read the live cache).
"""
import json, pathlib, subprocess, sys, zlib

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import run_vectors

SKIP = {"v1-lease", "v1-cache"}
ZERO_CRC = f"{zlib.crc32(bytes(153600)) & 0xFFFFFFFF:08x}"
FC_ACQUIRE = bytes([ord('F'), ord('C'), 1, 5, 1, 0]).hex()

def run_lens(vec, lens):
    lines = [f"lens {lens}", "tick 1000", "control " + FC_ACQUIRE, "msg 1000", "crc"]
    per_step = []
    for st in vec["steps"]:
        n = 0
        for op in st["ops"]:
            if "msg" in op:
                lines.append("msg 1001" + op["msg"]); n += 1
            elif "tick" in op:
                lines.append(f"tick {int(op['tick'])}")
            else:
                sys.exit(f"{vec['name']}: op {op} has no self-test form")
        lines += ["dmg", "crc"]
        per_step.append(n)
    r = subprocess.run([str(run_vectors.BIN)], input="\n".join(lines) + "\n", capture_output=True, text=True)
    if r.returncode != 0: sys.exit(f"{vec['name']} lens {lens}: harness exited {r.returncode}\n{r.stderr}")
    out = [l for l in r.stdout.splitlines() if l.strip()]
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
        results.append((d[9], rcs, c[1], c[4]))            # scratch crc, rcs, live shadow crc, presents
    return results

def main(argv):
    files = sorted(run_vectors.DEFAULT_DIR.glob("*.json"))
    if argv: files = [f for f in files if f.stem in argv]
    run_vectors.build()
    bad = 0
    for f in files:
        vec = json.loads(f.read_text())
        if vec["name"] in SKIP:
            print(f"  skip  {vec['name']} (lease or cache vector)"); continue
        for lens in ("L", "R"):
            for i, (crc, rcs, live, presents) in enumerate(run_lens(vec, lens)):
                exp = vec["steps"][i]["expect"]
                want_crc, want_rc = exp[lens], exp["rc"][lens]
                if crc != want_crc or rcs != want_rc:
                    bad += 1; print(f"  FAIL  {vec['name']} step {i} lens {lens}: scratch {crc} rc {rcs}, the normal path gives {want_crc} rc {want_rc}")
                if live != ZERO_CRC or presents != "0":
                    bad += 1; print(f"  FAIL  {vec['name']} step {i} lens {lens}: the live shadow changed ({live}) or a present happened ({presents})")
        if not bad: print(f"  PASS  {vec['name']}: {len(vec['steps'])} steps through the self-test, both lenses, nothing presented")
    print(f"RESULT: {bad} mismatch(es)" if bad else "RESULT: the self-test path agrees with the normal path")
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
