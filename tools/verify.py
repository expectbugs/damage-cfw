#!/usr/bin/env python3
"""Offline verification of the Damage build of the G2 custom firmware, before any
flashing conversation. Needs no network and no glasses. Stdlib only.

Context: this repo is a fork of the published g2flash firmware build for Adam's own
glasses (see DAMAGE.md). The stock image is Even's, fetched by build_cfw.sh; the
patch list turns it into the custom image. This script proves, on this machine:

  1. the stock image is the base the committed patch set pins
  2. the committed patch set reproduces its pinned output, and build_cfw.sh pins the same hash
  3. the local clang regenerates the committed patch set exactly (reproducibility; --no-regen skips)
  4. the appended code block has no Thumb-bit defect (openCFW's thumb_branch_audit.py)
  5. the main app fits below the OTA flag with its preamble length bumped to match
     (the flasher's own check_mainapp_fits_mram, not a reimplementation)
  6. the list of changed sites, each with its run address and, when the decompile
     corpus is present, the function it sits in — the review aid for FORK.md's
     "no boot-path sites" rule; nothing here judges the list, a person reads it

    python3 tools/verify.py                       # stock image at ./g2_2.2.6.10.bin
    python3 tools/verify.py --stock PATH --no-regen

Exit status is non-zero if any check fails. A pass does NOT authorise flashing:
that takes Adam's in-the-moment go and the dry-run staircase (DAMAGE.md).
"""
import hashlib, json, os, pathlib, re, subprocess, sys, tempfile, importlib.util

ROOT = pathlib.Path(__file__).resolve().parent.parent
PATCHES = ROOT / "patches"
PATCH_JSON = PATCHES / "cfw_patches.json"
BUILD_SH = ROOT / "build_cfw.sh"
FLASHER = ROOT / "g2flash.py"
DEFAULT_STOCK = ROOT / "g2_2.2.6.10.bin"
DEFAULT_AUDIT = pathlib.Path.home() / "damagewm/reference/evenRealities-openCFW/g2/tools/thumb_branch_audit.py"
DEFAULT_CORPUS = pathlib.Path.home() / "damagewm/reference/evenRealities-openCFW/g2/research/corpus/apollo-main/ghidra/decomp/functions.jsonl"

def sha(b): return hashlib.sha256(b).hexdigest()

def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod

class Report:
    def __init__(self): self.fails = []
    def check(self, label, ok, detail=""):
        print(f"  {'PASS' if ok else 'FAIL'}  {label}" + (f"\n          {detail}" if detail and not ok else ""))
        if not ok: self.fails.append(label)
    def note(self, text): print(f"  note  {text}")

