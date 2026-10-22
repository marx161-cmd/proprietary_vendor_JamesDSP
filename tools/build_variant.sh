#!/usr/bin/env bash
# Build libjamesdspaidl.so for one engine variant and stash it under release/<variant>/.
#   tools/build_variant.sh full|audio
# Steps: copy the variant's libjamesdsp_engine_static.a (built by
# JamesDSP-src/app/src/main/cpp/build_engine_variant.sh) into the Soong prebuilt
# slot, `m libjamesdspaidl`, copy the .so out with its sha256.
# The slot is restored to whatever was there before (git-tracked = the OTA's full build).
set -eo pipefail
VARIANT="${1:?variant: full|audio}"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
TOP="$(cd "$HERE/../.." && pwd)"                       # crDroid tree root
SRC="$HOME/builds/android/JamesDSP-src/app/.cxx-variants/$VARIANT/arm64-v8a/libjamesdsp_engine_static.a"
SLOT="$HERE/proprietary/vendor/lib64/soundfx/libjamesdsp_engine_static.a"
RELDIR="$HERE/release/$VARIANT"
[[ -f "$SRC" ]] || { echo "missing $SRC (run build_engine_variant.sh $VARIANT first)" >&2; exit 1; }
mkdir -p "$RELDIR"
cp -p "$SLOT" "$SLOT.bak"
trap 'mv -f "$SLOT.bak" "$SLOT"' EXIT
cp "$SRC" "$SLOT"; touch "$SLOT"   # fresh mtime: ninja compares input mtime vs the last output; -p hid a swap (2026-10-21)
cd "$TOP"
export SKIP_KERNEL_BUILD=true
# shellcheck disable=SC1091
# envsetup.sh/lunch are not set -e clean (a sourced test returning non-zero
# would abort this script right here, trap included) -- relax, then re-arm.
set +e; set +u
source build/envsetup.sh >/dev/null
lunch lineage_blazer bp4a user >/dev/null
set -e
m libjamesdspaidl ${EXTRA_TARGETS:-}
SO="$TOP/out/target/product/blazer/vendor/lib64/soundfx/libjamesdspaidl.so"
cp -p "$SO" "$RELDIR/libjamesdspaidl.so"
# Relative names only (no build-host paths in the published file): the engine .a is
# listed by its variant-tagged basename, the .so by its own.
{ sha256sum "$SRC" | sed "s| .*| libjamesdsp_engine_static.$VARIANT.a|"; (cd "$RELDIR" && sha256sum libjamesdspaidl.so); } | tee "$RELDIR/SHA256SUMS"
