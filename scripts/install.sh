#!/usr/bin/env bash
# memgraph — interactive installer.
#
# Works on Linux (apt / dnf / pacman), macOS (brew), and Windows MSYS2
# (pacman). Detects what is already in place and only asks the human for
# the decisions a human actually needs to make:
#
#   1. install missing system packages?      (sudo on Linux)
#   2. download the BGE-M3 embedding model?  (~600 MB)
#   3. embedding threads value               (defaults to nproc/2)
#
# Everything else — git submodules, llama.cpp build, memgraph build,
# default profile creation, smoke test — is automatic.

set -euo pipefail

# Non-interactive mode: accept every default ("yes" to package install, "yes"
# to model download, default thread count). Useful from CI or wrappers like
# install.ps1 that pre-decided the answers.
ASSUME_YES=0
for arg in "$@"; do
  case "$arg" in
    -y|--yes|--non-interactive) ASSUME_YES=1 ;;
  esac
done

# ---------- colors / prompts ----------

if [ -t 1 ]; then
  C_R=$'\033[31m'; C_G=$'\033[32m'; C_Y=$'\033[33m'; C_B=$'\033[34m'
  C_D=$'\033[2m'; C_N=$'\033[0m'
else
  C_R=''; C_G=''; C_Y=''; C_B=''; C_D=''; C_N=''
fi

step()  { printf "\n${C_B}==>${C_N} %s\n" "$*"; }
ok()    { printf "  ${C_G}✓${C_N} %s\n" "$*"; }
warn()  { printf "  ${C_Y}!${C_N} %s\n" "$*"; }
fail()  { printf "  ${C_R}✗${C_N} %s\n" "$*"; exit 1; }
note()  { printf "  ${C_D}%s${C_N}\n" "$*"; }

ask_yes_no() {
  # ask_yes_no "Prompt" "default(Y|n)"
  local prompt="$1" default="${2:-Y}" reply
  local hint="[Y/n]"
  [ "$default" = "n" ] || [ "$default" = "N" ] && hint="[y/N]"
  if [ "$ASSUME_YES" = 1 ]; then
    printf "  ${C_Y}?${C_N} %s %s ${C_D}(auto: %s)${C_N}\n" "$prompt" "$hint" "$default" >&2
    case "$default" in y|Y|yes|YES) return 0 ;; *) return 1 ;; esac
  fi
  printf "  ${C_Y}?${C_N} %s %s " "$prompt" "$hint" >&2
  read -r reply || true
  reply="${reply:-$default}"
  case "$reply" in y|Y|yes|YES) return 0 ;; *) return 1 ;; esac
}

ask_value() {
  # ask_value "Prompt" "default"
  local prompt="$1" default="$2" reply
  if [ "$ASSUME_YES" = 1 ]; then
    printf "  ${C_Y}?${C_N} %s [${default}] ${C_D}(auto)${C_N}\n" "$prompt" >&2
    printf "%s" "$default"
    return
  fi
  printf "  ${C_Y}?${C_N} %s [${default}] " "$prompt" >&2
  read -r reply || true
  printf "%s" "${reply:-$default}"
}

# ---------- platform detection ----------

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

OS=""; PKG=""; PKG_INSTALL=""; PKG_QUERY=""
case "$(uname -s)" in
  Linux)
    OS="linux"
    if command -v apt-get >/dev/null 2>&1; then
      PKG="apt"; PKG_INSTALL="sudo apt-get install -y"; PKG_QUERY="dpkg -s"
    elif command -v dnf >/dev/null 2>&1; then
      PKG="dnf"; PKG_INSTALL="sudo dnf install -y"; PKG_QUERY="rpm -q"
    elif command -v pacman >/dev/null 2>&1; then
      PKG="pacman"; PKG_INSTALL="sudo pacman -S --noconfirm --needed"; PKG_QUERY="pacman -Q"
    fi
    ;;
  Darwin)
    OS="macos"
    command -v brew >/dev/null 2>&1 \
      && PKG="brew" \
      && PKG_INSTALL="brew install" \
      && PKG_QUERY="brew list"
    ;;
  MINGW*|MSYS*|CYGWIN*)
    OS="msys2"; PKG="pacman"
    PKG_INSTALL="pacman -S --noconfirm --needed"
    PKG_QUERY="pacman -Q"
    ;;
  *) fail "unsupported OS: $(uname -s)" ;;
