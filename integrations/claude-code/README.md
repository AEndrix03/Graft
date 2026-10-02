# Claude Code — graft skill suite

Un solo livello di integrazione: **skill**, distribuite come plugin dal
marketplace di questo repo (oppure copiate da `graft setup`). `/graft-init`
installa la CLI se manca e scrive la regola d'uso nel CLAUDE.md e in
`.claude/rules/graft.md`. Nessun hook, nessuna modifica a `settings.json`.

## Skill

Sei skill collaborano:

| Skill              | Trigger                                           | Cosa fa                                                                  |
| ------------------ | ------------------------------------------------- | ------------------------------------------------------------------------ |
| `graft`         | Auto su qualunque problema tecnico non banale     | Master: orchestrazione, profili, reference CLI, troubleshooting.         |
| `graft-init`    | `/graft-init`, "configura graft"            | Wiring one-shot: una domanda (globale o progetto), poi scrive la regola in CLAUDE.md e `.claude/rules/graft.md`. |
| `recall`           | `/recall …`, "do we have X?", "ricordi se..."     | Cerca con strategia smart: query → retrieve → explore in cascata.        |
| `memoryze`         | `/memoryze …`, "save this", "ricorda questo"      | Distilla la conversazione in 1-5 nodi ben formati e li inserisce.        |
| `learn`            | `/learn …`, "ingest this folder", "porting"       | Batch-ingestion da fonti esterne (codebase, docs): plan + conferma + ingest. |
| `memory-audit`     | `/memory-audit`, "is the graph healthy"           | Health check read-only: hit rate, hoarding, champions, duplicati.        |

## Installazione

La sorgente unica delle skill e' il plugin in `plugins/graft/skills`; questa
cartella e' solo documentazione per Claude Code.

Via marketplace (consigliato):

```text
/plugin marketplace add AEndrix03/Graft
/plugin install graft@graft
/graft:graft-init     # installa la CLI se manca, poi una domanda e scrive la regola
```

Le skill del plugin hanno il prefisso `graft:` (`/graft:recall`,
`/graft:memoryze`, ...). Se in `~/.claude/skills` ci sono ancora le copie di un
vecchio `graft setup`, ogni skill compare due volte: `/graft:graft-init` le
trova e propone di rimuoverle.

Senza marketplace:

```bash
graft setup        # copia le skill in ~/.claude/skills
/graft-init        # dentro Claude Code: una domanda, poi scrive la regola
```

Oppure a mano, dalla checkout del repo:

```bash
# Project-scoped (solo questo repo)
mkdir -p .claude/skills
cp -r plugins/graft/skills/* .claude/skills/

# User-scoped (tutti i progetti)
mkdir -p ~/.claude/skills
cp -r plugins/graft/skills/* ~/.claude/skills/
```

Su Windows PowerShell:

```powershell
$dst = "$env:USERPROFILE\.claude\skills"
New-Item -ItemType Directory -Path $dst -Force | Out-Null
Copy-Item -Recurse plugins\graft\skills\* $dst
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

- Le skill assumono che `graft` sia in PATH (`scripts/build-from-source.sh` lo aggiunge automaticamente).
- Il daemon si auto-avvia al primo comando del CLI; non serve avviarlo manualmente.
- I prompt di tutte le skill sono in inglese per coerenza con la lingua di Claude Code.
