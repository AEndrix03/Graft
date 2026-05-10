# Open Code — graft integration

[Open Code](https://opencode.ai) is an open-source AI coding assistant that reads `AGENTS.md` for repo-level instructions.

## Install

```bash
cp integrations/opencode/AGENTS.md ./AGENTS.md
# or append to existing:
cat integrations/opencode/AGENTS.md >> ./AGENTS.md
```

## Daemon

```bash
./build/graftd --config ./config.example.yaml &
export GRAFT_SOCKET=/tmp/graft.sock
```

## Permission

Open Code asks for permission per command by default. To pre-approve `graft`:

Edit `~/.config/opencode/opencode.json` (or project-local `.opencode/opencode.json`):

```json
{
  "permission": {
    "bash": {
      "graft *": "allow"
    }
  }
}
```
