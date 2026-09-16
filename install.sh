#!/usr/bin/env sh
# graft - one-line installer (prebuilt binaries, no toolchain required).
#
#   curl -fsSL https://raw.githubusercontent.com/AEndrix03/Graft/master/install.sh | sh
#
# What it does:
#   1. detects OS/arch and picks the matching GitHub Release asset
#   2. downloads it + SHA256SUMS, verifies the checksum (fail-closed)
#   3. extracts into $GRAFT_HOME (default ~/.graft)
#   4. downloads the BGE-M3 embedding model (~600 MB) if not already there
#   5. writes ~/.graft/config.yaml with absolute paths (never clobbers yours)
#   6. puts ~/.graft/bin on PATH and runs a smoke check
#
# Env knobs:
#   GRAFT_HOME       install prefix           (default: $HOME/.graft)
#   GRAFT_VERSION    release tag to install   (default: latest)
#   GRAFT_REPO       owner/repo               (default: AEndrix03/Graft)
#   GRAFT_MODEL_URL  override the GGUF download URL
#   GRAFT_NO_MODEL   =1 to skip the model download
#   GRAFT_NO_PATH    =1 to skip editing shell rc files
#   GRAFT_NO_SETUP   =1 to skip installing the agent skills
#
# To build from source instead, see scripts/build-from-source.sh.

set -eu

REPO="${GRAFT_REPO:-AEndrix03/Graft}"
GRAFT_HOME="${GRAFT_HOME:-$HOME/.graft}"
MODEL_URL="${GRAFT_MODEL_URL:-https://huggingface.co/lm-kit/bge-m3-gguf/resolve/main/bge-m3-Q8_0.gguf}"

step() { printf '\n==> %s\n' "$*"; }
ok()   { printf '    ok   %s\n' "$*"; }
warn() { printf '    warn %s\n' "$*"; }
note() { printf '    %s\n' "$*"; }
fail() { printf '    FAIL %s\n' "$*" >&2; exit 1; }

need() { command -v "$1" >/dev/null 2>&1 || fail "'$1' is required but not installed."; }
need curl
need tar

TMP="$(mktemp -d 2>/dev/null || mktemp -d -t graft)"
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT INT TERM

# ---------- 1. platform ----------

step "Detecting platform"
os="$(uname -s)"
arch="$(uname -m)"
case "$arch" in
  x86_64|amd64)  arch=x86_64 ;;
  arm64|aarch64) arch=arm64 ;;
esac
case "$os" in
  Linux)  plat="linux-$arch" ;;
  Darwin) plat="macos-$arch" ;;
  MINGW*|MSYS*|CYGWIN*)
    fail "On Windows, use the PowerShell installer: irm https://raw.githubusercontent.com/$REPO/master/install.ps1 | iex" ;;
  *) fail "unsupported OS: $os" ;;
esac
ASSET="graft-${plat}.tar.gz"
ok "$plat"

# ---------- 2. resolve release ----------

step "Resolving release"
if [ -n "${GRAFT_VERSION:-}" ]; then
  API="https://api.github.com/repos/$REPO/releases/tags/$GRAFT_VERSION"
else
  API="https://api.github.com/repos/$REPO/releases/latest"
fi
curl -fsSL -H "User-Agent: graft-install" -o "$TMP/release.json" "$API" \
  || fail "cannot reach the GitHub release API ($API)"

TAG="$(tr ',' '\n' < "$TMP/release.json" | sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n 1)"
[ -n "$TAG" ] || fail "no release found at $API"

asset_url() {
  tr ',' '\n' < "$TMP/release.json" \
    | sed -n "s|.*\"browser_download_url\"[[:space:]]*:[[:space:]]*\"\([^\"]*/$1\)\".*|\1|p" \
    | head -n 1
}
ASSET_URL="$(asset_url "$ASSET")"
SUMS_URL="$(asset_url SHA256SUMS)"
if [ -z "$ASSET_URL" ]; then
  fail "release $TAG has no prebuilt archive for $plat. Use 'brew install AEndrix03/graft/graft', or build from source with scripts/build-from-source.sh"
fi
ok "$TAG ($ASSET)"

# ---------- 3. download + verify ----------

step "Downloading"
curl -fL --progress-bar -H "User-Agent: graft-install" -o "$TMP/$ASSET" "$ASSET_URL" \
  || fail "download failed: $ASSET_URL"

[ -n "$SUMS_URL" ] || fail "release $TAG publishes no SHA256SUMS - refusing to install unverified binaries"
curl -fsSL -H "User-Agent: graft-install" -o "$TMP/SHA256SUMS" "$SUMS_URL" || fail "cannot fetch SHA256SUMS"
want="$(awk -v a="$ASSET" '$2 == a || $2 == "*" a { print $1; exit }' "$TMP/SHA256SUMS")"
[ -n "$want" ] || fail "SHA256SUMS has no entry for $ASSET - refusing to install"
if command -v sha256sum >/dev/null 2>&1; then
  got="$(sha256sum "$TMP/$ASSET" | awk '{print $1}')"
elif command -v shasum >/dev/null 2>&1; then
  got="$(shasum -a 256 "$TMP/$ASSET" | awk '{print $1}')"
