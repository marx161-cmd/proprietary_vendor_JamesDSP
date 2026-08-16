# Task A: JamesDSP AIDL IEffect implementation

## What's actually wrong (verified, not guessed)

The currently-shipped `libjamesdspaidl.so` (built from
`/home/comrade/builds/android/JamesDSP-src/app/src/main/cpp/CMakeLists.txt`,
target `jamesdsp` → renamed `jamesdspaidl`) is a complete, working
**legacy pre-Treble** `audio_effect_library_t` effect library. It exports
`EffectCreate`/`EffectRelease`/`EffectGetDescriptor` and
`AUDIO_EFFECT_LIBRARY_INFO_SYM` (see
`libjamesdsp/Main/libjamesdsp/jni/jamesdsp/jamesdsp.c:845-901`). That ABI
is dead on this device (Android 16 / crDroid on Tensor). Confirmed live on
device: `audioserver` dlopens the `.so` fine, then silently drops it —
verified by pulling a working reference library (`libbundleaidl.so`) off
the same phone and diffing exported symbols. The modern loader
(`hardware/interfaces/audio/aidl/default/include/effectFactory-impl/EffectFactory.cpp:279-296`
in the crDroid tree) does `dlsym(handle, "createEffect")`,
`"queryEffect"`, `"destroyEffect"` — none of those three names exist in
the current `.so`.

**This task is not "port the DSP engine to AIDL."** The engine
(`EffectDSPMainConstructor/Destructor/Command/Process` in `jamesdsp.c`,
plus the LSP dynamics units, Lua bridge, chain ordering already wired in
via `chain_modules.c`/`lsp_wrap.cpp`/`lua_bridge.c`) is complete and
correct — it's just called via the wrong entry points. This task adds a
thin AIDL `IEffect` implementation that forwards to the existing engine
calls. Do not reimplement DSP logic.

## Confirmed: the app's parameter protocol reaches this cleanly

`JamesDspRemoteEngine.kt` in the app talks the **legacy** protocol —
`effect.setParameter(1200, enable.toShort())` etc. (raw int/short pairs,
not the new typed `Parameter` union). Verified this still works on the
modern stack: `frameworks/av/media/libaudiohal/impl/EffectConversionHelperAidl.cpp:61-133`
(`handleSetParameter`) accepts the exact legacy `effect_param_t` blob via
`cmdSize`/`pCmdData`, unchanged, and forwards it into the AIDL
`Parameter::Specific::vendorEffect` (`VendorExtension`/`DefaultExtension`
path). So: **no Kotlin/app-side changes needed.** The wrapper receives
the same raw bytes `EffectDSPMainCommand()` already knows how to parse —
just pass them straight through.

## Reference implementation to copy the shape from

`hardware/interfaces/audio/aidl/default/extension/ExtensionEffect.{h,cpp}`
in the crDroid tree is AOSP's own reference for exactly this situation
(vendor-extension/raw-bytes custom effect, not one of the strongly-typed
built-in effect types). Copy its structure, not its content. Read both
files in full before writing — they're short (~140 lines combined).

Also read `hardware/interfaces/audio/aidl/default/include/effect-impl/EffectImpl.h`
for the base class contract (4 pure virtuals: `getDescriptor`,
`getEffectName`, `releaseContext`, `effectProcessImpl`).

## Files to create

`vendor/JamesDSP/aidl-wrapper/JamesDspAidlEffect.h`
`vendor/JamesDSP/aidl-wrapper/JamesDspAidlEffect.cpp`

## Exact spec

### Descriptor / UUIDs (must match `audio_effects_config.xml` exactly — already deployed, do not change)

From `jamesdsp.c:790-800`:
```c
type uuid: f98765f4-c321-5de6-9a45-123459495ab2
impl uuid: f27317f4-c984-4de6-9a90-545759495bf2
flags:     EFFECT_FLAG_TYPE_INSERT | EFFECT_FLAG_INSERT_FIRST
name:      "JamesDSP v4.01"
implementor: "James Fung"
```
Translate flags the same way `VisualizerSw.cpp:74-76` does it:
`.flags = {.type = Flags::Type::INSERT, .insert = Flags::Insert::FIRST, .volume = Flags::Volume::NONE}`.
Use `getEffectTypeUuidExtension()`-style helper pattern, or construct the
`AudioUuid` literal directly — check how `ExtensionEffect.cpp` does
`getEffectImplUuidExtension()`/`getEffectTypeUuidExtension()`; if those
are generated per-effect in `EffectUUID.h`/`.cpp` and JamesDSP doesn't
have generated helpers, just build `AudioUuid` inline from the 5-field
UUID struct instead of adding new helper functions — don't invent a new
UUID registration mechanism.

