# JamesDSP vendor audio effect for crDroid / Pixel 10 Pro — rebuilt engine

`libjamesdspaidl.so` is the single vendor-partition AIDL audio effect that the
Android audio HAL loads for JamesDSP on crDroid (Pixel 10 Pro "blazer", Tensor G5).
This directory holds drop-in builds of a modified engine, plus the stock file for
comparison. Everything is built from source in the three repos listed at the end.

## Variants

| dir | what | sha256 (first 8) |
|---|---|---|
| `audio/` | **audio-only**: the DSP changes below, nothing else. No Lua interpreter, no analysis meter, no file I/O from inside the effect. | see `audio/SHA256SUMS` |
| `full/` | audio-only + embedded Lua 5.4 bridge + 108-band chroma meter written to a shared file for a live-wallpaper consumer. Only useful with that consumer. | see `full/SHA256SUMS` |
| `full-shipped/` | the `full` build as shipped in the 2026-10-10 OTA (11 days daily use before this release) | e041d142 |
| `stock/` | crDroid's original prebuilt, the baseline for every number below | 3005729e |

Both variants are built from one source tree; `full` is the CMake option
`JDSP_LUA_BRIDGE=ON`. The C struct layout shared with the AIDL wrapper is identical
in both, so either file drops in without a header re-sync.

## What changed vs stock (engine)

1. **Faithful LSP-plugins ports** of Gate, Compressor and Limiter replace the previous
   hand-written wrappers around the bare `lsp::dspu` primitives. The DSP primitive
   classes are compiled unchanged from the pinned lsp-plugins submodules; the ~1700
   lines of plugin-level logic (`update_settings()` / `process()`) are transliterated
   from the real `plug/*.cpp`. Scope: stereo mode, internal sidechain only.
2. **Limiter / post-gain tail actually runs.** Stock's real playback path called an
   incomplete duplicate of the process function that skipped the output limiter and
   post gain entirely.
3. **ARMv9 SVE2 batched FFT** (`fft_x2`, dual-buffer twiddle stages) for the stereo
   convolver, bit-exact vs the scalar reference at all 7 sizes, with a build-level and
   in-source `fp-contract=off` guard (clang otherwise fuses to FMA under `-Ofast` and
   breaks the 1e-4 tolerance).
4. **Realtime scheduling** for the two-stage convolver's tail-partition worker threads
   (the actual cause of convolver crackle under DVFS, not the FFT math).
5. **Locking**: double-buffered lock-free config swap for Gate/Compressor, chain-reset
   use-after-free fix, limiter configure/process lock (this release), zero-length
   vendor-parameter guard (this release).
6. FTZ/DAZ denormal flush on the processing thread.

## Verification (details in `../bench/`)

- **Differential test against the real LSP plugins** (`bench/difftest/`): the same
  48 kHz signals through this `.so` on the phone (real AIDL/FMQ path) and through the
  Fedora `lsp-plugins-ladspa` build of the same plugins on a PC with identical settings.
  RESULTS_PLACEHOLDER
- **On-device CPU / power** (`bench/`): audio HAL process CPU and battery draw while
  playing the same track with the same user preset, stock vs audio-only vs full.
  BENCH_PLACEHOLDER
- **FFT cycle counts**: `simpleperf stat` on Tensor G5, 27/27 bit-exact, 38.7–53 % fewer
  instructions, 1.4–1.9x wall-clock per FFT size (2026-08-08).

## Known limitations (found in the 2026-10-21 review, left as is)

- Gate, Compressor and Limiter take the engine's global mutex per block, the same one
  the convolver IR / DDC / LiveProg loaders hold while loading. Loading a long IR
  during playback can underrun. This is stock's own locking design.
- The limiter "knee" control has no effect: `dspu::Limiter` only consults it in ALR
  mode, which this port pins off (the real plugin's default).
- The three dynamics handles are created for 48 kHz. The Pixel audio HAL always runs
  the output at 48 kHz; on a 44.1 kHz output all time constants would run ~8.8 % long.
- A bypassed Gate/Compressor stops updating its detector (zero cost); the real plugin
  keeps the detector running. Re-enable starts from stale envelope for ~10 ms.

## Install

Flash as an APatch/KernelSU module that overlays `/vendor/lib64/soundfx/libjamesdspaidl.so`,
or bind-mount it for a RAM-only trial (`bench/swap_effect.sh`). Built for crDroid 16
on blazer; other devices need the AIDL wrapper rebuilt in their own vendor tree.

Proof of concept, not maintained. Sources: RootlessJamesDSP fork (app + CMake host),
JamesDSPManager fork (engine), proprietary_vendor_JamesDSP fork (AIDL wrapper, Soong
glue, these builds). Licenses: GPL-3.0 (JamesDSP), LGPL-3.0 (lsp-plugins).
