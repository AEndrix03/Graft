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
    const raw = fs.readFileSync(hooksPath, 'utf8').replace(/^\uFEFF/, '').trim();
    if (raw) doc = JSON.parse(raw);
  }
} catch (_) {
  console.error(`cannot parse existing hooks file: ${hooksPath}`);
  process.exit(2);
}
if (!doc || Array.isArray(doc) || typeof doc !== 'object') doc = {};
if (!doc.hooks || Array.isArray(doc.hooks) || typeof doc.hooks !== 'object') doc.hooks = {};
function cmd(script, timeout, statusMessage) {
  const hook = {
    type: 'command',
    command: `node "${script.replace(/"/g, '\\"')}"`,
    timeout,
    __script: script,
  };
  if (statusMessage) hook.statusMessage = statusMessage;
  return hook;
}
function sameHook(hook, script) {
  return hook && hook.type === 'command' && typeof hook.command === 'string' && hook.command.includes(script);
}
function upsert(event, matcher, hook) {
  const groups = Array.isArray(doc.hooks[event]) ? doc.hooks[event] : [];
  let placed = false;
  doc.hooks[event] = groups.map(group => {
    if (!group || typeof group !== 'object') return group;
    const hooks = Array.isArray(group.hooks) ? group.hooks.filter(h => !sameHook(h, hook.__script)) : [];
    const matches = matcher === null ? !Object.prototype.hasOwnProperty.call(group, 'matcher') : group.matcher === matcher;
    if (!matches) return { ...group, hooks };
    placed = true;
    return { ...group, hooks: [...hooks, hook] };
  });
  if (!placed) {
    doc.hooks[event].push(matcher === null ? { hooks: [hook] } : { matcher, hooks: [hook] });
  }
  for (const group of doc.hooks[event]) {
    if (group && Array.isArray(group.hooks)) {
      for (const h of group.hooks) delete h.__script;
    }
  }
}
upsert('UserPromptSubmit', null, cmd(path.posix.join(targetDir, 'query_inject.js'), 10, 'graft cache lookup'));
upsert('PostToolUse', 'apply_patch', cmd(path.posix.join(targetDir, 'mark_candidate.js'), 5));
upsert('Stop', null, cmd(path.posix.join(targetDir, 'propose_memoryze.js'), 5));
fs.mkdirSync(path.dirname(hooksPath), { recursive: true });
fs.writeFileSync(hooksPath, JSON.stringify(doc, null, 2) + '\n');
NODE
ok "hooks.json updated at $HOOKS_PATH"

step "Done."
printf "Restart Codex so it reloads config.toml and hooks.json.\n"
