#!/usr/bin/env bash
# Reload graftd: stop the running daemon and let the CLI auto-respawn it
# with the current ~/.graft/config.yaml. graftd has no hot-reload signal.
# Usage: ./scripts/reload.sh

set -euo pipefail

if pgrep -x graftd >/dev/null 2>&1; then
    pkill -x graftd || true
    # Wait briefly for the process to exit and release its socket.
    for _ in 1 2 3 4 5; do
        pgrep -x graftd >/dev/null 2>&1 || break
        sleep 0.2
    done
fi

# Drop any leftover unix-socket files that would block respawn on the same path.
sock_dir="${HOME}/.graft/sockets"
if [ -d "$sock_dir" ]; then
    rm -f "$sock_dir"/* 2>/dev/null || true
fi

# Trigger auto-respawn via a cheap CLI call.
if ! graft stats >/dev/null; then
    echo "graftd did not respawn (graft stats failed)" >&2
    exit 1
fi

echo "Reloaded."