esac
ok "Platform: ${OS} (${PKG:-no package manager detected})"

# ---------- system packages ----------

declare -a NEEDED_PKGS=()
declare -a MISSING_TOOLS=()

require_tool() {
  local tool="$1" pkg_name="$2"
  if ! command -v "$tool" >/dev/null 2>&1; then
    MISSING_TOOLS+=("$tool")
    NEEDED_PKGS+=("$pkg_name")
  fi
}

step "Checking required tools…"

case "$PKG" in
  apt)
    require_tool gcc       build-essential
    require_tool cmake     cmake
    require_tool git       git
    require_tool curl      curl
    require_tool pkg-config pkg-config
    NEEDED_PKGS+=(libsqlite3-dev libyaml-dev)
    ;;
  dnf)
    require_tool gcc       gcc
    require_tool make      make
    require_tool cmake     cmake
    require_tool git       git
    require_tool curl      curl
    require_tool pkg-config pkgconf-pkg-config
    NEEDED_PKGS+=(sqlite-devel libyaml-devel)
    ;;
  pacman)
    if [ "$OS" = "msys2" ]; then
      require_tool gcc       mingw-w64-x86_64-gcc
      require_tool cmake     mingw-w64-x86_64-cmake
      require_tool git       git
      require_tool curl      mingw-w64-x86_64-curl
      require_tool pkg-config mingw-w64-x86_64-pkgconf
      NEEDED_PKGS+=(mingw-w64-x86_64-sqlite3 mingw-w64-x86_64-libyaml)
    else
      require_tool gcc       base-devel
      require_tool cmake     cmake
      require_tool git       git
      require_tool curl      curl
      require_tool pkg-config pkgconf
      NEEDED_PKGS+=(sqlite libyaml)
    fi
    ;;
  brew)
    require_tool cmake     cmake
    require_tool git       git
    require_tool curl      curl
    require_tool pkg-config pkg-config
    NEEDED_PKGS+=(sqlite libyaml)
    ;;
  *)
    warn "no supported package manager — you'll need to install dependencies manually"
    ;;
esac

