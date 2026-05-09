# Codex — memgraph hooks

Three event hooks that move memgraph from "the agent should remember to use it" to "the harness guarantees it":

| Event              | Script                  | What it does                                                                                              |
| ------------------ | ----------------------- | --------------------------------------------------------------------------------------------------------- |
| `UserPromptSubmit` | `query_inject.js`       | Runs `memgraph query <prompt>`. STRONG/WEAK hits inject summary (and detail on STRONG). MISS injects only `<memgraph-cache hit="MISS"/>` — no fallback neighbors (see "MISS policy" below). Also surfaces any `<memgraph-proposal>` queued by the previous turn's Stop hook. |
| `PostToolUse` (matcher `apply_patch`) | `mark_candidate.js`     | Records the tool call + extracted file paths in `~/.codex/hooks/memgraph/state/<session>.candidates`. For Codex's `apply_patch`, the script parses the unified diff in `tool_input.input` and extracts paths from `+++ b/<path>` lines. Silent. |
| `Stop`             | `propose_memoryze.js`   | If the session accumulated candidates, writes a compact `/memoryze` proposal to `<session>.proposal`. The next `UserPromptSubmit` surfaces it. Proposes, never auto-saves. |

The scripts are pure Node, no external deps, BOM-tolerant, all silent on error (exit 0 always). Latency cap via per-hook timeouts. **The same scripts work for both Codex and Claude Code** — see `integrations/claude-code/hooks/`.

## Install

Codex hooks are gated by a feature flag. Both steps are required.

### 1. Enable the feature flag in `~/.codex/config.toml`

```toml
[features]
codex_hooks = true
```

If the `[features]` block already exists, just add the line.

### 2. Copy scripts and write `~/.codex/hooks.json`

```bash
mkdir -p ~/.codex/hooks/memgraph
cp integrations/codex/hooks/memgraph/*.js ~/.codex/hooks/memgraph/
```

Then create `~/.codex/hooks.json`:

```json
{
  "hooks": {
    "UserPromptSubmit": [
      {
        "hooks": [
          { "type": "command", "command": "node \"$HOME/.codex/hooks/memgraph/query_inject.js\"", "timeout": 10, "statusMessage": "memgraph cache lookup" }
        ]
      }
    ],
    "PostToolUse": [
      {
        "matcher": "apply_patch",
        "hooks": [
          { "type": "command", "command": "node \"$HOME/.codex/hooks/memgraph/mark_candidate.js\"", "timeout": 5 }
        ]
      }
    ],
    "Stop": [
      {
        "hooks": [
          { "type": "command", "command": "node \"$HOME/.codex/hooks/memgraph/propose_memoryze.js\"", "timeout": 5 }
        ]
      }
    ]
  }
}
```

On Windows, replace `$HOME` with the absolute path: `C:/Users/<you>/.codex/hooks/memgraph/...`. The hooks need `node` and `memgraph` in the PATH visible to Codex at session start. Codex does not reload config at runtime — restart the CLI after changes.

## MISS policy

On a cache MISS, the hook **does not inject the `fallback_retrieve` neighbors**. The verify pipeline already declared the top-1 sub-threshold; surfacing those nodes would contradict the system's own gating and feed retrieval-augmented hallucination. Empirically: on a query whose answer was actually saved in the graph, the top-5 fallback contained 1 tangentially relevant + 4 unrelated nodes (80% noise). The agent calls `/recall` explicitly when it wants browsing.

## Sharing scripts with Claude Code

The three scripts in `memgraph/` are byte-identical to those in `integrations/claude-code/hooks/memgraph/`. If you use both clients, install the scripts once and have both `~/.claude/hooks.json` and `~/.codex/hooks.json` point to the same on-disk location. The scripts read agent-specific payload fields generically (`prompt`, `session_id`, `tool_name`, `tool_input.file_path` or `tool_input.input` for diffs).

## Skip rules

- Prompts shorter than 4 words skip the query (avoids noise on short acknowledgement turns).
- All hooks exit 0 on any error — they never block the user's prompt.

## State files

`~/.codex/hooks/memgraph/state/` accumulates per-session `<session>.candidates` (JSONL) and `<session>.proposal` (text). The proposal is consumed and deleted on the next `UserPromptSubmit`. The candidates file is consumed and deleted by `Stop`.
