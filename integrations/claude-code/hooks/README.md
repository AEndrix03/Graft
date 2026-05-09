# Claude Code — memgraph hooks

Three event hooks that move memgraph from "the agent should remember to use it" to "the harness guarantees it":

| Event              | Script                  | What it does                                                                                              |
| ------------------ | ----------------------- | --------------------------------------------------------------------------------------------------------- |
| `UserPromptSubmit` | `query_inject.js`       | Runs `memgraph query <prompt>`. STRONG/WEAK hits inject summary (and detail on STRONG). MISS injects only `<memgraph-cache hit="MISS"/>` — no fallback neighbors (see "MISS policy" below). Also surfaces any `<memgraph-proposal>` queued by the previous turn's Stop hook. |
| `PostToolUse` (matcher `Edit\|Write\|MultiEdit\|NotebookEdit`) | `mark_candidate.js`     | Records the tool call + file path in `~/.claude/hooks/memgraph/state/<session>.candidates`. Silent. |
| `Stop`             | `propose_memoryze.js`   | If the session accumulated candidates, writes a compact `/memoryze` proposal to `<session>.proposal`. The next `UserPromptSubmit` surfaces it. Proposes, never auto-saves. |

The scripts are pure Node, no external deps, BOM-tolerant, all silent on error (exit 0 always). Latency cap via per-hook timeouts.

## Install

```bash
# 1. Copy scripts to user-level hooks dir.
mkdir -p ~/.claude/hooks/memgraph
cp integrations/claude-code/hooks/memgraph/*.js ~/.claude/hooks/memgraph/

# 2. Wire the hooks into ~/.claude/settings.json (merge with any existing
#    hooks). Example block:
```

```json
{
  "hooks": {
    "UserPromptSubmit": [
      {
        "hooks": [
          { "type": "command", "command": "node \"$HOME/.claude/hooks/memgraph/query_inject.js\"", "timeout": 10 }
        ]
      }
    ],
    "PostToolUse": [
      {
        "matcher": "Edit|Write|MultiEdit|NotebookEdit",
        "hooks": [
          { "type": "command", "command": "node \"$HOME/.claude/hooks/memgraph/mark_candidate.js\"", "timeout": 5 }
        ]
      }
    ],
    "Stop": [
      {
        "hooks": [
          { "type": "command", "command": "node \"$HOME/.claude/hooks/memgraph/propose_memoryze.js\"", "timeout": 5 }
        ]
      }
    ]
  }
}
```

On Windows, replace `$HOME` with the absolute path: `C:/Users/<you>/.claude/hooks/memgraph/...`. The hooks need `node` and `memgraph` in the PATH visible to Claude Code at session start.

## MISS policy

On a cache MISS, the hook **does not inject the `fallback_retrieve` neighbors**. The verify pipeline already declared the top-1 sub-threshold; surfacing those nodes would contradict the system's own gating and feed retrieval-augmented hallucination. Empirically: on a query whose answer was actually saved in the graph, the top-5 fallback contained 1 tangentially relevant + 4 unrelated nodes (80% noise). The agent calls `/recall` explicitly when it wants browsing.

## Skip rules

- Prompts shorter than 4 words skip the query (avoids noise on "ok thanks" turns).
- All hooks exit 0 on any error — they never block the user's prompt.

## State files

`~/.claude/hooks/memgraph/state/` accumulates per-session `<session>.candidates` (JSONL) and `<session>.proposal` (text). The proposal is consumed and deleted on the next `UserPromptSubmit`. The candidates file is consumed and deleted by `Stop`. Stale files from crashed sessions are harmless but you can clean them periodically.