if [ ${#MISSING_TOOLS[@]} -gt 0 ]; then
  warn "missing tools: ${MISSING_TOOLS[*]}"
fi

if [ ${#NEEDED_PKGS[@]} -gt 0 ] && [ -n "$PKG" ]; then
  # de-duplicate
  IFS=$'\n' read -r -d '' -a UNIQ_PKGS < <(printf "%s\n" "${NEEDED_PKGS[@]}" | sort -u && printf '\0') || true
  printf "  Will install: %s\n" "${UNIQ_PKGS[*]}"
  if ask_yes_no "Install missing system packages now?" "Y"; then
    # shellcheck disable=SC2086
    $PKG_INSTALL "${UNIQ_PKGS[@]}"
    ok "system packages installed"
  else
    warn "skipping system packages — build may fail if dependencies are missing"
  fi
fi

# ---------- git submodules ----------

step "Initializing third-party submodules…"
SUBMOD_FILES=(
  third_party/llama.cpp/include/llama.h
  third_party/mpack/src/mpack/mpack.h
  third_party/sqlite-vec/sqlite-vec.c
  third_party/BLAKE3/c/blake3.c
)
need_submods=0
for f in "${SUBMOD_FILES[@]}"; do
  [ -f "$f" ] || { need_submods=1; break; }
done
if [ "$need_submods" = 1 ]; then
  # Clear any stale index locks from a prior interrupted run.
  find .git/modules -name 'index.lock' -delete 2>/dev/null || true
  # --force re-checks out worktrees that were registered but left empty by
  # an interrupted earlier clone (common on Windows where git fails midway).
  git submodule update --init --recursive --force
  # verify
  for f in "${SUBMOD_FILES[@]}"; do
    [ -f "$f" ] || fail "submodule still missing: $f (try: git submodule update --init --recursive --force)"
  done
  ok "submodules ready"
else
  ok "submodules already present"
fi

# ---------- llama.cpp ----------

step "Building llama.cpp…"
LLAMA_LIB_GLOB=(third_party/llama.cpp/build/bin/libllama* third_party/llama.cpp/build/bin/llama.dll)
LLAMA_BUILT=0
for f in "${LLAMA_LIB_GLOB[@]}"; do [ -e "$f" ] && LLAMA_BUILT=1 && break; done

# GPU backend selection: opt-in via MEMGRAPH_GPU={cuda,hip,none}. Default none
# (CPU build). Set to cuda for NVIDIA, hip for AMD ROCm 6 or 7.
GPU_BACKEND="${MEMGRAPH_GPU:-none}"
GPU_FLAGS=()
case "$GPU_BACKEND" in
  none|cpu)
    ;;
  cuda)
    GPU_FLAGS=(-DGGML_CUDA=ON)
    ;;
  hip|rocm)
    GPU_FLAGS=(-DGGML_HIP=ON)
    ;;
  *)
    fail "MEMGRAPH_GPU='$GPU_BACKEND' invalid. Use cuda, hip, or none."
    ;;
esac

if [ "$LLAMA_BUILT" = 1 ]; then
  ok "llama.cpp already built (set MEMGRAPH_GPU and remove third_party/llama.cpp/build to rebuild with a different backend)"
else
  [ "$GPU_BACKEND" != "none" ] && step "  -> with $GPU_BACKEND backend"
  pushd third_party/llama.cpp >/dev/null
  CMAKE_GEN=()
  [ "$OS" = "msys2" ] && CMAKE_GEN=(-G Ninja)
  cmake -B build "${CMAKE_GEN[@]}" \
    -DBUILD_SHARED_LIBS=ON -DGGML_NATIVE=ON \
    -DLLAMA_CURL=OFF -DLLAMA_BUILD_SERVER=OFF \
    -DLLAMA_BUILD_TOOLS=OFF -DLLAMA_BUILD_EXAMPLES=OFF \
    -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_COMMON=OFF \
    -DCMAKE_BUILD_TYPE=Release \
    "${GPU_FLAGS[@]}"
  cmake --build build -j
  popd >/dev/null
  ok "llama.cpp built ($GPU_BACKEND)"
fi

# ---------- model ----------

step "Checking BGE-M3 model…"
mkdir -p models
MODEL_PATH="models/bge-m3.gguf"
MODEL_URL="https://huggingface.co/lm-kit/bge-m3-gguf/resolve/main/bge-m3-Q8_0.gguf"
if [ -s "$MODEL_PATH" ]; then
  ok "model already at $MODEL_PATH ($(du -h "$MODEL_PATH" | cut -f1))"
else
  if ask_yes_no "Download BGE-M3 (~600 MB) from Hugging Face?" "Y"; then
    note "this may take a few minutes…"
    curl -fL --ssl-no-revoke -o "$MODEL_PATH" "$MODEL_URL"
    ok "model downloaded"
  else
    warn "skipping model download — daemon will fail to embed until $MODEL_PATH is provided"
  fi
fi

# ---------- config ----------

