# Claude Code — graft skill suite

Due livelli: **skill**, distribuite come plugin dal marketplace di questo repo
(oppure copiate da `graft setup`), e due **hook del plugin**. `/graft-init`
installa la CLI se manca e scrive la regola d'uso nel CLAUDE.md e in
`.claude/rules/graft.md`. Nessuna modifica a `settings.json`.

Dopo il setup l'utente non deve piu' pensare a graft: la regola e' un ciclo di
vita zero-touch. Una volta per sessione l'agente chiama `graft status` (economico,
non avvia il daemon) e fa la manutenzione che la lista `next` chiede, sempre a
lotti limitati e senza bloccare il task: bootstrap progressivo dei progetti nuovi
(`/learn bootstrap`), `apply-safe`, pochi candidati di `graft maintain`. Cerca prima
del lavoro non banale, ripara subito le note smentite dal codice (`supersede`),
salva quello che ha imparato, e non chiede nulla all'utente salvo guasti, sospetta
perdita di dati o azioni irreversibili non decidibili dal contesto.

## Hook del plugin

`plugins/graft/hooks/hooks.json`, attivi solo col plugin installato e abilitato:

| Evento | Comando | Cosa fa |
| ------ | ------- | ------- |
| `SessionStart` | `graft hook session-start` | Se c'e' manutenzione da fare nel progetto, inietta una riga `graft:` con i passi di `graft status`. Altrimenti niente. |
| `UserPromptSubmit` | `graft hook prompt` | `graft query` sul prompt; solo su un hit `STRONG` inietta la nota (id, titolo, inizio del corpo). |

Non avviano mai il daemon, sono silenziosi ed escono 0 su qualunque errore (graft
assente, daemon spento, binario vecchio), timeout 5 s. Opt-out: `GRAFT_HOOKS=0`
(entrambi) o `GRAFT_HOOK_PROMPT=0` (solo il lookup per prompt), nell'ambiente o in
`settings.json` → `env`. `graft setup` (senza plugin) non installa hook.

## Skill

Sei skill collaborano:

| Skill              | Trigger                                           | Cosa fa                                                                  |
| ------------------ | ------------------------------------------------- | ------------------------------------------------------------------------ |
| `graft`         | Auto su qualunque problema tecnico non banale     | Master: orchestrazione, profili, reference CLI, troubleshooting.         |
| `graft-init`    | `/graft-init`, "configura graft"            | Wiring one-shot: una domanda (globale o progetto), poi scrive la regola in CLAUDE.md e `.claude/rules/graft.md`. |
| `recall`           | `/recall …`, "do we have X?", "ricordi se..."     | Cerca con strategia smart: query → retrieve → explore in cascata.        |
| `memoryze`         | `/memoryze …`, "save this", "ricorda questo"      | Distilla la conversazione in 1-5 nodi ben formati e li inserisce.        |
| `learn`            | `/learn …`, `/learn docs`, "ingest this folder"   | Batch-ingestion da fonti esterne (codebase, docs): plan + conferma + ingest. `/learn docs` ingerisce tutta la documentazione del repo, incrementale ai re-run. |
| `memory-audit`     | `/memory-audit`, "is the graph healthy"           | Manutenzione + health check: `graft maintain` (apply-safe, scan, resolve dei candidati), hit rate, hoarding, champions. |

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

- **Le skill manuali restano** (`/recall`, `/memoryze`, `/learn`, `/memory-audit`) per chi vuole guidare a mano o fare debug; nel flusso normale l'agente le usa da solo.
- **`/memoryze` solo dopo aver risolto qualcosa di non ovvio.** Salvare risposte triviali fa scendere il hit-rate e introduce rumore.
- **La manutenzione parte da sola** quando `graft status` / `graft maintain status` la raccomandano; `/memory-audit` serve per un passaggio completo su richiesta.
- **Profili distinti per contesti molto diversi** (`work`, `personal`, project-specific) — evita che ricerche lavorative peschino conoscenza personale e viceversa.

## Note tecniche

- Le skill assumono che `graft` sia in PATH (`scripts/build-from-source.sh` lo aggiunge automaticamente).
- Il daemon si auto-avvia al primo comando del CLI; non serve avviarlo manualmente.
- I prompt di tutte le skill sono in inglese per coerenza con la lingua di Claude Code.
