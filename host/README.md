# host/ — the patch sources on the PC

**Context for the reader.** This repo builds the Damage version of the G2 custom firmware for
Adam's own glasses (`../DAMAGE.md`): the published g2flash method, a small appended code block
and a short list of redirected call sites. This directory compiles those same C sources for the
PC (32-bit x86) so their drawing behaviour can be checked without the glasses. Nothing here is
flashed or sent to a device. It is display-rendering test tooling.

## What it does

- `cfw_host_patches.c` includes `../patches/patches_main.c` unchanged, built with `-DCFW_HOST`.
  The only source difference from the glasses build: the two entry shims written in ARM assembly
  (`snapshot_side`, `faceclaw_evenai_display_entry`) sit under `#ifndef CFW_HOST`; the glasses
  build is byte-identical (checked with `tools/verify.py` step 3).
- `cfw_host_shim.c` maps the firmware's code and RAM address ranges at their exact values and
  writes a small x86 jump at each firmware function address the sources call, to a host function
  with the same prototype. inflate is the host's zlib; the display task's refresh runs
  synchronously (copy hook, the gate given back, then the refresh call through
  `damage_refresh_hook`, whose modeled panel transfer advances the cycle counter by 1,234 µs);
  timers, sound, radio and sensors do nothing. The stock senders' lens rule is modeled: a
  message the code sends from the LEFT lens is refused (return 8) and printed by nothing, as
  `FUN_00475b14` does on the glasses; the `dmg` command reads the Damage extension's state from
  the context instead. `panel 0|1` models the display task's panel-on word (a refresh is skipped
  while it is 0) and `refresh` one stock type-3 refresh with no Damage job pending; the `crc`
  line also counts calls into the stock BMP loader, which the host refuses.
- `run_vectors.py` builds the harness and runs the conformance vectors, one process per lens.
- `test_damage_ext.py` checks `patches/damage_ext.c` (Damage `FIRMWARE.md` §0/§3: DamageCaps, the control
  ops, the telemetry record, flags cleared with the lease, the F1.3 transfer stamp and presented notify,
  F1.5 cache-keep with its generation and CRC, the mode-16 self-test) byte for byte against expectations
  written from the contract text. The harness answers `settings HEX`, `respond HEX` and `dmg` for it and
  prints every message the patch code sends from the right lens.
- `run_self_test.py` runs the drawing vectors through the self-test path (mode 16) and checks that every
  step's scratch CRC and return code equal the normal path's expectations, on both lenses, with the live
  shadow untouched and nothing presented.

## The vectors

They live in Damage (`~/damagewm/firmware/vectors/`, format `FIRMWARE.md` §9). Damage writes their
inputs from the documented message formats (`firmware/make_vectors.py`, clean-room); this harness
fills the expectations (`--write`) and checks them; Damage's `ConformanceVectorTest` runs the same
files through its Kotlin simulator. For v1 (the installed firmware) the C is the reference.

```
python3 host/run_vectors.py --write     # after Damage's make_vectors.py changes the inputs
python3 host/run_vectors.py             # check
python3 host/run_self_test.py           # the same vectors through the self-test path
python3 host/test_damage_ext.py         # the settings extension against the contract
```

Needs clang, a 32-bit libc and a 32-bit zlib (Gentoo multilib: present on beardos).

## Known limits

- zlib: the firmware carries 1.1.4; the host zlib gives the same output for a valid stream, and
  may report a damaged stream differently. The vectors use valid streams.
- The snapshot FIFO runs, but one message at a time: the reassembly race it exists for needs two
  radios and is not modeled here.
- Mode 15 (the firmware's own font) and the BMP path draw nothing here; Damage never sends them.
- The diagnostic overlay stays hidden: its text carries timings and heap figures that vary.
- Timing: the DWT cycle counter advances only in the modeled panel transfer, so the copy and
  worker stamps read 0 unless a present happened inside their region — on the host the worker's
  does (the refresh runs synchronously inside the dispatch), on the glasses it does not.
