#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
output=${1:?Usage: build.sh /external/output-directory}
sdk=${WEBOS_SDK:-$HOME/.local/opt/arm-webos-linux-gnueabi_sdk-buildroot}
mkdir -p "$output/tmp"
export TMPDIR="$output/tmp"
cc="$sdk/bin/arm-webos-linux-gnueabi-gcc"
flags=(-std=c11 -D_POSIX_C_SOURCE=200809L -DCLOGX_HAVE_CLOCK_GETTIME=1 -Os -Wall -Wextra -Werror -march=armv7-a -mthumb -mfpu=neon -mfloat-abi=softfp)
"$cc" "${flags[@]}" -shared -fPIC -I"$root/native/include" \
    "$root/tools/avc444-rpc-probe/trace.c" "$root/tools/avc444-rpc-probe/trace.S" \
    "$root/native/src/category_log.c" -ldl -pthread -o "$output/libavc444-rpc-trace.so"
"$cc" "${flags[@]}" -shared -fPIC -DPROBE_STUB \
    "$root/tools/avc444-rpc-probe/test_forwarding.c" -o "$output/librpc-probe-fake.so"
"$cc" "${flags[@]}" "$root/tools/avc444-rpc-probe/test_forwarding.c" \
    -L"$output" -lrpc-probe-fake -Wl,-rpath,'$ORIGIN' -o "$output/test-forwarding"
sysroot="$sdk/arm-webos-linux-gnueabi/sysroot"
glib_flags=(-I"$sysroot/usr/include/glib-2.0" -I"$sysroot/usr/lib/glib-2.0/include")
"$cc" "${flags[@]}" "${glib_flags[@]}" -shared -fPIC -I"$root/native/include" \
    "$root/tools/avc444-rpc-probe/omx_mode.c" "$root/native/src/category_log.c" \
    -lgobject-2.0 -lglib-2.0 -ldl -o "$output/libavc444-omx-mode.so"
"$cc" "${flags[@]}" "${glib_flags[@]}" "$root/tools/avc444-rpc-probe/test_omx_mode.c" \
    -lgobject-2.0 -lglib-2.0 -o "$output/test-omx-mode"
"$cc" "${flags[@]}" -I"$root/native/include" "$root/tools/avc444-rpc-probe/bench_h264.c" \
    "$root/native/src/h264_annexb.c" "$root/native/src/category_log.c" -pthread -o "$output/bench-h264"
