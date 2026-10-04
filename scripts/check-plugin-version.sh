#!/usr/bin/env bash
# Fails when a plugin manifest's version differs from VERSION. The marketplaces
# serve the plugin straight from the repo, so a stale manifest would ship skills
# that claim a graft version that does not exist.
set -euo pipefail
cd "$(dirname "$0")/.."
version="$(tr -d '[:space:]' < VERSION)"
rc=0
for manifest in plugins/graft/.claude-plugin/plugin.json plugins/graft/.codex-plugin/plugin.json; do
  got="$(sed -n 's/^[[:space:]]*"version":[[:space:]]*"\([^"]*\)".*/\1/p' "$manifest" | head -1)"
  if [ "$got" != "$version" ]; then
    echo "::error title=Plugin version mismatch::$manifest has version '$got', VERSION is '$version'"
    rc=1
  fi
done
exit $rc
