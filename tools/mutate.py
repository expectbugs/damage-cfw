#!/usr/bin/env python3
"""Which of the patch sources' refusal guards does the gate set actually PROVE?

Context: a personal device, the published patch method, display-rendering work. This is a
measuring tool, not a build step. It never touches the working tree: every mutant is built in a
COPY under a temporary directory, and the real `patches/` and `build_cfw.sh` are only read.

The question it answers is the one a green gate set cannot: for each guard that refuses a
message, would the gates notice if it stopped guarding? It neutralises one guard at a time —
`if (COND) { damage_refuse(...); return -1; }` becomes `if (0) { ... }` — rebuilds the host
harness from the mutated copy and runs the three host gates. A guard no gate reacts to is a
guard nothing proves.

    python3 tools/mutate.py                 # every site
    python3 tools/mutate.py --file damage_draw.c --limit 20
    python3 tools/mutate.py --list          # just enumerate the sites, build nothing

The 2026-09-16 run found 39 of 106 sites caught and 55 reachable ones caught by nothing — the
largest being the lease (3) and DRAW2 (9) checks of six of the eight v2 modes, which
`FIRMWARE.md` §4 opens by requiring (Damage `HANDOFF.md` §64.1). The remaining uncovered list is
what a vector pass should work from. A dozen sites are structurally unreachable on the host (no
shadow, an allocation that cannot fail there); those show as NOT CAUGHT and are named in §64.5.

A mutant that fails to BUILD is reported as such, not counted as caught.

Each mutant is a clang rebuild plus all three gate suites — about a minute — so a full sweep of
the ~96 single-line guards is a couple of hours. Use `--file` and `--limit` while iterating.
Multi-line guards are not matched; the 2026-09-16 count of 106 included those, read by hand.
"""
import argparse, pathlib, re, shutil, subprocess, sys, tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCES = ["damage_draw.c", "damage_ext.c", "texture_cache.c", "zlib_glue.c"]
# a guard and its refusal on one line: `if (COND) { damage_refuse(...); return -1; }`
GUARD = re.compile(r"^(\s*)if\s*\((?P<cond>.+?)\)\s*\{\s*damage_refuse\(.*?\)\s*;\s*return\s+-1\s*;\s*\}\s*$")


def sites(files):
    out = []
    for name in files:
        p = ROOT / "patches" / name
        for i, line in enumerate(p.read_text().splitlines()):
            if GUARD.match(line):
                out.append((name, i, line.strip()))
    return out


def run_gates(work, budget):
    """The three host gates against the copy. Returns (verdict, note), where the verdict is True
    (a gate reacted), False (none did), None (the mutant did not build) or "hung".

    The sweep is the EXTERNAL SUPERVISOR the Three Absolute Rules call for: this is an offline
    measuring tool running a deliberately broken build, not a BLE, render, input or flashing path,
    and a guard whose removal stops the harness returning is a RESULT to report rather than a
    reason to wait for ever. Seen on the first run (2026-09-16): one mutant left `cfw_host`
    spinning and the sweep sat behind it. `--budget` is generous — a clean mutant is about a
    minute — and a hung one is reported, not silently skipped.
    """
    for script in ("run_vectors.py", "run_self_test.py", "test_damage_ext.py"):
        try:
            r = subprocess.run([sys.executable, str(work / "host" / script)],
                               capture_output=True, text=True, cwd=work, timeout=budget)
        except subprocess.TimeoutExpired:
            return "hung", f"{script} did not return in {budget}s"
        if "host build failed" in r.stdout + r.stderr:
            return None, f"build failed ({script})"
        if r.returncode != 0:
            return True, script
    return False, ""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--file", action="append", choices=SOURCES, help="only this source (repeatable)")
    ap.add_argument("--limit", type=int, default=0, help="stop after N sites")
    ap.add_argument("--list", action="store_true", help="enumerate the sites and exit")
    ap.add_argument("--budget", type=int, default=300,
                    help="seconds a single gate may take before the mutant is reported as hung (default 300)")
    a = ap.parse_args()
    found = sites(a.file or SOURCES)
    if a.limit: found = found[:a.limit]
    if a.list:
        for name, i, text in found: print(f"{name}:{i + 1}  {text[:110]}")
        print(f"\n{len(found)} guard(s)")
        return 0

    caught, missed, broken, hung = [], [], [], []
    with tempfile.TemporaryDirectory(prefix="cfw-mutate-") as td:
        work = pathlib.Path(td) / "repo"
        for sub in ("patches", "host", "tools"):
            shutil.copytree(ROOT / sub, work / sub)
        # the base image and the vectors are read from the real tree by absolute path
        for f in ROOT.glob("*.bin"):
            (work / f.name).symlink_to(f.resolve())
        pristine = {n: (work / "patches" / n).read_text() for n in SOURCES}
        for k, (name, i, text) in enumerate(found, 1):
            src = pristine[name].splitlines(keepends=True)
            m = GUARD.match(src[i].rstrip("\n"))
            src[i] = src[i].replace(m.group("cond"), "0", 1)
            (work / "patches" / name).write_text("".join(src))
            hit, note = run_gates(work, a.budget)
            (work / "patches" / name).write_text(pristine[name])       # always restore
            mark = ("HUNG" if hit == "hung" else "BUILD?" if hit is None
                    else "caught" if hit else "NOT CAUGHT")
            print(f"  [{k:3d}/{len(found)}] {mark:10s} {name}:{i + 1}  {text[:80]}"
                  + (f"   ({note})" if note else ""), flush=True)
            bucket = hung if hit == "hung" else broken if hit is None else caught if hit else missed
            bucket.append((name, i + 1, text))

    print(f"\n{len(caught)} guard(s) caught, {len(missed)} NOT caught"
          + (f", {len(broken)} did not build" if broken else "")
          + (f", {len(hung)} left the harness not returning" if hung else ""))
    for label, rows in (("the harness did not return without these", hung),
                        ("these did not build", broken)):
        if rows:
            print(f"\n{label}:")
            for name, line, text in rows: print(f"  {name}:{line}  {text[:100]}")
    if missed:
        print("\nno gate reacts to these:")
        for name, line, text in missed: print(f"  {name}:{line}  {text[:100]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
