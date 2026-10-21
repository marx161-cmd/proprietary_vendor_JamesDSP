#!/usr/bin/env bash
# Build arbitrary AOSP targets from this tree: tools/m.sh <target>...
set -eo pipefail
TOP="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$TOP"; export SKIP_KERNEL_BUILD=true
set +e; set +u; source build/envsetup.sh >/dev/null; lunch lineage_blazer bp4a user >/dev/null; set -e
m "$@"
