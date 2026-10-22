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

1. **Faithful LSP-plugins ports** of Gate, Compressor, Limiter, Expander (new module),
   AutoGain (replaces the legacy integrator), Loudness Compensator (new module,
   ISO 226 / Fletcher-Munson / Robinson-Dadson equal-loudness contours, FFT or IIR
   mode, last stage before the output limiter) and Multiband Compressor (new module,
   up to 8 bands, Classic / Modern / Linear Phase, after the Expander) replace the previous
   hand-written wrappers around the bare `lsp::dspu` primitives. The DSP primitive
   classes are compiled unchanged from the pinned lsp-plugins submodules; the ~1700
   lines of plugin-level logic (`update_settings()` / `process()`) are transliterated
   from the real `plug/*.cpp`. Scope: stereo mode, internal sidechain only. The output
   limiter runs with ALR (automatic level regulation) on, as the real plugin's default;
   the "knee" slider maps onto the ALR knee (-12..0 dB). The output limiter can be
   switched off (default on).
2. **Limiter / post-gain tail actually runs.** Stock's real playback path called an
   incomplete duplicate of the process function that skipped the output limiter and
   post gain entirely.
3. **ARMv9 SVE2 batched FFT** (`fft_x2`, dual-buffer twiddle stages) for the stereo
   convolver, bit-exact vs the scalar reference at all 7 sizes, with a build-level and
   in-source `fp-contract=off` guard (clang otherwise fuses to FMA under `-Ofast` and
   breaks the 1e-4 tolerance).
4. **Realtime scheduling** for the two-stage convolver's tail-partition worker threads
   (the actual cause of convolver crackle under DVFS, not the FFT math).
5. **Locking**: lock-free configuration handoff for all five dynamics modules (the
   control thread publishes a sanitised config, the audio thread applies it in its own
   `update_settings()`, the real LSP host structure); no global mutex on the dynamics
   path any more. Chain-reset use-after-free fix, zero-length vendor-parameter guard.
6. FTZ/DAZ denormal flush on the processing thread.

## Verification (details in `../bench/`)

- **Differential test against the real LSP plugins** (`bench/difftest/`): the same
  48 kHz signals through this `.so` on the phone (real AIDL/FMQ path) and through the
  Fedora `lsp-plugins-ladspa` build of the same plugins on a PC with identical settings.
  Residual vs the real plugin (RMS, dBFS, final binaries, 2026-10-21): gate -110,
  compressor -114, limiter (ALR on) -126, expander -108, autogain -89, loudness
  compensator (FFT mode) -112, multiband compressor -79 (Classic) / -91 (Linear
  Phase). Audio-only and full produce identical output. Two of these sit at the real
  plugin's own float32 floor rather than at the port's: the compensator's IIR mode
  (-30 dB) and the multiband compressor's Classic mode. Proof in both cases: the
  unmodified x86 plugin differs from itself between an AVX2 and an SSE4.2 run by the
  same amount (-31.5 dB IIR; -76.6 dB Classic, -91 dB Linear Phase), the residual
  sitting in the low-frequency biquad sections (poles near |z| = 0.997).
- **On-device CPU / power** (`bench/`): audio HAL process CPU and battery draw while
  playing the same track with the same user preset, stock vs audio-only vs full.
  Pixel 10 Pro, 60 s of the same track, screen off, unplugged, user preset
  (convolver + DDC + graphic EQ + LiveProg on; gate / compander off; output limiter on),
  2026-10-22, `bench/results/*.csv`:

  | variant | audio HAL CPU (% of one core) | whole-phone draw |
  |---|---|---|
  | stock | 60.9 | 2184 mW |
  | audio-only | 23.9 | 1764 mW |
  | full (meter + Lua bridge) | 27.7 | 1824 mW |

  The CPU column is the HAL process's utime+stime over the window; the draw column is
  the battery fuel gauge (everything on the phone, not just audio), one run each. The
  HAL process also runs the player's own `DynamicsProcessing` effect (VLC), about 12 %
  of the HAL's samples in the profile below, identical for all three variants.
- **Profile** (`bench/profile_hal.sh`, `bench/results/perf-full.txt`, simpleperf 30 s,
  full variant, same preset): convolver FFT/overlap-add 28 % (`FFTConvolver2x2Process`,
  `DFT*`, `TwoStageFFTConvolver2x2Process`), per-sample main loop `JamesDSPProcess` 9 %,
  output limiter 6 %, LiveProg (EEL) 3 %, DDC 2.5 %, Lua bridge + meter 2.5 %.
- **FFT cycle counts**: `simpleperf stat` on Tensor G5, 27/27 bit-exact, 38.7–53 % fewer
  instructions, 1.4–1.9x wall-clock per FFT size (2026-08-08).

## Known limitations (found in the 2026-10-21 review, left as is)

- The convolver IR / DDC / LiveProg loaders still take the engine's global mutex while
  loading; the dynamics modules no longer do, but the rest of the chain does. Loading a
  long IR during playback can still underrun. This is stock's own locking design.
- The dynamics handles are created for 48 kHz. The Pixel audio HAL always runs the
  output at 48 kHz; on a 44.1 kHz output all time constants would run ~8.8 % long.
- A bypassed Gate/Compressor/Expander stops updating its detector (zero cost); the real
  plugin keeps the detector running. Re-enable starts from stale envelope for ~10 ms.
- Switching the output limiter off drops its ~5 ms lookahead in one step: one tick.

## Install

Flash as an APatch/KernelSU module that overlays `/vendor/lib64/soundfx/libjamesdspaidl.so`,
or bind-mount it for a RAM-only trial (`bench/swap_effect.sh`). Built for crDroid 16
on blazer; other devices need the AIDL wrapper rebuilt in their own vendor tree.

Proof of concept, not maintained. Sources: RootlessJamesDSP fork (app + CMake host),
JamesDSPManager fork (engine), proprietary_vendor_JamesDSP fork (AIDL wrapper, Soong
glue, these builds). Licenses: GPL-3.0 (JamesDSP), LGPL-3.0 (lsp-plugins).
