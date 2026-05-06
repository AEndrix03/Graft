# Claude Code — memgraph skill

## Installazione

Copia (o symlinka) la directory `skills/memgraph/` dentro la skill folder di Claude Code:

```bash
# Project-scoped (solo questo repo)
mkdir -p .claude/skills
cp -r integrations/claude-code/skills/memgraph .claude/skills/

# User-scoped (tutti i progetti)
mkdir -p ~/.claude/skills
cp -r integrations/claude-code/skills/memgraph ~/.claude/skills/
```

Su Windows PowerShell:
```powershell
New-Item -ItemType Directory -Path "$env:USERPROFILE\.claude\skills" -Force | Out-Null
Copy-Item -Recurse integrations\claude-code\skills\memgraph "$env:USERPROFILE\.claude\skills\"
```

## Verifica

In una sessione Claude Code:

```
/help
```

Dovresti vedere `memgraph` nell'elenco delle skill disponibili. Claude la invocherà autonomamente quando il `description` matcha il contesto (vedi `SKILL.md` frontmatter).

## Reload

Le skill vengono caricate all'avvio di Claude Code. Se modifichi `SKILL.md`, riavvia la sessione.

## Note

- La skill **non** include il binario `memgraph` né avvia il daemon — assume che siano disponibili. Aggiungi un'istruzione in `CLAUDE.md` per ricordare il path se necessario.
- Per ridurre i prompt-permission ogni volta, allowa `Bash(memgraph:*)` nelle settings (`/permissions`).
