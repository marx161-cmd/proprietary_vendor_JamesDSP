# Task B: build wiring — split into engine prebuilt + real AIDL module

## The blocker this solves

The current `.so` is a `cc_prebuilt_library_shared` — a binary copied in
whole from `/home/comrade/builds/android/JamesDSP-src/app/src/main/cpp`'s
**Gradle/NDK** CMake build (see that project's
`app/src/main/cpp/CMakeLists.txt:68-91`, target `jamesdsp` → renamed
`jamesdspaidl`). That build only links `liblog`. **It cannot link
`libbinder_ndk` or `android.hardware.audio.effect-*-ndk`** — those are
Soong modules that live inside the platform tree; the standalone NDK
sysroot Gradle uses doesn't have them. This is not a build config
mistake to fix — the Gradle/CMake build is structurally incapable of
producing an AIDL-linked binary. Don't try to make it link them; it
can't.

So Task A's new AIDL wrapper (`JamesDspAidlEffect.cpp/.h`) has to be
compiled by **Soong**, inside this AOSP tree, as a real
`cc_library_shared` — not folded into the existing Gradle output.

## The plan: two artifacts, not one

1. **Existing Gradle/CMake build keeps doing what it already does well**
   — compile the DSP engine (`jamesdsp.c`, LSP units, Lua, chain
   ordering). Nothing about that pipeline changes except output naming
   (below). This is the "don't rewrite what already works" part —
   the engine binary is untouched.
2. **New Soong module** in `vendor/JamesDSP/Android.bp` compiles Task
   A's two new files as a real `cc_library_shared`, linked against the
   AIDL stubs (via the shared defaults block every other AOSP effect
   library uses — see below) *and* against the Gradle-built engine
   binary as a prebuilt dependency.
3. Soong module #2 is the one actually named `libjamesdspaidl` and
   installed to `soundfx` (must match `audio_effects_config.xml`'s
   `<library path="libjamesdspaidl.so">` — do not change that filename).
   Module #1 becomes an internal dependency lib, different name, not
   directly referenced by the XML config.

## Step 1 — rename the Gradle build's output

In `/home/comrade/builds/android/JamesDSP-src/app/src/main/cpp/CMakeLists.txt`,
line 88 currently reads:
```cmake
set_target_properties(jamesdsp PROPERTIES OUTPUT_NAME jamesdspaidl)
```
Change to:
```cmake
set_target_properties(jamesdsp PROPERTIES OUTPUT_NAME jamesdsp_engine)
```
Rebuild (`assembleRootFdroidRelease` — see the loose end at the bottom of
this file about *why* it must be `Fdroid` not `Full`). Output lands at
`app/build/outputs/.../libjamesdsp_engine.so` (arm64-v8a slice needed).

## Step 2 — copy the renamed engine binary into the vendor tree

Same mechanism already used for the current prebuilt (this is not a new
pattern, just a new filename):
```
cp <build output arm64-v8a>/libjamesdsp_engine.so \
   vendor/JamesDSP/proprietary/vendor/lib64/soundfx/libjamesdsp_engine.so
```
Do **not** delete `proprietary/vendor/lib64/soundfx/libjamesdspaidl.so`
yet — Step 4 repurposes that filename for the new Soong-built module's
output, so it'll be overwritten by the build, not by hand.

## Step 3 — rewrite `vendor/JamesDSP/Android.bp`