step "Preparing config.yaml…"
if [ ! -f config.yaml ]; then
  cp config.example.yaml config.yaml
  THREADS_DEFAULT=4
  if command -v nproc >/dev/null 2>&1; then
    THREADS_DEFAULT=$(( $(nproc) / 2 ))
    [ "$THREADS_DEFAULT" -lt 1 ] && THREADS_DEFAULT=1
  elif command -v sysctl >/dev/null 2>&1; then
    THREADS_DEFAULT=$(( $(sysctl -n hw.ncpu) / 2 ))
    [ "$THREADS_DEFAULT" -lt 1 ] && THREADS_DEFAULT=1
  fi
  THREADS=$(ask_value "Embedding threads (CPU cores to use for inference)?" "$THREADS_DEFAULT")
  # in-place tweak with a portable awk so we don't depend on GNU sed -i
  awk -v t="$THREADS" '
    /^embedding:/ { in_embed=1; print; next }
    in_embed && /^[[:space:]]+threads:/ { print "  threads: " t; next }
    /^[a-z]/ && !/^embedding:/ { in_embed=0 }
    { print }
  ' config.yaml > config.yaml.tmp && mv config.yaml.tmp config.yaml
  ok "config.yaml created (threads=${THREADS})"
else
  ok "config.yaml already present (leaving as-is)"
fi

# ---------- memgraph ----------

# sqlite-vec ships only a header template; substitute the placeholders so
# our CMakeLists picks up a real header. Idempotent.
step "Generating sqlite-vec header…"
SQLITE_VEC_HEADER=third_party/sqlite-vec/sqlite-vec.h
if [ ! -f "$SQLITE_VEC_HEADER" ]; then
  SV_VERSION=$(cat third_party/sqlite-vec/VERSION)
  SV_MAJOR=$(echo "$SV_VERSION" | cut -d. -f1)
  SV_MINOR=$(echo "$SV_VERSION" | cut -d. -f2)
  SV_PATCH=$(echo "$SV_VERSION" | cut -d. -f3 | sed 's/-.*//')
  SV_DATE=$(date -u +%Y-%m-%dT%H:%M:%SZ)
  SV_SOURCE=$(cd third_party/sqlite-vec && git log -n 1 --pretty=format:%H 2>/dev/null || echo unknown)
  sed \
    -e "s/\${VERSION}/$SV_VERSION/g" \
    -e "s/\${DATE}/$SV_DATE/g" \
    -e "s/\${SOURCE}/$SV_SOURCE/g" \
    -e "s/\${VERSION_MAJOR}/$SV_MAJOR/g" \
    -e "s/\${VERSION_MINOR}/$SV_MINOR/g" \
    -e "s/\${VERSION_PATCH}/$SV_PATCH/g" \
    third_party/sqlite-vec/sqlite-vec.h.tmpl > "$SQLITE_VEC_HEADER"
  ok "generated $SQLITE_VEC_HEADER"
else
  ok "already present"
fi

step "Building memgraph…"
CMAKE_GEN_TOP=()
[ "$OS" = "msys2" ] && CMAKE_GEN_TOP=(-G Ninja)
cmake -B build -DCMAKE_BUILD_TYPE=Release "${CMAKE_GEN_TOP[@]}"
cmake --build build -j
ok "memgraph built"

# ---------- install into ~/.lmemorygraph ----------

step "Installing into ~/.lmemorygraph/…"
# On MSYS2, $HOME points at /home/<user> inside the MSYS2 root, but the
# Windows-side memgraph.exe resolves MEMGRAPH_HOME from $USERPROFILE
# (e.g. C:\Users\<user>). Install where the CLI will look — otherwise the
# files land in one place and the daemon looks in another.
if [ "$OS" = "msys2" ]; then
  WIN_USERPROFILE="${USERPROFILE:-}"
  if [ -z "$WIN_USERPROFILE" ] && command -v powershell.exe >/dev/null 2>&1; then
    # MSYS2 bash sometimes doesn't propagate USERPROFILE through scripts; ask
    # PowerShell directly. Single-shot, ~300ms — fine for an installer.
    WIN_USERPROFILE=$(powershell.exe -NoProfile -Command "[Environment]::GetFolderPath('UserProfile')" 2>/dev/null | tr -d '\r' | tail -1)
  fi
  if [ -n "$WIN_USERPROFILE" ] && command -v cygpath >/dev/null 2>&1; then
    INSTALL_HOME=$(cygpath -u "$WIN_USERPROFILE")
  else
    INSTALL_HOME="$HOME"
  fi
