#!/usr/bin/env python3
"""Build the x86 host harness and run conformance vectors through the patch sources' C.

Context: host/README.md. The vectors live in Damage (`~/damagewm/firmware/vectors/`,
format in Damage's FIRMWARE.md §9); Damage's Kotlin simulator runs the same files.

    python3 host/run_vectors.py                        # check every vector against its expectations
    python3 host/run_vectors.py --write                # fill the expectations from this C
    python3 host/run_vectors.py --dir PATH v1-cache    # one vector from another directory

A vector carrying "panel": true also has the panel's CRC — what the lens shows, which a
partial refresh (mode 24) updates only in its own rows — as "P" in every expectation.

Each vector runs once per lens in its own harness process (each lens has its own RAM
on the glasses). After every step the harness reports the shadow's CRC-32, the
return code of every message, the last image-lane refusal recorded (Damage
FIRMWARE.md §4, fields 23-25: mode byte, reason, copy sequence; zeros when none) and
the status register (field 4); --write stores them as the step's expectation:
    "expect": {"L": "<crc>", "R": "<crc>", "rc": {"L": [..], "R": [..]}, "ref": {"L": [m, r, s, st], "R": [m, r, s, st]}}
Ops in a step: {"tick": ms}, {"lease": "acquire"|"release"}, {"msg": HEX}, and — Phase 2 —
{"flags": N} (a FLAGS_SET of the whole set N), {"cachesize": KiB} (op 5), both sid-0x09
field-112 requests to the lens, and {"stock": true} (the stock compositor's own repaint).
Exit status is non-zero on any mismatch, a harness error, or a gate imbalance
(the display gate must be given back as often as it is taken).
"""
import json, pathlib, subprocess, sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
OBJ = ROOT / "obj"
BIN = OBJ / "cfw_host"
DEFAULT_DIR = pathlib.Path.home() / "damagewm/firmware/vectors"
LEASE_OP = {"acquire": 5, "release": 6}

def damage_control(op, arg):
    """A sid-0x09 request carrying field 112 ['D','M',1,op,argLo,argHi] (Damage FIRMWARE.md §3)."""
    body = b"DM" + bytes([1, op, arg & 0xFF, (arg >> 8) & 0xFF])
    return (bytes([0x08, 1, 0x10, 0]) + bytes([0x82, 0x07, len(body)]) + body).hex()

def build():
    OBJ.mkdir(exist_ok=True)
    cmds = [
        ["clang", "-m32", "-O1", "-g", "-DCFW_HOST", "-ffreestanding", "-fno-builtin",
         "-Wno-incompatible-library-redeclaration", "-c", str(HERE / "cfw_host_patches.c"), "-o", str(OBJ / "host_patches.o")],
        ["clang", "-m32", "-O1", "-g", "-Wall", "-c", str(HERE / "cfw_host_shim.c"), "-o", str(OBJ / "host_shim.o")],
        ["clang", "-m32", "-no-pie", str(OBJ / "host_patches.o"), str(OBJ / "host_shim.o"), "-lz", "-o", str(BIN)],
    ]
    for c in cmds:
        r = subprocess.run(c, capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f"host build failed: {' '.join(c)}\n{r.stderr}")
        if r.stderr.strip():
            print(r.stderr.strip(), file=sys.stderr)