Current file has two modules: the `JamesDSP` `android_app_import` (leave
completely unchanged — that's the APK, unrelated to this) and the
`libjamesdspaidl` `cc_prebuilt_library_shared` (replace this one).

Replace the `cc_prebuilt_library_shared` block with:

```bp
cc_prebuilt_library_shared {
    name: "libjamesdsp_engine",
    strip: { none: true },
    target: {
        android_arm64: {
            srcs: ["proprietary/vendor/lib64/soundfx/libjamesdsp_engine.so"],
        },
    },
    vendor: true,
    ignore_max_page_size: true,
    // internal dependency only — not installed under soundfx directly,
    // libjamesdspaidl below pulls it in via shared_libs.
}

cc_library_shared {
    name: "libjamesdspaidl",
    defaults: ["aidlaudioeffectservice_defaults"],
    srcs: [
        "aidl-wrapper/JamesDspAidlEffect.cpp",
        ":effectCommonFile",
    ],
    shared_libs: [
        "libjamesdsp_engine",
    ],
    header_libs: [
        "libjamesdsp_engine_headers",
    ],
    relative_install_path: "soundfx",
    vendor: true,
    ignore_max_page_size: true,
}

cc_library_headers {
    name: "libjamesdsp_engine_headers",
    export_include_dirs: ["proprietary/include"],
    vendor: true,
}
```

Notes on why each piece is there, so it isn't second-guessed mid-task:

- `defaults: ["aidlaudioeffectservice_defaults"]` — this is the exact
  same defaults block every AOSP effect library in this tree uses
  (confirmed: `hardware/interfaces/audio/aidl/default/visualizer/Android.bp`
  uses it for `libvisualizersw`). It resolves the AIDL stub versions
  automatically (`latest_android_hardware_audio_effect_ndk_shared` etc.
  — defined at `hardware/interfaces/audio/aidl/default/Android.bp:294-318`)
  instead of hand-pinning a version like the old Android.bp did
  (`-V2-ndk`, which may not even be the version this Android 16 tree
  ships — don't reintroduce manual version pinning).
- `:effectCommonFile` — filegroup providing `EffectContext.cpp`,
  `EffectThread.cpp`, `EffectImpl.cpp` (defined
  `hardware/interfaces/audio/aidl/default/Android.bp:327-334`), the base
  class implementations `EffectImpl` (which Task A's class subclasses)
  needs. Every AOSP effect library includes this the same way — it's
  not JamesDSP-specific.
- `libjamesdsp_engine_headers` — Task A's `.cpp` needs the `EffectDSPMain`
  struct definition and the five function declarations
  (`EffectDSPMainConstructor/Destructor/Command/Process`, `configure`)
  from the engine's own headers. Point `export_include_dirs` at wherever
  those headers actually live relative to
  `libjamesdsp/Main/libjamesdsp/jni/jamesdsp/jdsp/` — copy just the
  needed `.h` file(s) into `vendor/JamesDSP/proprietary/include/` (same
  "copy build artifacts into vendor/JamesDSP" pattern as the engine
  `.so` itself), don't try to point Soong at the Gradle project's live
  source tree outside this repo.

## Step 4 — sanity checks before treating this as done

- `m libjamesdspaidl` in isolation should succeed and produce a `.so`
  whose `NEEDED` entries now include `libbinder_ndk.so` — verify with
  `readelf -d out/.../libjamesdspaidl.so | grep NEEDED`, don't just trust
  a clean build log. (This is the exact check that revealed the original
  bug — use it again to confirm the fix.)
- `nm -D --defined-only <built .so> | grep -w -e createEffect -e destroyEffect -e queryEffect`
  should show all three, non-empty.
- Full rebuild (`blazer-ota-build`, per the project's normal gated
  pipeline — don't run raw `mka bacon`), then on-device: force-restart
  `audioserver` (`su -c 'setprop ctl.restart audioserver'`) and check
  logcat for `ASvc::ExternalLibrary: Initialize: query uuid =
  f27317f4-...` followed by a `got a descriptor for` line — that pattern
  is what every *working* effect library produces and what the current
  broken one never does. Its presence is the actual pass/fail signal,
  not just "build succeeded."

## Loose end from the same debugging session — fix in the same pass

`device/google/blazer-secur/prebuilts-apk/com.termux.jdsp.apk` is
currently the `rootFull` Gradle flavor, which crashes on every launch
(`FirebaseApp` not initialized — `google-services.json` has no entry for
the `com.termux.jdsp` applicationId, only upstream's package names). This
was already root-caused and fixed once as a temporary live overlay via
`blazer-sysapp-update`, but the fix never got promoted into the tree, so
the next full OTA silently reintroduces the crash. When staging the new
engine build in Step 1, **also** build `assembleRootFdroidRelease`
(not `rootFull`) and copy that APK — not the `rootFull` one — into
`prebuilts-apk/com.termux.jdsp.apk`, and update its row in
`APPS_MANIFEST.md` (new SHA256, note the flavor fix). `rootFdroid` sets
`FOSS_ONLY=true`, which skips planting the Crashlytics Timber tree
(`MainApplication.kt:111-112`) entirely — same DSP functionality, no
Firebase dependency, no code changes needed, just the correct Gradle
flavor.
