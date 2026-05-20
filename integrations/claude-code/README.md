# Claude Code — graft skill suite

Due livelli di integrazione:

- **Skill** (`skills/`) — slash command + auto-invocation in base al contesto. L'agent decide quando usarli.
- **Hooks** (`hooks/`) — eseguiti dal harness in modo deterministico su `UserPromptSubmit` / `PostToolUse` / `Stop`. Non dipendono dal modello che si ricorda di usarli. Vedi [`hooks/README.md`](./hooks/README.md) per il setup.

## Skill

Sei skill collaborano:

| Skill              | Trigger                                           | Cosa fa                                                                  |
| ------------------ | ------------------------------------------------- | ------------------------------------------------------------------------ |
| `graft`         | Auto su qualunque problema tecnico non banale     | Master: orchestrazione, profili, reference CLI, troubleshooting.         |
| `graft-init`    | `/graft-init`, "configura graft"            | Configuratore one-shot: scrive il blocco istruzioni in CLAUDE.md (global o local). |
| `recall`           | `/recall …`, "do we have X?", "ricordi se..."     | Cerca con strategia smart: query → retrieve → explore in cascata.        |
| `memoryze`         | `/memoryze …`, "save this", "ricorda questo"      | Distilla la conversazione in 1-5 nodi ben formati e li inserisce.        |
| `learn`            | `/learn …`, "ingest this folder", "porting"       | Batch-ingestion da fonti esterne (codebase, docs): plan + conferma + ingest. |
| `memory-audit`     | `/memory-audit`, "is the graph healthy"           | Health check read-only: hit rate, hoarding, champions, duplicati.        |

## Installazione

La sorgente autorevole e condivisa per skill, hook e istruzioni e'
`integrations/standard`; questa cartella resta un adapter/documentazione per
Claude Code.

Installazione user-scoped automatica:

```bash
graft setup claudecode
```

Il setup automatico copia solo le skill in `~/.claude/skills`; non installa hook
e non modifica `~/.claude/settings.json`.

Copia tutta la directory standard `skills/` nella skill folder di Claude Code:

```bash
# Project-scoped (solo questo repo)
mkdir -p .claude/skills
cp -r integrations/standard/skills/* .claude/skills/

# User-scoped (tutti i progetti)
mkdir -p ~/.claude/skills
cp -r integrations/standard/skills/* ~/.claude/skills/
```

Su Windows PowerShell:

```powershell
$dst = "$env:USERPROFILE\.claude\skills"
New-Item -ItemType Directory -Path $dst -Force | Out-Null
Copy-Item -Recurse integrations\standard\skills\* $dst
```

## Verifica

In una sessione Claude Code:

```
/help
```

Dovresti vedere `graft`, `recall`, `memoryze`, `memory-audit` tra le skill disponibili. Claude le invoca autonomamente quando i `description` matchano il contesto, oppure puoi forzarle con `/<nome>`.

## Reload

Le skill vengono caricate all'avvio. Se modifichi `SKILL.md`, riavvia la sessione.

## Permessi consigliati

Per ridurre i prompt di permesso, in `~/.claude/settings.json` (o project-scoped):

```json
{
  "permissions": {
    "allow": [
      "Bash(graft:*)",
      "Bash(graft profile:*)"
    ]
  }
}
```

## Note di flusso

- **Sempre `/recall` prima di rispondere a un problema non banale.** Le skill lo enfatizzano, ma serve anche disciplina dell'agente.
- **`/memoryze` solo dopo aver risolto qualcosa di non ovvio.** Salvare risposte triviali fa scendere il hit-rate e introduce rumore.
- **`/memory-audit` periodicamente.** La frequenza giusta dipende dal volume: ogni 100 inserts circa, o all'inizio di una sessione lunga.
- **Profili distinti per contesti molto diversi** (`work`, `personal`, project-specific) — evita che ricerche lavorative peschino conoscenza personale e viceversa.

## Note tecniche

- Le skill assumono che `graft` sia in PATH (`scripts/install.sh` lo aggiunge automaticamente).
- Il daemon si auto-avvia al primo comando del CLI; non serve avviarlo manualmente.
- I prompt di tutte le skill sono in inglese per coerenza con la lingua di Claude Code.
