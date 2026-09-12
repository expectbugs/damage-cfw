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
dry-run staircase first, every time; read every patch source before it is flashed; keep the
appended code off the boot path; keep the wording plain in every file, comment and commit.

Local changes on branch `damage` so far: the one-line flasher fix in `g2flash.py`
(`recover_session()` resets the sequence before authenticating — Damage `HANDOFF.md` §10),
carried over from the pinned reference clone.

Remotes: `origin` = upstream g2flash (fetch only, never pushed to), `reference` = the pinned local
clone at `~/damagewm/reference/g2flash`.
