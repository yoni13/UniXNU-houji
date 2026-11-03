#!/usr/bin/env bash
# Build an already prepared Darwin 22 tree with caller-supplied KDK/CTF tools.
set -euo pipefail
: "${HOUJI_XNU:?path to prepared XNU source}"
# HOUJI_KDK is optional: XNU links KDK archives only for T6000, T8101 and VMAPPLE (makedefs/MakeInc.def), never for SWEET.
HOUJI_KDK=${HOUJI_KDK:-}
: "${HOUJI_FAKEROOT:?path containing usr/local/bin/ctfconvert, ctfmerge, ctfdump}"
: "${HOUJI_BUILD_ROOT:?new/local kernel output directory}"
if [[ -n "$HOUJI_KDK" && ! -d "$HOUJI_KDK/System/Library/KernelSupport" ]]; then
  echo "Missing KDK payload KernelSupport directory" >&2
  exit 1
fi
for ctf_tool in ctfconvert ctfmerge ctfdump; do
  if [[ ! -x "$HOUJI_FAKEROOT/usr/local/bin/$ctf_tool" ]]; then
    echo "Missing executable CTF tool: $ctf_tool" >&2
    exit 1
  fi
done
if [[ ! -f "$HOUJI_XNU/config/MASTER.arm64.sweet" ]]; then
  echo "Missing SWEET source configuration; prepare the kernel with the current overlay first" >&2
  exit 1
fi
port_root=$(cd "$(dirname "$0")/.." && pwd)
python3 "$port_root/tools/check-trial-bindings.py" "$HOUJI_XNU"
export DEVELOPER_DIR=${DEVELOPER_DIR:-$(xcode-select -p)}
export PATH="$DEVELOPER_DIR/Toolchains/XcodeDefault.xctoolchain/usr/bin:$DEVELOPER_DIR/usr/bin:$PATH"
# This SDK header is an external build input, not part of the public overlay.
# Populate only the caller's prepared tree; never copy SDK bytes into Git.
hid_header="$HOUJI_XNU/iokit/Kernel/arm/hid/IOHIDUsageTables.h"
if [[ ! -f "$hid_header" ]]; then
  sdk_root=$(xcrun --sdk macosx --show-sdk-path)
  sdk_hid=${HOUJI_HID_USAGE_HEADER:-"$sdk_root/System/Library/Frameworks/Kernel.framework/Headers/IOKit/hid/IOHIDUsageTables.h"}
  if [[ ! -r "$sdk_hid" ]]; then
    echo "Missing IOHIDUsageTables.h; set HOUJI_HID_USAGE_HEADER to the local SDK header" >&2
    exit 1
  fi
  cp "$sdk_hid" "$hid_header"
fi
export KERNEL_BUILD_DATE='Mon Aug 14 05:06:07 UTC 2023'
export KERNEL_BUILD_OBJROOT=1b191cb
mkdir -p "$HOUJI_BUILD_ROOT"
# Trial headers may be copied from an archived allocation with an older mtime
# than an existing object. Their bytes must be recompiled even in that case:
# a stale log binding fails the ownership check at an early XNU checkpoint.
for trial_name in HoujiLogAllocation.h HoujiZapTrial.h; do
  trial_header="$HOUJI_XNU/iokit/Kernel/houji-ufs-root/$trial_name"
  if [[ -f "$trial_header" ]]; then
    touch "$trial_header"
  fi
done
make -C "$HOUJI_XNU" install -j"${HOUJI_JOBS:-8}" \
  SDKROOT=macosx TARGET_CONFIGS='DEVELOPMENT ARM64 SWEET' BUILD_WERROR=0 BUILD_LTO=0 \
  SRCROOT="$HOUJI_XNU" OBJROOT="$HOUJI_BUILD_ROOT/xnu.obj" \
  SYMROOT="$HOUJI_BUILD_ROOT/xnu.sym" DSTROOT="$HOUJI_BUILD_ROOT/fakeroot" \
  FAKEROOT_DIR="$HOUJI_FAKEROOT" KDKROOT="$HOUJI_KDK" \
  TIGHTBEAMC=tightbeamc-not-supported RC_DARWIN_KERNEL_VERSION=22.6.0 \
  MEMORY_SIZE="$(sysctl -n hw.memsize)" SYSCTL_HW_PHYSICALCPU="$(sysctl -n hw.physicalcpu)" \
  SYSCTL_HW_LOGICALCPU="$(sysctl -n hw.logicalcpu)" KERNEL_BUILDS_IN_PARALLEL=1 \
  CTFCONVERT="$HOUJI_FAKEROOT/usr/local/bin/ctfconvert" \
  CTFMERGE="$HOUJI_FAKEROOT/usr/local/bin/ctfmerge" CTFDUMP="$HOUJI_FAKEROOT/usr/local/bin/ctfdump" \
  IIG="$port_root/tools/iig-darwin22.py" CCACHE="${HOUJI_CCACHE:-}" HOUJI_EXTRA_OBJS="${HOUJI_EXTRA_OBJS:-}"   # yoni13: the Ath12kWifi module objects (tools/build-ath12k.py)
