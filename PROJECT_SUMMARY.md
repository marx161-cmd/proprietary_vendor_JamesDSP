# JDSP LSP port — project summary

This is a private fork of `crdroidandroid/proprietary_vendor_JamesDSP`, the
crDroid vendor-tree overlay that ships JamesDSP as the system audio effect
(AudioFX/MusicFX override) on the Pixel 10 Pro ("blazer") build. `ddd540f`
("Initial commit") is the pristine, unmodified stock import — everything
after it is this project's work. Use `git show ddd540f:<path>` to recover
any stock file for comparison.

The DSP algorithm side of this work (the actual engine source) lives in two
other private repos, both forked from their real upstreams with `origin`
left pointed at upstream and a `private` remote added for this work:

- `github.com/marx161-cmd/RootlessJamesDSP` (app/frontend, fork of
  `ThePBone/RootlessJamesDSP`)
- `github.com/marx161-cmd/JamesDSPManager` (the `libjamesdsp` engine
  submodule of the above, fork of `ThePBone/JamesDSPManager`) — commit
  `0b1aa0f^` in that repo's history is the exact pre-port commit, i.e. the
  DSP algorithm equivalent of stock: James Fung's original hand-rolled
  Compressor/Gate/Limiter, before any LSP code existed.

## What this project actually is

JamesDSP was already an Android app before this project touched it. What
got ported was the other direction: real LSP (Linux Studio Plugins, a
desktop Linux audio project) Compressor/Gate/Limiter algorithm code —
vendored in as `lsp-dsp-units`/`lsp-dsp-lib`/`lsp-common-lib` source trees
in the JamesDSPManager repo — bridged into the existing Android JamesDSP
engine via `lsp_wrap.cpp`/`chain_modules.c`, replacing James Fung's
original hand-rolled compressor/gate implementations. AutoGain uses
libebur128 rather than an LSP port. Multiband compression is optional;
single-band is the default. A 30Hz Lua scripting bridge was also added
(`lua_bridge.c`) so external tools (e.g. the audio-reactive live wallpaper)
can tap RMS/band-energy/postgain without a second audio session.

The non-negotiable constraint carried through the entire multi-week effort:
**preserve stock's AIDL hook into Android's audio routing exactly.** The
effect must remain a single self-contained drop-in-replacement `.so`
file — matching stock's own architecture — not a wrapper file plus a
separately-deployed engine file that also needs pushing to the vendor
tree. That constraint was violated partway through (the AIDL wrapper and
the DSP engine ended up as two separately-linked shared objects,
`libjamesdspaidl.so` + `libjamesdsp_engine.so`) and fixed on 2026-08-15/16
— see below.

## This repo's own contents (`vendor/JamesDSP`)

- `aidl-wrapper/JamesDspAidlEffect.{h,cpp}` — the AIDL `EffectImpl`-derived
  class (`JamesDspAidlEffect`) that IS stock's hook mechanism, unchanged in
  shape from stock's own architecture. Bridges to the engine via a legacy
  C ABI (`EffectDSPMainConstructor`/`Command`/`Process`) matching
  `jamesdsp.c`'s `EffectDSPMain` struct layout exactly.
- `tools/wav_effect_harness.cpp` + `Android.bp` — standalone offline A/B
  test tool. dlopens any `libjamesdspaidl.so` (stock or current), drives it
  through its real public `IEffect` interface (`open`/`command`/FMQ
  read-write) exactly like the real audio HAL loader does, processes a WAV
  file through it, and reports realtime-multiple + writes the output WAV.
  Build with `m jdsp_wav_effect_harness` from a `lunch lineage_blazer bp4a
  user` environment. Not part of the shipped device image.
- `bench/` — benchmark scripts and a sample A/B run from 2026-08-15/16 (see
  below).
- `Android.bp` — the actual Soong build config. `libjamesdsp_engine` is a
  `cc_prebuilt_library_static` (was `cc_prebuilt_library_shared` before the
  2026-08-16 fix); `libjamesdspaidl` links it in via `whole_static_libs`.