def main(argv):
    args = list(argv)
    def opt(name, default):
        if name in args:
            i = args.index(name); v = args[i + 1]; del args[i:i + 2]; return v
        return default
    stock_path = pathlib.Path(opt("--stock", str(DEFAULT_STOCK)))
    audit_path = pathlib.Path(opt("--audit", str(DEFAULT_AUDIT)))
    corpus_path = pathlib.Path(opt("--corpus", str(DEFAULT_CORPUS)))
    regen = "--no-regen" not in args
    r = Report()

    sys.path.insert(0, str(PATCHES))
    apply_mod = load_module("apply_patches", PATCHES / "apply_patches.py")
    pc = load_module("patch_compress", PATCHES / "patch_compress.py")
    spec = json.loads(PATCH_JSON.read_text())

    print("== 1. stock base ==")
    if not stock_path.exists():
        print(f"  FAIL  stock image not found at {stock_path} (run ./build_cfw.sh --skip-venv, or pass --stock)")
        return 1
    stock = stock_path.read_bytes()
    r.check("stock image == the patch set's pinned base", sha(stock) == spec["base_sha256"],
            f"got {sha(stock)}, want {spec['base_sha256']}")

    print("== 2. committed patch set ==")
    out = apply_mod.apply_ops(stock, spec["patches"])
    r.check("applied output == the pinned output hash", sha(out) == spec["output_sha256"],
            f"got {sha(out)}, want {spec['output_sha256']}")
    m = re.search(r'^OUT_SHA256="([0-9a-f]{64})"', BUILD_SH.read_text(), re.M)
    r.check("build_cfw.sh pins the same output hash", bool(m) and m.group(1) == spec["output_sha256"],
            f"build_cfw.sh has {m.group(1) if m else 'no OUT_SHA256'}")

    print("== 3. reproducibility with the local clang ==")
    if not regen:
        r.note("skipped (--no-regen)")
    else:
        ver = subprocess.run(["clang", "--version"], capture_output=True, text=True).stdout.splitlines()
        r.note(f"clang: {ver[0] if ver else 'not found'}")
        with tempfile.TemporaryDirectory() as td:
            tmp_json = pathlib.Path(td) / "regen.json"
            g = subprocess.run([sys.executable, str(PATCHES / "gen_patches.py"), str(stock_path), str(tmp_json)],
                               capture_output=True, text=True)
            if g.returncode != 0:
                r.check("gen_patches.py ran", False, (g.stderr or g.stdout).strip()[-400:])
            else:
                regen_spec = json.loads(tmp_json.read_text())
                same = regen_spec["patches"] == spec["patches"] and regen_spec["output_sha256"] == spec["output_sha256"]
                diff = sum(1 for a, b in zip(regen_spec["patches"], spec["patches"]) if a != b)
                r.check("the local clang regenerates the committed patch set exactly", same,
                        f"{diff} entr{'y' if diff == 1 else 'ies'} differ; regenerated output {regen_spec['output_sha256'][:16]}… "
                        f"vs committed {spec['output_sha256'][:16]}… — a different clang, or sources changed without "
                        "./build_cfw.sh --update-patches")

    print("== 4. Thumb-bit audit of the appended block ==")
    idx, comp_off, old_ps = pc.find_mainapp(stock)
    append_ops = [p for p in spec["patches"] if p["old"] == ""]
    if len(append_ops) != 1:
        r.check("exactly one append entry", False, f"found {len(append_ops)}")
    elif not audit_path.exists():
        r.check("audit script present", False, f"not found at {audit_path} (pass --audit)")
    else:
        base = pc.mram_addr(old_ps)
        blob = bytes.fromhex(append_ops[0]["new"])
        with tempfile.TemporaryDirectory() as td:
            bp = pathlib.Path(td) / "blob.bin"
            bp.write_bytes(blob)
            a = subprocess.run([sys.executable, str(audit_path), str(bp), "--base", hex(base)],
                               capture_output=True, text=True)
        lines = [l for l in a.stdout.splitlines() if l.strip()]
        bad = [l for l in lines if "-> ARM" in l or "MISSING" in l.upper()]
        total = sum(1 for l in lines if l.rstrip().endswith("Thumb"))
        r.check(f"{len(blob):,}-byte block at 0x{base:08x}: {total} constant interworking branches, all Thumb",
                a.returncode == 0 and not bad and total > 0, "\n          ".join(bad) or a.stderr.strip())

    print("== 5. the main app fits below the OTA flag ==")
    try:
        fl = load_module("g2flash_mod", FLASHER)
        segs = fl.validate_firmware(out)
        fl.check_mainapp_fits_mram(out, segs)
        s = next(x for x in segs if x["fn"] == fl.REQUIRED_SEGMENT)
        end = fl.APP_LOAD_ADDR + s["ps"] - fl.APP_PREAMBLE
        r.check(f"preamble length matches the payload ({s['ps']:,} B); ends 0x{end:08x}, "
                f"{(fl.OTA_FLAG_ADDR - end) // 1024} KB below the OTA flag", True)
    except Exception as exc:                 # report, never mask
        r.check("check_mainapp_fits_mram", False, str(exc))

    print("== 6. changed sites (for a person to review) ==")
    funcs = []
    if corpus_path.exists():
        for line in corpus_path.open():
            j = json.loads(line)
            for lo, hi in j["ranges"]:
                funcs.append((int(lo, 16), int(hi, 16), j["name"]))
        funcs.sort()
    def owner(addr):
        for lo, hi, name in funcs:            # small list; linear is fine offline
            if lo <= addr <= hi: return name
        return "-"
    app_start = comp_off + 128
    for p in spec["patches"]:
        off = p["offset"]
        if p["old"] == "":
            print(f"    append   @file 0x{off:07x}  {len(p['new']) // 2:,} B  {p['desc']}")
        elif app_start + pc.APP_PREAMBLE <= off < app_start + old_ps and "crc" not in p["desc"] and "preamble" not in p["desc"]:
            run = off + pc.DELTA
            print(f"    code     @run  0x{run:08x}  in {owner(run):14s} {p['desc']}")
        else:
            print(f"    metadata @file 0x{off:07x}  {p['desc']}")
    if not funcs: r.note(f"decompile corpus not found at {corpus_path}: function names omitted")

    print()
    if r.fails:
        print(f"RESULT: {len(r.fails)} check(s) failed — do not flash.")
        return 1
    print("RESULT: all checks passed. This does NOT authorise flashing (DAMAGE.md).")
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
