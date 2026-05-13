#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

VERSION="$(tr -d '[:space:]' < VERSION)"
PLATFORM="${1:-}"
if [ -z "$PLATFORM" ]; then
  case "$(uname -s)" in
    Linux)  OS=linux ;;
    Darwin) OS=macos ;;
    MINGW*|MSYS*|CYGWIN*) OS=windows ;;
    *) echo "unsupported OS: $(uname -s)" >&2; exit 1 ;;
  esac
  ARCH="$(uname -m)"
  case "$ARCH" in
    x86_64|amd64) ARCH=x86_64 ;;
    arm64|aarch64) ARCH=arm64 ;;
  esac
  PLATFORM="$OS-$ARCH"
fi

DIST="$ROOT/dist/release"
PAYLOAD="$DIST/payload"
rm -rf "$PAYLOAD"
mkdir -p "$PAYLOAD/bin" "$PAYLOAD/share/graft/integrations" "$DIST"

EXE=""
case "$PLATFORM" in
  windows-*) EXE=".exe" ;;
esac

cp -f "build/graft${EXE}" "build/graftd${EXE}" "$PAYLOAD/bin/"

case "$PLATFORM" in
  windows-*)
    cp -f third_party/llama.cpp/build/bin/*.dll "$PAYLOAD/bin/" 2>/dev/null || true
    ;;
  linux-*)
    for d in third_party/llama.cpp/build/bin third_party/llama.cpp/build/src third_party/llama.cpp/build/ggml/src; do
      cp -f "$d"/*.so* "$PAYLOAD/bin/" 2>/dev/null || true
    done
    ;;
  macos-*)
    for d in third_party/llama.cpp/build/bin third_party/llama.cpp/build/src third_party/llama.cpp/build/ggml/src; do
      cp -f "$d"/*.dylib "$PAYLOAD/bin/" 2>/dev/null || true
    done
    ;;
esac

cp -f config.example.yaml "$PAYLOAD/config.example.yaml"
cp -R integrations/standard "$PAYLOAD/share/graft/integrations/standard"

if [ -d viewer ] && [ -f viewer/package.json ]; then
  mkdir -p "$PAYLOAD/viewer"
  if command -v rsync >/dev/null 2>&1; then
    rsync -a --delete --exclude node_modules --exclude dist viewer/ "$PAYLOAD/viewer/"
  else
    (cd viewer && find . -mindepth 1 \( -path ./node_modules -o -path ./dist \) -prune -o -print) |
      while read -r p; do
        src="viewer/${p#./}"
        dst="$PAYLOAD/viewer/${p#./}"
        if [ -d "$src" ]; then mkdir -p "$dst"; else mkdir -p "$(dirname "$dst")"; cp "$src" "$dst"; fi
      done
  fi
fi

NAME="graft-${PLATFORM}"
rm -f "$DIST/${NAME}.tar.gz" "$DIST/${NAME}.zip"
(
  cd "$PAYLOAD"
  if [[ "$PLATFORM" == windows-* ]]; then
    powershell.exe -NoProfile -Command "Compress-Archive -Force -Path * -DestinationPath '$DIST/${NAME}.zip'" 2>/dev/null \
      || pwsh -NoProfile -Command "Compress-Archive -Force -Path * -DestinationPath '$DIST/${NAME}.zip'"
  else
    tar -czf "$DIST/${NAME}.tar.gz" .
  fi
)

echo "$DIST/${NAME}$([[ "$PLATFORM" == windows-* ]] && echo .zip || echo .tar.gz)"
