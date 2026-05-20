# OpenCode - graft integration

[OpenCode](https://opencode.ai) reads `AGENTS.md` for repo-level instructions
and supports native skills in `~/.config/opencode/skills/<name>/SKILL.md`.
The shared source for instructions and skills is `integrations/standard`.

## Install

```bash
graft setup opencode
```

Setup installs compatible skills to `~/.config/opencode/skills` only. It does
not write `AGENTS.md` or agent settings.

For project-local instructions, copy the snippet manually:

```bash
cat integrations/standard/project-snippet.md >> ./AGENTS.md
```

## Permission

OpenCode asks for permission per command by default. To pre-approve `graft`,
edit `~/.config/opencode/opencode.json` or project-local `.opencode/opencode.json`:

```json
{
  "permission": {
    "bash": {
      "graft *": "allow"
    }
  }
}
```
