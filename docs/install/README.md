# Installation

Graft is intentionally easy to install. You pick one of three paths depending on how much you want to tweak.

| Path | Time | When to pick it |
| ---- | ---- | --------------- |
| **Homebrew tap** (macOS / Linux)  | ~2 minutes | You want it to just work. |
| **GitHub Release archive**        | ~1 minute  | You want a signed prebuilt binary archive. |
| **One-shot installer**            | ~3 minutes | You want a managed build from source with sane defaults. |
| **Manual build**                  | ~5 minutes | You want full control (custom flags, custom prefix, distro packaging). |

All three paths end up at the same binaries: `graft` (CLI) and `graftd` (daemon).
The daemon auto-starts on the first CLI call, so you don't manage process lifecycle.

---

## Homebrew (macOS / Linux)

```bash
brew tap AEndrix03/graft https://github.com/AEndrix03/Graft.git
brew install graft
graft stats
```

What the formula does:

1. Builds **llama.cpp** as a shared library, statically pinned to a known commit.
2. Builds **graft** + **graftd** in release mode, with rpaths so the binaries find the llama.cpp shared libraries inside the Cellar.
3. Downloads the pinned **BGE-M3 Q8_0 GGUF** model (~600 MB) as a checksummed Homebrew resource and installs it under `$(brew --prefix graft)/models/bge-m3.gguf`.
4. Drops a `config.example.yaml` under `$(brew --prefix graft)/` with the model path rewritten to the Cellar location.
5. Bundles the prebuilt **viewer SPA** (`viewer/dist/`) under `$(brew --prefix graft)/viewer/`.

User data still lives under `~/.graft/` (profiles, sockets, usage log). Upgrades touch only the Cellar — your graph survives.

To customise runtime settings, copy the example config once:

```bash
mkdir -p ~/.graft
cp "$(brew --prefix graft)/config.example.yaml" ~/.graft/config.yaml
# edit ~/.graft/config.yaml
graft stats
```

### Track the bleeding edge

```bash
brew install --HEAD graft        # pulls master
brew reinstall --HEAD graft      # re-pull and rebuild
brew test graft                  # smoke check
```

---

## GitHub Release archives

Download the archive for your platform from GitHub Releases, verify it with
`SHA256SUMS` and the attached Sigstore signature, then extract it into
`~/.graft/`.

After a release-based install, future updates can be applied with:

```bash
graft upgrade
graft upgrade --check
```

`graft upgrade` never overwrites profiles, DBs, models, or `~/.graft/config.yaml`.

---

## One-shot installer

```bash
git clone https://github.com/AEndrix03/graft.git
cd graft
bash scripts/install.sh          # Linux, macOS, MSYS2 on Windows
pwsh scripts/install.ps1         # Native Windows PowerShell — auto-installs MSYS2 if needed
```

The installer is **idempotent** — rerun it freely; it does not double-write anything.

What it does, in order:

1. Checks for the required system packages (`cmake`, `git`, `pkgconf`, `libyaml`, `sqlite`, `curl`).
2. Initialises submodules (`llama.cpp`, `mpack`, `BLAKE3`, `sqlite-vec`).
3. Builds **llama.cpp** with sensible flags (no server, no tools, no examples — just the embeddings backend).
4. Downloads **BGE-M3 Q8_0 GGUF** to `models/bge-m3.gguf`.
5. Builds **graft** + **graftd** in `build/`.
6. Runs a smoke check (`graft stats`).
7. Activates the **commit-msg policy hook** for contributors (`.git/hooks/commit-msg`).

If any step fails the installer prints the actual command that failed, so you can re-run only the broken step.

---

## GPU acceleration

Default is CPU. To build llama.cpp with GPU offload, pass `GRAFT_GPU`:

```bash
GRAFT_GPU=cuda  bash scripts/install.sh        # NVIDIA CUDA
GRAFT_GPU=hip   bash scripts/install.sh        # AMD ROCm 6 or 7
pwsh scripts/install.ps1 -Gpu cuda             # Windows PowerShell equivalent
```

Then opt in to GPU offload in `config.yaml`:

```yaml
embedding:
  hardware_accel: true
```

> ⚠ The daemon **refuses to start** with `hardware_accel: true` on a CPU-only build. There is no silent CPU fallback — you'll see a clear error in stderr / the log. This is deliberate: silent fallback hides huge latency regressions.

CUDA / ROCm requirements:

- NVIDIA: a working CUDA toolkit (12.x recommended) and a driver matching it.
- AMD: ROCm 6 or 7 with `hipcc` on `PATH`. ROCm 5 is not supported (llama.cpp dropped it).

