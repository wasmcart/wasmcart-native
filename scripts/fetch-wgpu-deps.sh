#!/bin/sh
# Fetch what a WebGPU build needs (README, "WebGPU support") into DEST:
#   DEST/native-dawn/  native-dawn's release archive for TARGET, sha256-checked
#                      against the hashes pinned below   -> -DNATIVE_DAWN_DIR
#   DEST/wasmcart/     the wasmcart npm package           -> -DWASMCART_WGPU_JS_DIR=DEST/wasmcart/src/wgpu
#
#   sh scripts/fetch-wgpu-deps.sh TARGET DEST
#
# TARGET is native-dawn's name for the platform: linux-x64, linux-arm64,
# darwin-x64, darwin-arm64, win32-x64, win32-arm64. Versions come from
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
  0.1.2-linux-x64)    SHA=101b160cbc4f2912d94d8d9da1ea1ff85146b203a71970fd8de42d3dd5ce1d7f ;;
  0.1.2-linux-arm64)  SHA=49511d97293efbfe5e38f63f9ae23e768896fd53ae81fe57b744b3dff8e5472b ;;
  0.1.2-darwin-x64)   SHA=c1178f6bb7c2d6ab09682202a2432f26604ec68b79ff0facf4179f5b8de3eab7 ;;
  0.1.2-darwin-arm64) SHA=8181ad3ed1819ca4c02440cf5a3419a92d0e18d59334fce6bfba622120422058 ;;
  0.1.2-win32-x64)    SHA=1b9d13cf65a946895e5a758a545da408ae9b1c8caa2f1fba809bfb02c10ffe04 ;;
  0.1.2-win32-arm64)  SHA=0a776fd111c7752eda1aa202b80d8cb58db8a3e1dc6d72680f5774394c70ff88 ;;
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
