#!/usr/bin/env bash
# Install graft hooks for Codex.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CODEX_HOME="${CODEX_HOME:-$HOME/.codex}"
SOURCE_DIR="$REPO_ROOT/integrations/codex/hooks/graft"
TARGET_DIR="$CODEX_HOME/hooks/graft"
CONFIG_PATH="$CODEX_HOME/config.toml"
HOOKS_PATH="$CODEX_HOME/hooks.json"

step() { printf "\n==> %s\n" "$*"; }
ok() { printf "  [OK] %s\n" "$*"; }

[ -d "$SOURCE_DIR" ] || { echo "missing hook source directory: $SOURCE_DIR" >&2; exit 1; }

step "Copying Codex graft hooks..."
mkdir -p "$TARGET_DIR"
cp -f "$SOURCE_DIR"/*.js "$TARGET_DIR/"
ok "hooks copied to $TARGET_DIR"

step "Updating Codex feature flag..."
mkdir -p "$CODEX_HOME"
if [ ! -f "$CONFIG_PATH" ]; then
  printf '[features]\nhooks = true\n' > "$CONFIG_PATH"
else
  node - "$CONFIG_PATH" <<'NODE'
const fs = require('fs');
const path = process.argv[2];
let text = fs.readFileSync(path, 'utf8');
text = text.replace(/^\s*codex_hooks\s*=\s*true\s*\r?\n?/gm, '');
if (/^\[features\]\s*$/m.test(text)) {
  if (/^\s*hooks\s*=/m.test(text)) {
    text = text.replace(/^\s*hooks\s*=.*$/m, 'hooks = true');
  } else {
    text = text.replace(/^(\[features\]\s*)$/m, '$1\nhooks = true');
  }
} else {
  text = text.replace(/\s*$/, '') + '\n\n[features]\nhooks = true\n';
}
fs.writeFileSync(path, text);
NODE
fi
ok "enabled [features].hooks"

step "Writing Codex hooks.json entries..."
node - "$HOOKS_PATH" "$TARGET_DIR" <<'NODE'
const fs = require('fs');
const path = require('path');
const hooksPath = process.argv[2];
const targetDir = process.argv[3].replace(/\\/g, '/');
let doc = {};
try {
  if (fs.existsSync(hooksPath)) {
    const raw = fs.readFileSync(hooksPath, 'utf8').trim();
    if (raw) doc = JSON.parse(raw);
  }
} catch (_) {
  doc = {};
}
doc.hooks = {
  UserPromptSubmit: [
    {
      hooks: [
        {
          type: 'command',
          command: `node "${path.posix.join(targetDir, 'query_inject.js')}"`,
          timeout: 10,
          statusMessage: 'graft cache lookup',
        },
      ],
    },
  ],
  PostToolUse: [
    {
      matcher: 'apply_patch',
      hooks: [
        {
          type: 'command',
          command: `node "${path.posix.join(targetDir, 'mark_candidate.js')}"`,
          timeout: 5,
        },
      ],
    },
  ],
  Stop: [
    {
      hooks: [
        {
          type: 'command',
          command: `node "${path.posix.join(targetDir, 'propose_memoryze.js')}"`,
          timeout: 5,
        },
      ],
    },
  ],
};
fs.mkdirSync(path.dirname(hooksPath), { recursive: true });
fs.writeFileSync(hooksPath, JSON.stringify(doc, null, 2) + '\n');
NODE
ok "hooks.json updated at $HOOKS_PATH"

step "Done."
printf "Restart Codex so it reloads config.toml and hooks.json.\n"
