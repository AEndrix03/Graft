# Open Code — memgraph integration

[Open Code](https://opencode.ai) is an open-source AI coding assistant that reads `AGENTS.md` for repo-level instructions.

## Install

```bash
cp integrations/opencode/AGENTS.md ./AGENTS.md
# or append to existing:
cat integrations/opencode/AGENTS.md >> ./AGENTS.md
```

## Daemon

```bash
./build/memgraphd --config ./config.example.yaml &
export MEMGRAPH_SOCKET=/tmp/memgraph.sock
```

## Permission

Open Code asks for permission per command by default. To pre-approve `memgraph`:

Edit `~/.config/opencode/opencode.json` (or project-local `.opencode/opencode.json`):

```json
{
  "permission": {
    "bash": {
      "memgraph *": "allow"
    }
  }
}
```
