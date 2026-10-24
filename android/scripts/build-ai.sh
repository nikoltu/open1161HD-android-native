#!/usr/bin/env bash
# Builds the built-in AI (android/ai/unifiedai.cpp) for both ABIs straight into
# the app's assets, where the launcher picks it up:
#
#   bash android/scripts/build-ai.sh
#
# Uses the NDK's clang (ANDROID_NDK, or the SDK's ndk/26.3.11579264). After
# rebuilding, raise versionCode -- the app extracts bundled bots only when the
# version changes.
set -euo pipefail

NDK="${ANDROID_NDK:-${ANDROID_HOME:-$LOCALAPPDATA/Android/Sdk}/ndk/26.3.11579264}"
BIN="$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin"
[ -d "$BIN" ] || BIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
[ -d "$BIN" ] || BIN="$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin"
STRIP="$BIN/llvm-strip"; [ -x "$STRIP" ] || STRIP="$STRIP.exe"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$ROOT/android/app/src/main/assets/bots"

build_one() {  # <target-prefix> <abi-dir>
	local cc="$BIN/$1-clang++" abi="$2"
	[ -x "$cc" ] || cc="$cc.cmd"
	mkdir -p "$OUT/$abi"
	"$cc" -std=c++14 -shared -O2 -fPIC \
		-I "$ROOT/android/app/src/main/cpp" \
		"$ROOT/android/ai/unifiedai.cpp" -o "$OUT/$abi/libunifiedai.so"
	"$STRIP" --strip-unneeded "$OUT/$abi/libunifiedai.so"
	echo "built $OUT/$abi/libunifiedai.so"
}

build_one aarch64-linux-android24 arm64-v8a
build_one x86_64-linux-android24  x86_64
