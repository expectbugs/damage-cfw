# damage-cfw — the Damage build of the G2 custom firmware

A fork of [g2flash](https://github.com/jimrandomh/g2flash) at `a5d1c31` (stock base 2.2.6.10),
GPL-3.0 like its upstream. It is built the same way: the stock image is fetched from Even's CDN at
build time, a reviewed patch list (`patches/cfw_patches.json`) is applied locally, and the result's
hash is pinned. No vendor bytes are redistributed.

This fork serves one consumer, Damage (`~/damagewm`), Adam's window manager for his own glasses.
The plan is `~/damagewm/FORK.md`; the contract both sides implement is `~/damagewm/FIRMWARE.md`;
the conformance vectors live in `~/damagewm/firmware/vectors/`. Damage itself stays clean-room:
it holds no code from this repo, only the contract.

Rules carried over from Damage's `CLAUDE.md`: never flash without Adam's in-the-moment go; the
dry-run staircase first, every time; read every patch source before it is flashed; no new site on a
path that runs before the first radio message; keep the wording plain in every file, comment and
commit.

## Local changes on branch `damage`

- `g2flash.py`: `recover_session()` resets the sequence before authenticating (Damage
  `HANDOFF.md` §10), carried over from the pinned reference clone.
- **The patch set is pinned to this machine's toolchain (2026-09-13).** clang 22.1.8 builds the
  unchanged a5d1c31 sources into output `1920dda6…` (upstream's pin, from a different clang, is
  `d4054ab1…`, the image installed on Adam's glasses since 2026-08-30 and archived in Damage's
  `fws/2.2.6.10-cfw-d4054ab1/`). Same sources, different code layout: every redirected call moves
  with the function offsets. `build_cfw.sh` and `patches/cfw_patches.json` carry the new pin.
- `tools/verify.py`: the offline check before any flashing conversation — stock hash, the pinned
  output, reproducibility with the local clang, openCFW's Thumb-bit audit, the flasher's own size
  guard, and the list of every changed site with its run address and containing function.
- `host/`: the patch sources compiled for 32-bit x86 with the firmware's addresses mapped, and the
  conformance-vector runner (`host/README.md`). The two ARM-assembly entry shims sit under
  `#ifndef CFW_HOST`; the glasses build is byte-identical with or without that guard.
- **`patches/damage_ext.c` (Damage FIRMWARE.md §0/§3, draft, Phase 1, not flashed):** field 110
  `DamageCaps` on every settings READ response; field 112 control ops (TELEMETRY, FLAGS_SET,
  FLAGS_CLEAR) answered with a field-111 telemetry record; flags (only bit 15 PROBE implemented)
  cleared at every texture-cache release point; field 4 of the record is a status register (the
  last recording op's status — TELEMETRY records nothing; a malformed body records 1 and gets no
  reply). The boot count (field 13) is not sent: stock keeps `kvbooCount` only in the KV store
  (Damage `CLAIMS.md`, 2026-09-14). It rides the settings hooks already in place — no new patch
  site (the site list is unchanged). `host/test_damage_ext.py` checks the bytes against the
  contract (11 checks). Pin with it: `f9211ea2…` (2026-09-14; `b88eb6b9…` was the 2026-09-13 build,
  with "this op's status" and an unverified boot-count read). The no-feature baseline (the same
  sources as the installed image, our clang) is commit `6db86e2`, pin `1920dda6…`.

A local convenience: `g2_2.2.6.10.bin` in the repo root may be a symlink to Damage's archived stock
image (`*.bin` is ignored by git).

Remotes: `github` = this fork on GitHub (`https://github.com/expectbugs/damage-cfw`, branch
`damage` tracks it), `origin` = upstream g2flash (fetch only, never pushed to), `reference` = the
pinned local clone at `~/damagewm/reference/g2flash`.