## The single-file merge (2026-08-15/16)

Root cause of the two-file split: the engine's CMake target
(`app/src/main/cpp/CMakeLists.txt` in JamesDSPManager) only built a SHARED
library, so the crDroid Soong side had no choice but to import it as a
`cc_prebuilt_library_shared` and link it as a runtime dependency of the
AIDL wrapper .so — producing two files on the vendor partition instead of
stock's one.

Fix: added a second, additive CMake target, `jamesdsp_static` (`STATIC`,
`POSITION_INDEPENDENT_CODE ON`, output name `jamesdsp_engine_static`),
building the exact same sources as the existing `jamesdsp` SHARED target
(which is untouched — the standalone Gradle app's own JNI wrapper still
needs it). Soong's `libjamesdsp_engine` prebuilt now imports the `.a`
instead of the `.so`, and `libjamesdspaidl` links it with
`whole_static_libs` instead of `shared_libs`.

Verified via `readelf -d` and `nm -D` on the actual Soong-built output
(`vendor/lib64/soundfx/libjamesdspaidl.so`, 2,744,816 bytes): NEEDED list
contains only standard AOSP/AIDL framework libs — no
`libjamesdsp_engine.so` — and `nm -D` shows both the AIDL wrapper symbols
(`JamesDspAidlEffect::kDescriptor`, `createContext`, ...) and the DSP
engine symbols (`autogain_process`, `lsp_comp_process`, ...) present in the
same file. Genuinely self-contained, matching stock's one-file shape.
Size lands close to stock's (2.2MB) — much leaner than the old two-file
split's combined 8.5MB.

## Live functional verification (2026-08-16)

Ran both stock and the newly-merged current `.so` through
`jdsp_wav_effect_harness` on-device (real arm64 binaries, real AIDL/FMQ
data path, SVE2 FFT path active — this can only be tested on the real
Tensor chip, not emulated or cross-compiled elsewhere).

- Stock needed three older AIDL interface-version libraries rebuilt
  on-demand (`android.hardware.audio.effect-V2-ndk`,
  `android.media.audio.common.types-V3-ndk`,
  `android.hardware.audio.common-V3-ndk`) since the live device only ships
  the current V3/V4 set — expected, since stock is a prebuilt binary blob
  from an older upstream crDroid build, not something rebuildable from
  source in this tree.
- Stock's `open()` null-derefs if called with no vendor-extension parameter
  blob at all (`std::nullopt`); current's guards on `specific.has_value()`
  and doesn't. The harness now always passes a minimal valid empty
  `DefaultExtension`/`VendorExtension` blob to `open()` to work with both.
- Both variants processed a 10.5s test signal (log sweep 20Hz-20kHz + pink
  noise) successfully. Current: ~125-130x realtime.
- Spectral (FFT magnitude) comparison of the two outputs: **~0.04 dB
  average delta per band across the whole spectrum**, RMS level matching
  to within 0.0003 dB. Raw sample-difference RMS/peak numbers are NOT
  trustworthy as-is (they read as louder than the signal itself) because
  the two DSP chains have different internal processing latency
  (different lookahead/block alignment between the old hand-rolled
  compressor and the LSP-ported one) — a same-instant sample subtraction
  compares misaligned audio. Needs cross-correlation delay alignment
  before that particular metric means anything; the spectral comparison
  sidesteps the alignment problem and is the trustworthy number for now.
- Compute-cost (CPU%) and power-draw (`current_now`/`voltage_now`)
  comparison scripts are written (`bench/bench_compute.sh`,
  `bench/bench_power.sh`) but not yet run against real playback — next
  step when resuming this work.