def run_lens(vec, lens):
    """Returns (crcs per step, rcs per step, refs per step, panel CRCs per step, notes)."""
    lines = [f"lens {lens}"]
    per_step_msgs = []
    for st in vec["steps"]:
        count = 0
        for op in st["ops"]:
            if "tick" in op:
                lines.append(f"tick {int(op['tick'])}")
            elif "lease" in op:
                nonce = 1
                lines.append("control " + bytes([ord('F'), ord('C'), 1, LEASE_OP[op["lease"]], nonce, 0]).hex())
            elif "msg" in op:
                lines.append(f"msg {op['msg']}")
                count += 1
            elif "flags" in op:
                lines.append("settings " + damage_control(2, int(op["flags"])))
            elif "cachesize" in op:
                lines.append("settings " + damage_control(5, int(op["cachesize"])))
            elif "stock" in op:
                # the stock compositor's own repaint (Damage FIRMWARE.md §9): its copy puts stock
                # content in the framebuffer and the refresh that follows sends it to the panel
                lines.append("refresh")
            else:
                sys.exit(f"{vec['name']}: unknown op {op}")
        lines.append("crc")
        lines.append("dmg")
        per_step_msgs.append(count)
    r = subprocess.run([str(BIN)], input="\n".join(lines) + "\n", capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"{vec['name']} lens {lens}: harness exited {r.returncode}\n{r.stderr}")
    out = [l for l in r.stdout.splitlines() if l.strip()]
    errors = [l for l in out if l.startswith("error")]
    if errors:
        sys.exit(f"{vec['name']} lens {lens}: {errors}")
    crcs, rcs, refs, panels, notes = [], [], [], [], []
    it = iter(out)
    for n in per_step_msgs:
        step_rcs = []
        for _ in range(n):
            l = next(it)
            while l.startswith("send "): l = next(it)      # a control op's reply (RIGHT)
            assert l.startswith("rc "), l
            step_rcs.append(int(l.split()[1]))
        c = next(it).split()
        while c[0] == "send": c = next(it).split()
        assert c[0] == "crc", c
        takes, gives = map(int, c[6].split("/"))
        if takes != gives:
            notes.append(f"gate taken {takes} times, given {gives}")
        d = next(it).split()
        assert d[0] == "dmg", d
        crcs.append(c[1])
        panels.append(c[10])                              # what the lens shows (Damage FIRMWARE.md §9)
        rcs.append(step_rcs)
        refs.append(([int(d[13]), int(d[14]), int(d[15])] if d[12] != "0" else [0, 0, 0]) + [int(d[2])])
    return crcs, rcs, refs, panels, notes

def main(argv):
    args = list(argv)
    write = "--write" in args
    if write: args.remove("--write")
    vdir = DEFAULT_DIR
    if "--dir" in args:
        i = args.index("--dir"); vdir = pathlib.Path(args[i + 1]); del args[i:i + 2]
    files = sorted(vdir.glob("*.json"))
    if args: files = [f for f in files if f.stem in args]
    if not files: sys.exit(f"no vectors in {vdir}")
    build()
    bad = 0
    for f in files:
        bad_before = bad
        vec = json.loads(f.read_text())
        res = {lens: run_lens(vec, lens) for lens in ("L", "R")}
        for lens, (_, _, _, _, notes) in res.items():
            for n in notes:
                print(f"  FAIL  {vec['name']} {lens}: {n}"); bad += 1
        for i, st in enumerate(vec["steps"]):
            got = {"L": res["L"][0][i], "R": res["R"][0][i], "rc": {"L": res["L"][1][i], "R": res["R"][1][i]},
                   "ref": {"L": res["L"][2][i], "R": res["R"][2][i]}}
            # a vector marked "panel" is compared on what the lens SHOWS as well as on the shadow:
            # a partial refresh (mode 24) transfers only its own rows (Damage FIRMWARE.md §9)
            if vec.get("panel"): got["P"] = {"L": res["L"][3][i], "R": res["R"][3][i]}
            if write:
                st["expect"] = got
            elif st.get("expect") != got:
                bad += 1
                print(f"  FAIL  {vec['name']} step {i}: expected {st.get('expect')} got {got}")
        if write:
            f.write_text(json.dumps(vec, indent=1) + "\n")
            print(f"  wrote {f.name}: {len(vec['steps'])} steps")
        elif bad == bad_before:
            # `bad` is cumulative: reading it bare meant one early mismatch silenced the PASS line
            # of every vector after it, so nothing said the rest had run (2026-09-15, the third
            # review; run_self_test.py already had this shape)
            print(f"  PASS  {vec['name']}: {len(vec['steps'])} steps, both lenses")
    if bad:
        print(f"RESULT: {bad} mismatch(es)"); return 1
    print("RESULT: " + ("expectations written" if write else "every vector matches"))
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