### Class shape (mirror `ExtensionEffect.h`)

```cpp
class JamesDspEffectContext final : public EffectContext {
  public:
    JamesDspEffectContext(int statusDepth, const Parameter::Common& common);
    ~JamesDspEffectContext();
    RetCode setParams(const std::vector<uint8_t>& params);   // -> EffectDSPMainCommand(EFFECT_CMD_SET_PARAM)
    std::vector<uint8_t> getParams(const std::vector<uint8_t>& id) const; // -> EffectDSPMainCommand(EFFECT_CMD_GET_PARAM)
  private:
    EffectDSPMain mDspMain;   // struct from jamesdsp.c — constructed/destructed here
};

class JamesDspAidlEffect final : public EffectImpl {
    // same 4 overrides + createContext/releaseContext as ExtensionEffect
};
```

### Exact forwarding (do not reinvent any of this — these functions already exist and work)

- Context construction → `EffectDSPMainConstructor(&mDspMain)` (declared
  `jamesdsp.c:43`). Context destruction → `EffectDSPMainDestructor(&mDspMain)`
  (`jamesdsp.c:55`).
- `setParams(bytes)` / `getParams(bytes)` → call
  `EffectDSPMainCommand(&mDspMain, EFFECT_CMD_SET_PARAM, bytes.size(),
  (void*)bytes.data(), &replySize, replyBuf)` (signature at
  `jamesdsp.c:143`) — same for `EFFECT_CMD_GET_PARAM`. This is a direct
  passthrough of the byte vector `EffectConversionHelperAidl` already
  hands you in the legacy `effect_param_t` layout — do not re-parse or
  re-pack it, `EffectDSPMainCommand` already expects exactly this shape.
- `effectProcessImpl(float* in, float* out, int samples)` → call
  `EffectDSPMainProcess(&mDspMain, &inBuf, &outBuf)` (`jamesdsp.c:766`).
  **Use the float path, not int16**: the engine supports both
  (`jamesdsp.c` ~line 772 vs 775, `in->s16` vs `in->f32`), and the AIDL
  interface already hands you `float*` — configure for float
  (`processFloatMultiplexd`) to avoid a needless format conversion.
  `audio_buffer_t` needs `frameCount` = `samples / channelCount` (get
  channel count from the `Parameter::Common` passed to `createContext`,
  store it on the context) — **`samples` from `effectProcessImpl` is
  sample count, not frame count; dividing by channel count is required
  or you'll process garbage / crash on stride.**
- Before first `EffectDSPMainProcess` call, the engine needs `configure()`
  called once (`jamesdsp.c:66`, signature
  `configure(EffectDSPMain*, void* pCmdData, effect_buffer_access_e*)`)
  — check what `pCmdData` it expects (likely an `effect_config_t`-style
  struct with sample rate / channel mask, built from `Parameter::Common`).
  Read `configure()`'s body (`jamesdsp.c:66-142`) to see exactly what it
  reads from `pCmdData` before assuming the shape.
- `getDescriptor` → return the static `Descriptor` built from the values
  above (same pattern as `ExtensionEffect::kDescriptor`).

### Exported C entry points (exact signatures — do not deviate)

```cpp
extern "C" binder_exception_t createEffect(const AudioUuid* in_impl_uuid,
                                            std::shared_ptr<IEffect>* instanceSpp);
extern "C" binder_exception_t destroyEffect(const std::shared_ptr<IEffect>& instanceSp);
extern "C" binder_exception_t queryEffect(const AudioUuid* in_impl_uuid, Descriptor* _aidl_return);
```
Signatures are pinned by
`hardware/interfaces/audio/aidl/default/include/effect-impl/EffectTypes.h:25-32`
— copy `ExtensionEffect.cpp:40-63` almost verbatim, swapping the UUID
check and instance type.

## Do not touch

- `jamesdsp.c`, `chain_modules.c`, `lsp_wrap.cpp`, `lua_bridge.c`, any
  `lsp-*` sources — the engine is correct and working, this task only
  adds a caller.
- `audio_effects_config.xml` (already correctly deployed at
  `/vendor/etc/`, UUIDs already match).
- The app / Kotlin side — confirmed unnecessary, see parameter-protocol
  section above.

## Handoff note

This file assumes Task B (see `../TASK_B_build_integration.md`) sets up
the actual compile target these two new files get built into — don't
invent your own Android.bp/CMake wiring here, that's Task B's job and
depends on decisions made there (how the existing DSP engine gets linked
in).