- The ARMv9/SVE2 FFT convolver optimization (`codelet.c`'s `_x2` variant,
  gated on `JDSP_SVE2_FFT_X2`/`__ARM_FEATURE_SVE2`) is exercised by this
  on-device test (unlike any x86 host build, where that code doesn't
  compile in at all) but was already separately verified earlier
  (guard-page + bit-exact review, see `project_jdsp_sve2_fft_optimization`
  notes) — not the subject of this particular A/B.

## Current staged state (2026-08-16) — READ THIS BEFORE ASSUMING WHAT'S ON-DEVICE

**What's actually running on the phone right now**: still the OLD stock
two-file split — a thin `libjamesdspaidl.so` (427KB) plus a separate
`libjamesdsp_engine.so`. The single-file merge below (`87f31de`) has
**never been flashed**. Don't assume the merge is live just because it's
committed here — it isn't, until a real OTA ships it.

**What's staged in this repo, not yet built or flashed**:
`proprietary/vendor/lib64/soundfx/libjamesdsp_engine_static.a` was just
regenerated (ninja target `jamesdsp_static` in
`~/builds/android/JamesDSP-src/app/.cxx/RelWithDebInfo/4s520615/arm64-v8a/`,
sha256 `c5e89833...`) from the Gradle source AFTER a `/code-review ultra`
pass found and fixed real plumbing bugs: a chain-toggle use-after-free in
`chain_init_static()`, missing `jdsp_lock`/`jdsp_unlock` around
convolver/DDC/liveprog reload (real audio-thread-vs-AIDL-loader race), and
a dropped post-benchmark convolver rebuild. Full detail in the
`RootlessJamesDSP`/`JamesDSPManager` private repos' own commits
(`d20c7bb`/`d36c8e6`).

**The `libjamesdspaidl.so` sitting in this same directory right now
(2.7MB) is STALE** — it was linked from the pre-fix static lib
(timestamped 23:03, before the fix). It has NOT been regenerated from the
new `.a` above. Don't ship that file as-is.

**Update 2026-08-16, same day**: step 1 below is now DONE and verified —
`m libjamesdspaidl` (isolated, `lunch lineage_blazer bp4a user` first)
built clean in 2m22s, `readelf -d` confirms `libbinder_ndk.so` + the full
real AIDL effect stack in `NEEDED`, `nm -D --defined-only` confirms
`createEffect`/`destroyEffect`/`queryEffect` all present and defined. The
fixed `libjamesdspaidl.so` this produced (sha256 `cc84cece...`,
`out/target/product/blazer/vendor/lib64/soundfx/libjamesdspaidl.so`) is
now copied into this repo's `proprietary/vendor/lib64/soundfx/` as the
staged reference build. **Still not flashed to the device** — this only
proves it builds and links correctly, not that it's live. Steps 2-3 below
remain undone, deliberately, until a real OTA is wanted.

**What's left before this can actually OTA** (deliberately not done yet,
per explicit instruction — build-verify only, no flash this session):
1. ~~`m libjamesdspaidl` (Soong rebuild against the new `.a`) — verify via
   `readelf -d` / `nm -D --defined-only`~~ DONE, see above.
2. Full `blazer-ota-build` (not raw `mka bacon`) → boot-chain-patch →
   `ota_from_target_files` → flash.
3. On-device verify: `su -c 'setprop ctl.restart audioserver'`, check
   logcat for the `ASvc::ExternalLibrary` init pattern documented in
   Step 4 of the same task doc.

## Publishing plan

Intent is to eventually publish this work publicly (with a beta warning)
once benchmarking is further along — "low-compute, pristine" is the
explicit top design goal, so real compute/power numbers should exist
before that claim goes out publicly. This repo is private for now,
specifically so the in-progress state and the two Pixel-specific `.apk`
binaries (stock + current, both included in `proprietary/` for direct
A/B reference) are backed up without being distributed. The public release
(when it happens) should reconsider whether to ship binaries at all, same
question as was already resolved for the `vlc-android` fork this session
(FFmpeg codec licensing — source-only is the safer default for a public
release; this repo being private sidesteps that question for now).
