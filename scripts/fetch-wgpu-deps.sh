#!/bin/sh
# Fetch what a WebGPU build needs (README, "WebGPU support") into DEST:
#   DEST/native-dawn/  native-dawn's release archive for TARGET, sha256-checked
#                      against the hashes pinned below   -> -DNATIVE_DAWN_DIR
#   DEST/wasmcart/     the wasmcart npm package           -> -DWASMCART_WGPU_JS_DIR=DEST/wasmcart/src/wgpu
#
#   sh scripts/fetch-wgpu-deps.sh TARGET DEST
#
# TARGET is native-dawn's name for the platform: linux-x64, linux-arm64,
# darwin-x64, darwin-arm64, win32-x64. Versions come from
# NATIVE_DAWN_VERSION and WASMCART_VERSION (the workflow pins both).
# WASMCART_SPEC overrides where wasmcart comes from (anything `npm pack`
# accepts, e.g. a local checkout); the package it yields must still be
# WASMCART_VERSION.
set -eu
TARGET=$1
DEST=$2
: "${NATIVE_DAWN_VERSION:?set NATIVE_DAWN_VERSION}"
: "${WASMCART_VERSION:?set WASMCART_VERSION}"

# sha256 of native-dawn-v<version>-<target>.tar.gz, copied from the .sha256
# files on github.com/monteslu/native-dawn/releases/tag/v<version> when the
# version is bumped. They live here, not fetched from the release, so a
# release asset replaced after the fact cannot change what gets built.
case "$NATIVE_DAWN_VERSION-$TARGET" in
  0.1.4-linux-x64)     SHA=f32e02fba55dc2c04be4599478cb8bfe85838a982393fabf83fb9a3e4e645683 ;;
  0.1.4-linux-arm64)   SHA=acf62d488e16b0ed8e84b809cf6438a4a0b69ffad0753f4b8686ad219b966d9f ;;
  0.1.4-darwin-x64)    SHA=31b862434609d46f1459ddfae1a9ab46bcae346f7a8c60f23c256230171c9e18 ;;
  0.1.4-darwin-arm64)  SHA=76305af7a36278c89defe0f1658640f61a8cb13a396391b63633cbf999959880 ;;
  0.1.4-win32-x64)     SHA=fde6d9e3b40fe509f290444a20a88cd3a2f2edb18a00878fdd4ff299d923f0fc ;;
  0.1.4-android-arm64) SHA=b9d8d82260b9bbd8506117fe16b5506731f29ed779894a2d78faee818e10c593 ;;
  *) echo "fetch-wgpu-deps: no pinned sha256 for native-dawn $NATIVE_DAWN_VERSION $TARGET" >&2; exit 1 ;;
esac
case "$TARGET" in
  win32-*) DAWN_NODE=native-dawn/bin/dawn.node ;;
  *) DAWN_NODE=native-dawn/lib/dawn.node ;;
esac

mkdir -p "$DEST"
DEST=$(cd "$DEST" && pwd)
rm -rf "$DEST/native-dawn" "$DEST/wasmcart"
mkdir -p "$DEST/native-dawn" "$DEST/wasmcart"

ARCHIVE="native-dawn-v$NATIVE_DAWN_VERSION-$TARGET.tar.gz"
curl -sfL --retry 3 -o "$DEST/$ARCHIVE" \
  "https://github.com/monteslu/native-dawn/releases/download/v$NATIVE_DAWN_VERSION/$ARCHIVE"
if command -v sha256sum > /dev/null; then GOT=$(sha256sum "$DEST/$ARCHIVE" | cut -d' ' -f1)
else GOT=$(shasum -a 256 "$DEST/$ARCHIVE" | cut -d' ' -f1); fi
if [ "$GOT" != "$SHA" ]; then
  echo "fetch-wgpu-deps: $ARCHIVE sha256 is $GOT, expected $SHA" >&2
  exit 1
fi
tar xzf "$DEST/$ARCHIVE" -C "$DEST/native-dawn"
rm "$DEST/$ARCHIVE"

# wasmcart's src/wgpu (host.js and the generated glue) and src/cartMemory.js
# come from its npm package; `npm pack` only downloads it (no install scripts).
SPEC=${WASMCART_SPEC:-wasmcart@$WASMCART_VERSION}
if ! TGZ=$(cd "$DEST" && npm pack --silent "$SPEC" | tail -1) || [ ! -f "$DEST/$TGZ" ]; then
  echo "fetch-wgpu-deps: npm pack $SPEC failed. A wasmcart-native release needs wasmcart $WASMCART_VERSION published to npm first." >&2
  exit 1
fi
tar xzf "$DEST/$TGZ" -C "$DEST/wasmcart" --strip-components=1
rm "$DEST/$TGZ"
GOTV=$(node -p "require(process.argv[1]).version" "$DEST/wasmcart/package.json")
if [ "$GOTV" != "$WASMCART_VERSION" ]; then
  echo "fetch-wgpu-deps: $SPEC is wasmcart $GOTV, expected $WASMCART_VERSION" >&2
  exit 1
fi
for f in $DAWN_NODE wasmcart/src/wgpu/host.js wasmcart/src/cartMemory.js; do
  [ -f "$DEST/$f" ] || { echo "fetch-wgpu-deps: $DEST/$f is missing" >&2; exit 1; }
done
echo "fetch-wgpu-deps: native-dawn $NATIVE_DAWN_VERSION ($TARGET) and wasmcart $GOTV in $DEST"
