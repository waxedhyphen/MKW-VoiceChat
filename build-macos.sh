#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
ARCH="$(uname -m)"

case "$ARCH" in
  arm64) TRIPLET="arm64-osx" ;;
  x86_64) TRIPLET="x64-osx" ;;
  *) echo "Unsupported macOS architecture: $ARCH"; exit 1 ;;
esac

if ! xcode-select -p >/dev/null 2>&1; then
  echo "Install Apple Command Line Tools first: xcode-select --install"
  exit 1
fi

if [ -n "${VCPKG_ROOT:-}" ] && [ -x "$VCPKG_ROOT/vcpkg" ]; then
  VCPKG="$VCPKG_ROOT/vcpkg"
elif command -v vcpkg >/dev/null 2>&1; then
  VCPKG="$(command -v vcpkg)"
elif [ -x "$HOME/vcpkg/vcpkg" ]; then
  VCPKG="$HOME/vcpkg/vcpkg"
else
  VCPKG_ROOT="$ROOT/.tools/vcpkg"
  if [ ! -d "$VCPKG_ROOT/.git" ]; then
    git clone --depth 1 https://github.com/microsoft/vcpkg.git "$VCPKG_ROOT"
  fi
  "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
  VCPKG="$VCPKG_ROOT/vcpkg"
fi

"$VCPKG" install --triplet "$TRIPLET" --x-manifest-root="$ROOT" --x-install-root="$ROOT/vcpkg_installed"

INSTALL_ROOT="$ROOT/vcpkg_installed"
DEPS="$INSTALL_ROOT/$TRIPLET"
OPUS_LIB="$DEPS/lib/libopus.a"
if [ ! -f "$OPUS_LIB" ]; then
  echo "vcpkg installed the packages, but libopus.a was not found at $OPUS_LIB"
  find "$INSTALL_ROOT" -maxdepth 4 -type f 2>/dev/null | sort | head -200
  exit 1
fi

OUT="$ROOT/bin/mkw_voicechat"

for LIB in libimgui.a libglfw3.a libopus.a libspeexdsp.a libdatachannel.a libusrsctp.a libjuice.a libssl.a libcrypto.a; do
  if [ ! -f "$DEPS/lib/$LIB" ]; then
    echo "Missing $DEPS/lib/$LIB"
    find "$INSTALL_ROOT" -type f -name "$LIB" -print 2>/dev/null
    exit 1
  fi
done

if ! command -v python3 >/dev/null 2>&1; then
  echo "Python 3 is required to prepare the pinned RNNoise model"
  exit 1
fi

python3 "$ROOT/tools/prepare_rnnoise_model.py" --fetch "$ROOT/third_party/rnnoise/src/rnnoise_data.c"

mkdir -p "$ROOT/bin" "$ROOT/obj/rnnoise"

RNNOISE_OBJECTS=()
for SOURCE in "$ROOT"/third_party/rnnoise/src/*.c; do
  OBJECT="$ROOT/obj/rnnoise/$(basename "${SOURCE%.c}").o"
  clang -std=c99 -O2 -D_DARWIN_C_SOURCE -DRNNOISE_BUILD -DDISABLE_DEBUG_FLOAT -I"$ROOT/third_party/rnnoise/include" -I"$ROOT/third_party/rnnoise/src" -c "$SOURCE" -o "$OBJECT"
  RNNOISE_OBJECTS+=("$OBJECT")
done
cp "$ROOT/third_party/rnnoise/COPYING" "$ROOT/bin/RNNoise-LICENSE.txt"

clang++ -std=c++23 -O2 -pthread -DMKWVC_HAS_ICE=1 -DGL_SILENCE_DEPRECATION \
  -I"$ROOT/include" \
  -I"$ROOT/third_party/rnnoise/include" \
  -I"$DEPS/include" \
  -I"$DEPS/include/opus" \
  "$ROOT/src/AdaptiveJitterBuffer.cpp" \
  "$ROOT/src/AudioProcessor.cpp" \
  "${RNNOISE_OBJECTS[@]}" \
  "$ROOT/src/AppConfig.cpp" \
  "$ROOT/src/AudioEngine.cpp" \
  "$ROOT/src/IcePeerTransport.cpp" \
  "$ROOT/src/IceSignal.cpp" \
  "$ROOT/src/LoadTest.cpp" \
  "$ROOT/src/MicrophoneTest.cpp" \
  "$ROOT/src/NetworkSimulator.cpp" \
  "$ROOT/src/OpusCodec.cpp" \
  "$ROOT/src/SignalingClient.cpp" \
  "$ROOT/src/UdpVoiceTransport.cpp" \
  "$ROOT/src/VoiceClient.cpp" \
  "$ROOT/src/main.cpp" \
  "$DEPS/lib/libimgui.a" \
  "$DEPS/lib/libglfw3.a" \
  "$DEPS/lib/libopus.a" \
  "$DEPS/lib/libspeexdsp.a" \
  -Wl,-force_load,"$DEPS/lib/libdatachannel.a" \
  "$DEPS/lib/libusrsctp.a" \
  "$DEPS/lib/libjuice.a" \
  "$DEPS/lib/libssl.a" \
  "$DEPS/lib/libcrypto.a" \
  -framework Cocoa \
  -framework IOKit \
  -framework QuartzCore \
  -framework CoreFoundation \
  -framework CoreAudio \
  -framework AudioToolbox \
  -framework OpenGL \
  -o "$OUT"

echo "Built: $OUT"