---

## Manual build

If you'd rather drive each step yourself:

```bash
# 1. Submodules
git submodule update --init --recursive

# 2. llama.cpp (CPU build; add -DGGML_CUDA=ON or -DGGML_HIP=ON for GPU)
cmake -S third_party/llama.cpp -B third_party/llama.cpp/build \
      -DBUILD_SHARED_LIBS=ON -DGGML_NATIVE=ON \
      -DLLAMA_CURL=OFF -DLLAMA_BUILD_SERVER=OFF \
      -DLLAMA_BUILD_TOOLS=OFF -DLLAMA_BUILD_EXAMPLES=OFF \
      -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_COMMON=OFF \
      -DCMAKE_BUILD_TYPE=Release
cmake --build third_party/llama.cpp/build -j

# 3. BGE-M3 GGUF
mkdir -p models
curl -L --ssl-no-revoke -o models/bge-m3.gguf \
  https://huggingface.co/lm-kit/bge-m3-gguf/resolve/main/bge-m3-Q8_0.gguf

# 4. graft
cmake -B build
cmake --build build -j
```

You get:

| Binary | What it is |
| ------ | ---------- |
| `build/graft`  | The CLI (thin client; auto-spawns the daemon if needed) |
| `build/graftd` | The daemon (long-running; owns the DB and the embedding model) |

Optional: `cmake --build build --target test` runs the C unit tests under `tests/`.

---

## First-run check

```bash
graft stats
```

If this prints a JSON object with `n_nodes: 0` and similarity-distribution percentiles (all zeros at first), you're good.

If it prints an error mentioning the daemon socket, run it once more — the **first** call does the cold-start dance (load the GGUF model into memory, open the DB, listen on the socket); the second call hits the warm daemon in milliseconds.

### Cold-start budget

| Operation                | Cost (warm) | Cost (cold first call) |
| ------------------------ | ----------- | ---------------------- |
| `graft stats`            | <50 ms      | 1–2 s                  |
| `graft query <text>`     | 30–80 ms    | 1–2 s                  |
| `graft insert ...`       | 60–150 ms   | 1–2 s                  |
| `graft retrieve ...`     | 40–120 ms   | 1–2 s                  |
| `graft view`             | opens browser | builds viewer SPA on first run (npm install + npm run build) |

The cold cost is dominated by loading the Q8_0 BGE-M3 weights into the daemon's process — about 600 MB, mmap'd. On a warm OS page cache the second cold start is closer to 400 ms.

---

## Verifying the install

A real round-trip:

```bash
graft insert \
  --title "First memory" \
  --body  "If this is retrievable below, graft is wired correctly." \
  --keyword smoke-test

graft query "the very first thing I saved"
```

If the `query` returns `"hit": "STRONG"` (or at worst `"WEAK"`) and the body of the inserted node, the full pipeline (embedding ↔ vector index ↔ FTS5 ↔ verify) is working end-to-end.

---

## Where things live

| Path | What is there |
| ---- | ------------- |
| `~/.graft/profiles/<name>/graft.db` | Per-profile SQLite DB (and WAL / SHM siblings while the daemon is running). |
| `/tmp/graft-<name>.sock` (POSIX)    | Per-profile daemon socket. |
| `~\.graft\sockets\<name>.sock` (Windows) | Per-profile daemon socket. |
| `~/.graft/config.yaml`              | User config; overrides the example config when present. |
| `~/.graft/usage.jsonl`              | Append-only usage log (one JSON line per CLI invocation). Used by `graft analytics`. |
| `$(brew --prefix graft)/...`        | Homebrew Cellar contents (binaries, model, viewer SPA, example config). |

You can override any of these with environment variables. See [`configuration/`](../configuration/) for the full list.

---

## What's missing and how to improve it

- **Package-manager coverage beyond Homebrew.** Signed GitHub Release archives are the base layer; distro-native packages still need maintainers.
- **Linux distro packages** (apt / dnf / pacman / AUR). Out of scope for the core team; community PRs welcome.
- **Container image**. A `Dockerfile` that builds graft + the viewer + the OAuth gateway in one image would unlock easy deployment behind any reverse proxy.
- **Windows MSI / `winget`**. Today the only Windows path is the PowerShell installer or a manual build under MSYS2 / MinGW.
- **Model download integrity**. The Homebrew formula pins the SHA256; the shell installer does not. Adding a checksum check after `curl` would close that gap.
- **A genuine `--check` mode for `scripts/install.sh`** that prints what would be installed without writing anything. Useful for CI and for reviewers.