else
  fail "no sha256sum/shasum available - cannot verify the download"
fi
[ "$got" = "$want" ] || fail "SHA256 mismatch for $ASSET (want $want, got $got)"
ok "checksum verified"

# ---------- 4. extract ----------

step "Installing into $GRAFT_HOME"
mkdir -p "$GRAFT_HOME"
tar -xzf "$TMP/$ASSET" -C "$GRAFT_HOME" || fail "extraction failed"
[ -f "$GRAFT_HOME/bin/graft" ] || fail "archive did not contain bin/graft"
chmod +x "$GRAFT_HOME/bin/graft" "$GRAFT_HOME/bin/graftd" 2>/dev/null || true
ok "binaries under $GRAFT_HOME/bin"

# ---------- 5. model ----------

MODEL="$GRAFT_HOME/models/bge-m3.gguf"
if [ "${GRAFT_NO_MODEL:-0}" = "1" ]; then
  warn "skipping model download (GRAFT_NO_MODEL=1) - the daemon cannot embed until $MODEL exists"
elif [ -s "$MODEL" ]; then
  step "Embedding model"
  ok "already present at $MODEL"
else
  step "Downloading BGE-M3 embedding model (~600 MB, one time)"
  mkdir -p "$GRAFT_HOME/models"
  curl -fL --progress-bar --ssl-no-revoke -o "$MODEL.part" "$MODEL_URL" \
    || { rm -f "$MODEL.part"; fail "model download failed: $MODEL_URL"; }
  mv "$MODEL.part" "$MODEL"
  ok "model at $MODEL"
fi

# ---------- 6. config ----------

step "Configuring"
CONFIG="$GRAFT_HOME/config.yaml"
EXAMPLE=""
for c in "$GRAFT_HOME/config.example.yaml" "$GRAFT_HOME/share/graft/config.example.yaml"; do
  if [ -f "$c" ]; then EXAMPLE="$c"; break; fi
done
VIEWER="$GRAFT_HOME/viewer/dist"
if [ -d "$GRAFT_HOME/share/graft/viewer" ]; then VIEWER="$GRAFT_HOME/share/graft/viewer"; fi

if [ -f "$CONFIG" ]; then
  ok "keeping your existing $CONFIG"
elif [ -n "$EXAMPLE" ]; then
  awk -v model="$MODEL" -v viewer="$VIEWER" '
    /^embedding:/ { in_embed = 1; in_http = 0; print; next }
    /^http:/      { in_http = 1; in_embed = 0; print; next }
    /^[a-z]/      { in_embed = 0; in_http = 0 }
    in_embed && /^[ \t]+model_path:/  { print "  model_path: \"" model "\""; next }
    in_http  && /^[ \t]+viewer_path:/ { print "  viewer_path: \"" viewer "\""; next }
    { print }
  ' "$EXAMPLE" > "$CONFIG" || fail "could not write $CONFIG"
  ok "wrote $CONFIG"
else
  warn "no config.example.yaml in the archive - graft falls back to its built-in defaults"
fi

# ---------- 7. PATH ----------

BIN="$GRAFT_HOME/bin"
case ":${PATH}:" in
  *":$BIN:"*) ON_PATH=1 ;;
  *)          ON_PATH=0 ;;
esac
if [ "$ON_PATH" = "0" ] && [ "${GRAFT_NO_PATH:-0}" != "1" ]; then
  step "Adding $BIN to PATH"
  LINE="export PATH=\"$BIN:\$PATH\"   # graft"
  added=0
  for rc in "$HOME/.bashrc" "$HOME/.zshrc" "$HOME/.profile"; do
    [ -f "$rc" ] || continue
    if grep -Fq "$BIN" "$rc" 2>/dev/null; then added=1; continue; fi
    if printf '\n%s\n' "$LINE" >> "$rc" 2>/dev/null; then
      ok "updated $rc"
      added=1
    else
      warn "could not write to $rc"
    fi
  done
  [ "$added" = "1" ] || warn "no shell rc found - add this line yourself: $LINE"
  note "open a new shell, or run: $LINE"
fi

# ---------- 8. smoke check ----------

step "Smoke check"
if "$BIN/graft" stats >/dev/null 2>&1; then
  ok "daemon answered - graft is ready"
else
  warn "'graft stats' did not answer on the first try"
  note "the first call cold-starts the daemon and loads the model; run '$BIN/graft stats' again"
fi

# ---------- 9. agent skills ----------

SETUP_OK=0
if [ "${GRAFT_NO_SETUP:-0}" != "1" ]; then
  step "Installing the agent skills"
  if "$BIN/graft" setup 2>&1; then
    SETUP_OK=1
  else
    note "no agent set up yet - run 'graft setup' once your agent is installed"
  fi
fi

printf '\ngraft %s installed.\n\n' "$TAG"
if [ "$SETUP_OK" = "1" ]; then
  printf '  One step left: run /graft-init inside your agent.\n\n'
else
  printf '  Next: graft setup      installs the skills into Claude Code / Codex / OpenCode\n'
  printf '        /graft-init      run that inside the agent; it does the rest\n\n'
fi

# The installer succeeded; do not inherit a status from the last helper call.
exit 0