else
  INSTALL_HOME="$HOME"
fi
INSTALL_DIR="$INSTALL_HOME/.lmemorygraph"
INSTALL_BIN="$INSTALL_DIR/bin"
INSTALL_MODELS="$INSTALL_DIR/models"
mkdir -p "$INSTALL_BIN" "$INSTALL_MODELS"

# binaries
EXE_SUFFIX=""
[ "$OS" = "msys2" ] && EXE_SUFFIX=".exe"
cp -f "build/memgraph${EXE_SUFFIX}"  "$INSTALL_BIN/"
cp -f "build/memgraphd${EXE_SUFFIX}" "$INSTALL_BIN/"

# llama.cpp shared libs (Windows DLLs / Linux .so / macOS .dylib)
LLAMA_BIN="third_party/llama.cpp/build/bin"
LLAMA_SRC="third_party/llama.cpp/build/src"
LLAMA_GGML="third_party/llama.cpp/build/ggml/src"
case "$OS" in
  msys2)
    cp -f "$LLAMA_BIN"/*.dll "$INSTALL_BIN/" 2>/dev/null || true
    # Copy MinGW runtime + libyaml/sqlite DLLs that memgraphd depends on,
    # so the binary works without /mingw64/bin on the user's PATH.
    MGW_BIN="/mingw64/bin"
    [ -d "$MGW_BIN" ] || MGW_BIN="/c/msys64/mingw64/bin"
    if [ -d "$MGW_BIN" ]; then
      # MinGW runtime + project deps. Hard-coded list because ldd on the
      # build-tree binary doesn't see transitive deps from llama.cpp, and
      # missing one of these makes the daemon fail to load with a cryptic
      # "shared object file" error in non-MSYS2 shells.
      for n in libgcc_s_seh-1 libstdc++-6 libwinpthread-1 libgomp-1 libyaml-0-2 libsqlite3-0; do
        [ -f "$MGW_BIN/$n.dll" ] && cp -f "$MGW_BIN/$n.dll" "$INSTALL_BIN/" || true
      done
    fi
    ;;
  linux)
    for d in "$LLAMA_BIN" "$LLAMA_SRC" "$LLAMA_GGML"; do
      cp -f "$d"/*.so* "$INSTALL_BIN/" 2>/dev/null || true
    done
    ;;
  macos)
    for d in "$LLAMA_BIN" "$LLAMA_SRC" "$LLAMA_GGML"; do
      cp -f "$d"/*.dylib "$INSTALL_BIN/" 2>/dev/null || true
    done
    ;;
esac

# model
if [ -s "$MODEL_PATH" ]; then
  cp -f "$MODEL_PATH" "$INSTALL_MODELS/bge-m3.gguf"
fi

# generate per-install config with absolute model path
INSTALL_CONFIG="$INSTALL_DIR/config.yaml"
MODEL_ABS="$INSTALL_MODELS/bge-m3.gguf"
if [ "$OS" = "msys2" ] && command -v cygpath >/dev/null 2>&1; then
  MODEL_ABS=$(cygpath -m "$MODEL_ABS")
fi
awk -v model="$MODEL_ABS" '
  /^embedding:/ { in_embed=1; print; next }
  in_embed && /^[[:space:]]+model_path:/ { print "  model_path: \"" model "\""; next }
  /^[a-z]/ && !/^embedding:/ { in_embed=0 }
  { print }
' config.yaml > "$INSTALL_CONFIG"
ok "installed binaries, model and config under $INSTALL_DIR"

# ---------- PATH ----------

step "Adding ~/.lmemorygraph/bin to your PATH…"

INSTALL_BIN_NATIVE="$INSTALL_BIN"
if [ "$OS" = "msys2" ] && command -v cygpath >/dev/null 2>&1; then
  INSTALL_BIN_NATIVE=$(cygpath -w "$INSTALL_BIN")
fi

case "$OS" in
  msys2)
    # Persist on user-level Windows env var via .NET API (no 1024-char setx limit).
    # Idempotent: skip if the directory is already in the user PATH.
    powershell.exe -NoProfile -Command "
      \$cur = [Environment]::GetEnvironmentVariable('Path','User');
      \$add = '$INSTALL_BIN_NATIVE';
      if (\$cur -split ';' -notcontains \$add) {
        \$new = if ([string]::IsNullOrEmpty(\$cur)) { \$add } else { \$cur.TrimEnd(';') + ';' + \$add };
        [Environment]::SetEnvironmentVariable('Path', \$new, 'User');
        Write-Output 'added';
      } else { Write-Output 'already-present'; }
    " | tr -d '\r' | while read -r line; do
        case "$line" in
          added)           ok "added to user PATH (open a new shell to pick it up)" ;;
          already-present) ok "already on user PATH" ;;
        esac
      done
    ;;
  linux|macos)
    # Detect rc file, append a line guarded by a marker so re-runs are idempotent.
    SHELL_NAME=$(basename "${SHELL:-bash}")
    case "$SHELL_NAME" in
      zsh)  RC="$HOME/.zshrc" ;;
      fish) RC="$HOME/.config/fish/config.fish" ;;
      *)    RC="$HOME/.bashrc"; [ "$OS" = "macos" ] && [ -f "$HOME/.bash_profile" ] && RC="$HOME/.bash_profile" ;;
    esac
    mkdir -p "$(dirname "$RC")"
    touch "$RC"
    MARKER="# >>> lmemorygraph PATH <<<"
    if grep -qF "$MARKER" "$RC"; then
      ok "$RC already contains the lmemorygraph PATH entry"
    else
      if [ "$SHELL_NAME" = "fish" ]; then
        printf '\n%s\nset -gx PATH %s $PATH\n' "$MARKER" "$INSTALL_BIN" >> "$RC"
      else
        printf '\n%s\nexport PATH="%s:$PATH"\n' "$MARKER" "$INSTALL_BIN" >> "$RC"
      fi
      ok "appended PATH update to $RC (open a new shell to pick it up)"
    fi
    ;;
esac

# ---------- smoke test ----------

step "Smoke test — auto-start daemon and ensure 'default' profile…"
BIN_CLI="$INSTALL_BIN/memgraph${EXE_SUFFIX}"
export MEMGRAPH_CONFIG="$INSTALL_CONFIG"
if "$BIN_CLI" stats >/dev/null 2>&1; then
  ok "daemon up, profile 'default' usable"
else
  warn "smoke test failed — try '$BIN_CLI stats' to see the daemon logs at $INSTALL_DIR/memgraphd.log"
fi

# ---------- final hints ----------

step "Done."
cat <<EOF

The CLI is installed at:
  $INSTALL_BIN/memgraph${EXE_SUFFIX}

Open a NEW shell (so the PATH change is picked up) and try:
  memgraph profile list
  memgraph insert --summary "hello memgraph" --detail "first node" --keyword test
  memgraph query  "hello memgraph"
  memgraph analytics

Switch profile in the current shell:
  eval "\$(memgraph profile set work)"        # bash/zsh/fish
  memgraph profile set work | iex             # PowerShell

Layout:
  $INSTALL_DIR/
  ├── bin/         (memgraph, memgraphd, llama dlls/so)
  ├── models/      (bge-m3.gguf)
  ├── config.yaml  (used by the daemon)
  ├── profiles/    (one DB per profile)
  └── memgraphd.log
EOF
