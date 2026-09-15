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
- `host/`: the patch sources compiled for 32-bit x86 with the firmware's addresses mapped, the
  conformance-vector runner, the self-test runner and the contract tests (`host/README.md`). The
  two ARM-assembly entry shims sit under `#ifndef CFW_HOST`; the glasses build is byte-identical
  with or without that guard.
- **`patches/damage_ext.c` (Damage FIRMWARE.md §0/§3, draft, Phase 1, not flashed):** field 110
  `DamageCaps` on every settings READ response; field 112 control ops (TELEMETRY, FLAGS_SET,
  FLAGS_CLEAR, CACHE_INFO) answered with a field-111 telemetry record; flags cleared at every
  texture-cache release point; field 4 of the record is a status register (the last recording
  op's status — TELEMETRY and CACHE_INFO record nothing; a malformed body records 1 and gets no
  reply). The boot count (field 13) is not sent: stock keeps `kvbooCount` only in the KV store
  (Damage `CLAIMS.md`, 2026-09-14). The Phase 1 candidate's features (2026-09-14 evening):
  - **F1.3, the panel-transfer stamp** — `damage_refresh_hook` replaces the display task's
    `bl FUN_004ca564` at `0x00473ce4` (**the one new patch site**, in `FUN_00473c44`, on a path
    that runs for every stock refresh from boot on: it reads only a validated context and is the
    stock call with its six arguments unchanged unless a Damage frame was just copied). The
    transfer's DWT stamp lands in telemetry fields 15/16; flag bit 0 PRESENTED sends a field-113
    notify after each transfer.
  - **F1.5, cache-keep** — flag bit 1 CACHE_KEEP, read when the flags clear at a lease lapse or
    release, is latched for the fresh acquire that follows, which keeps the texture cache; a
    generation counter (field 17), the size (18) and, on CACHE_INFO, the CRC-32 (19) let the
    phone verify what it uploaded is still there. Mode 11 frees the cache regardless.
  - **the self-test, image mode 16** — `[16][0]` begin, `[16][1][message]` step, `[16][2]` end: a
    drawing message runs through the unchanged dispatcher against a scratch shadow with presents
    suppressed and its own frame-order diagnostics; the scratch CRC-32, the step count and the
    last refusal ride telemetry fields 20–22. `host/run_self_test.py` proves the path gives the
    normal path's CRCs and return codes on every drawing vector, both lenses, nothing presented.

  **Only the RIGHT lens sends** (read at instruction level 2026-09-14): the stock senders
  `FUN_00475b14` (responses; the image acks and the settings responder use it) and `FUN_00475c1a`
  (notifies) both call `FUN_0046f258`, which is `FUN_0045a568() == 2` (left), and return 8 without
  sending. Replies and notifies are built on RIGHT only; the left lens runs every op blind, and the
  host harness's `dmg` command reads what it holds. `host/test_damage_ext.py` checks the bytes
  against the contract (44 checks). Pin with it: **`c5e4f8b7…`** (2026-09-14, night, after the
  second review below; 27 entries, the same one new site; `70e47938…` was the late evening's pin
  after the first review, `5ff9159b…` the evening's first candidate, `f9211ea2…` the morning's
  build without F1.3/F1.5/the self-test, `b88eb6b9…` the 2026-09-13 build). The no-feature
  baseline (the same sources as the installed image, our clang) is commit `6db86e2`, pin `1920dda6…`.

  **Reviewed 2026-09-14, late evening (Damage `HANDOFF.md` §52) — two defects found on the host
  harness and fixed, one ordering guard added:**
  - a self-test step carrying a mode-3 or mode-6 message shorter than its own header (alone or as
    a batch's sub-message) fell through the dispatcher to `load_bmp_fast`, which cleared the live
    `direct_active` (the next stock repaint would have overwritten the Damage frame) and handed the
    stack-built container state to the stock BMP loader. `load_bmp_fast` now refuses while a step
    runs; the step is refused and counted, the scratch and the live frame untouched.
  - a direct copy whose refresh the display task skipped (panel off, `0x00473cca`) left the F1.3
    mark set, and a later stock refresh — after a stock copy, with stock content in the
    framebuffer — was stamped and reported as that Damage frame's transfer. `display_copy_hook`
    clears the mark on its stock-copy path; the preserved-frame path keeps it (the refresh that
    follows does transfer the Damage frame).
  - the self-test's `dmg_st_shadow` / `dmg_st_active` / `dmg_st_free_pending` are volatile: a
    step marks itself active before it reads the scratch pointer and a release from another task
    checks the mark before it frees, and the compiler may not reorder either pair. The window
    between the two is the installed firmware's own for its texture cache.
  The host harness gained `panel 0|1`, `refresh` and a BMP-loader call count so both defects are
  pinned by `host/test_damage_ext.py`; Damage's simulator pins the first (`DamageMsgTest`).

  **Reviewed again 2026-09-14, night (Damage `HANDOFF.md` §53) — one contract deviation found on
  the host harness and fixed, one guard added:**
  - `damage_lease_ended` returned before clearing the flags once the lapse had been settled, so
    a FLAGS_SET the glasses took after a settled lapse (no lease held) survived the FB_RELEASE
    that followed and the next telemetry reply reported it in force. The contract has the flags
    clear at every release point; the settled marker now keeps only the latch. Damage's model
    had the same gap and one more (a lease check with no lease cleared the flags, which the C
    never did); both are fixed and pinned on both sides (three host checks, two `DamageMsgTest`
    pins). A FLAGS_SET with no lease held is taken and stays in force until the next release
    point on both sides — whether it should be refused instead is Adam's call (`FIRMWARE.md` §3).
  - a begin (`[16][0]`) allocated and zeroed the scratch with no active mark, so a release from
    another task in that time would have freed the scratch under the zeroing; it now runs under
    the same mark as a step (`damage_self_test_settle` ends both), and a begin the lease ended
    during is refused with the scratch freed. No observed failure; the window matches the step's.

- **The Phase 2 candidate (2026-09-15 evening, Damage `HANDOFF.md` §60; `FIRMWARE.md` §4), not flashed:**
  `patches/damage_draw.c` adds image-lane modes 17–24 (a per-lens image draw with u16 dimensions, a string draw
  over a 224-entry table, a cache write with 4-byte-unit offsets, a clip and a present hint inside a batch, a
  fill, a LUT over a rect, save-under slots), off until the phone arms flag bit 2; `damage_ext.c` becomes
  contract 2 (a FLAGS_SET or CACHE_SIZE without the lease is refused, status 3), gains op 5 CACHE_SIZE (up to
  160 KiB), telemetry fields 23–25 (the last image-lane refusal: mode byte, reason, copy sequence — every refusal
  on the lane records one, v1 modes included) and 26 (the last transfer's path), and routes a hinted present
  through the JBD4010 record's per-row partial entry (`+0x2C`, the same six arguments as the async entry) from
  the Phase 1 refresh hook — no new site. The three link edits ported from upstream `c63710c` to our 2.2.6.10
  sites (LE 2M in the startup feature command, both fast records at 7.5 ms, the idle slow request bound to the
  fast record) are in `patch_compress.py`. Pin **`55746389…`** after the review of 2026-09-15 night (Damage
  `HANDOFF.md` §61; 31 entries, a 53,256-byte block — clang's inlining moved with the changes — 20 Thumb branches,
  371 KB below the OTA flag; `aacdc63a…` was the candidate before it): op 5 takes 64..160 KiB and a request no longer
  shares the allocated size's field, a per-lens pair is checked whole on both lenses, mode 19 checks length → lease →
  DRAW2 → record, mode 23 inside a step picks the self-test's slots by the step's mark (no swap), mode 16 records its
  refusals, the refusal record is read under a write count, and a hinted present is sent whole after a frame the
  panel-off path never transferred or while the overlay shows, the partial entry called with (0, 0, 0, y0, 640, y1).
  The host harness models the JBD4010 and A6N-G records (`ops`), a held display job (`hold`) and the partial call's
  arguments (`partial`); `test_damage_ext.py` (69 checks), `run_vectors.py` (the `flags`/`cachesize` ops, the `ref`
  expectation) and `run_self_test.py` (a cache write sent live, the control ops before the begin, no PASS for a
  skipped vector) cover it; Damage's simulator matches the C on the 13 v2 vectors.

A local convenience: `g2_2.2.6.10.bin` in the repo root may be a symlink to Damage's archived stock
image (`*.bin` is ignored by git).

Remotes: `github` = this fork on GitHub (`https://github.com/expectbugs/damage-cfw`, branch
`damage` tracks it), `origin` = upstream g2flash (fetch only, never pushed to), `reference` = the
pinned local clone at `~/damagewm/reference/g2flash`.
