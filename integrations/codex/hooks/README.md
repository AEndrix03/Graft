# Codex — graft hooks

Three event hooks that move graft from "the agent should remember to use it" to "the harness guarantees it":

| Event              | Script                  | What it does                                                                                              |
| ------------------ | ----------------------- | --------------------------------------------------------------------------------------------------------- |
| `UserPromptSubmit` | `query_inject.js`       | Runs `graft query <prompt>`. STRONG/WEAK hits inject title (and body on STRONG). MISS injects only `<graft-cache hit="MISS"/>` — no fallback neighbors (see "MISS policy" below). Also surfaces any `<graft-proposal>` queued by the previous turn's Stop hook. |
| `PostToolUse` (matcher `apply_patch`) | `mark_candidate.js`     | Records the tool call + extracted file paths in `~/.codex/hooks/graft/state/<session>.candidates`. For Codex's `apply_patch`, the script parses the unified diff in `tool_input.input` and extracts paths from `+++ b/<path>` lines. Silent. |
| `Stop`             | `propose_memoryze.js`   | If the session accumulated candidates, writes a compact `/memoryze` proposal to `<session>.proposal`. The next `UserPromptSubmit` surfaces it. Proposes, never auto-saves. |

The scripts are pure Node, no external deps, BOM-tolerant, all silent on error (exit 0 always). Latency cap via per-hook timeouts. **The same scripts work for both Codex and Claude Code** — see `integrations/claude-code/hooks/`.

## Install

Codex hooks are gated by a feature flag. Both steps are required.

### 1. Enable the feature flag in `~/.codex/config.toml`

```toml
[features]
hooks = true
```

If the `[features]` block already exists, just add the line.

### 2. Copy scripts and write `~/.codex/hooks.json`

```bash
scripts/install-codex-hooks.sh
```

On Windows PowerShell:

```powershell
.\scripts\install-codex-hooks.ps1
```

The installer copies the scripts to `~/.codex/hooks/graft`, writes the graft hook entries in `~/.codex/hooks.json`, and updates `~/.codex/config.toml` from the deprecated `codex_hooks` flag to `hooks = true`.

Manual equivalent:

```bash
mkdir -p ~/.codex/hooks/graft
cp integrations/codex/hooks/graft/*.js ~/.codex/hooks/graft/
```

Then create or merge this block in `~/.codex/hooks.json`:

```json
{
  "hooks": {
    "UserPromptSubmit": [
      {
        "hooks": [
          { "type": "command", "command": "node \"$HOME/.codex/hooks/graft/query_inject.js\"", "timeout": 10, "statusMessage": "graft cache lookup" }
        ]
      }
    ],
    "PostToolUse": [
      {
        "matcher": "apply_patch",
        "hooks": [
          { "type": "command", "command": "node \"$HOME/.codex/hooks/graft/mark_candidate.js\"", "timeout": 5 }
        ]
      }
    ],
    "Stop": [
      {
        "hooks": [
          { "type": "command", "command": "node \"$HOME/.codex/hooks/graft/propose_memoryze.js\"", "timeout": 5 }
        ]
      }
    ]
  }
}
```

On Windows, replace `$HOME` with the absolute path: `C:/Users/<you>/.codex/hooks/graft/...`. The hooks need `node` and `graft` in the PATH visible to Codex at session start. Codex does not reload config at runtime — restart the CLI after changes.

## MISS policy

On a cache MISS, the hook **does not inject the `fallback_retrieve` neighbors**. The verify pipeline already declared the top-1 sub-threshold; surfacing those nodes would contradict the system's own gating and feed retrieval-augmented hallucination. Empirically: on a query whose answer was actually saved in the graph, the top-5 fallback contained 1 tangentially relevant + 4 unrelated nodes (80% noise). The agent calls `/recall` explicitly when it wants browsing.

## Sharing scripts with Claude Code

The three scripts in `graft/` are byte-identical to those in `integrations/claude-code/hooks/graft/`. If you use both clients, install the scripts once and have both `~/.claude/hooks.json` and `~/.codex/hooks.json` point to the same on-disk location. The scripts read agent-specific payload fields generically (`prompt`, `session_id`, `tool_name`, `tool_input.file_path` or `tool_input.input` for diffs).

## Skip rules

- Prompts shorter than 4 words skip the query (avoids noise on short acknowledgement turns).
- All hooks exit 0 on any error — they never block the user's prompt.

## State files

`~/.codex/hooks/graft/state/` accumulates per-session `<session>.candidates` (JSONL) and `<session>.proposal` (text). The proposal is consumed and deleted on the next `UserPromptSubmit`. The candidates file is consumed and deleted by `Stop`.
