<div align="center">

<img src="./assets/graft.png" alt="Graft logo" width="160"/>

# graft

### Your coding agent already solved this. Graft makes sure it remembers.

[![CI](https://img.shields.io/github/actions/workflow/status/AEndrix03/Graft/ci.yml?branch=develop&style=flat-square&label=ci)](https://github.com/AEndrix03/Graft/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/AEndrix03/Graft?style=flat-square)](https://github.com/AEndrix03/Graft/releases)
[![License: Apache 2.0](https://img.shields.io/badge/license-Apache%202.0-blue.svg?style=flat-square)](./LICENSE)

</div>

---

## How it works

<img src="./assets/how-it-works.png" alt="Solved once, remembered, answered instantly" width="100%"/>

Your agent writes down what it learns: the fix, the gotcha, the decision and why.
Next time it runs into the same thing, graft hands it the answer before it starts
over. The agent spends a moment checking it still holds, then moves on.

- **Fewer tokens.** No more whole turns of reasoning for a problem already solved once.
- **Faster.** A lookup takes a fraction of a second.
- **Consistent.** The same problem gets the same answer, not a new invention every session.

---

## Stop solving the same problem twice

```text
without graft    bug → investigate → fix → forgotten   (again, next week)
with graft       bug → recall → check → done
```

---

## Benchmark

<img src="./assets/bench-hero.svg" alt="On questions about a team's own codebase, Claude Code answers 40% faster with 49% fewer tokens with graft, right answers go from 27% to 73% and made-up answers from 27% to 0%" width="100%"/>

- **It stops making things up.** Without graft, a quarter of the answers about the
  team's own code were invented. With graft, none.
- **A wrong note does not derail it.** On 30 questions no note answers, graft
  sometimes offered the wrong note, and the agent set it aside every time.
- **Where it still slips.** graft finds the right note for 88% of the questions.
  When it picks a wrong one, the question is usually about the same technology as
  another note: that is the next thing being fixed.

How it was measured, and how to run it yourself → [`bench/`](./bench/)

---

## Install

### 1. The CLI

<img src="./assets/badges/linux.svg" alt="Linux" height="26"/>

```bash
curl -fsSL https://raw.githubusercontent.com/AEndrix03/Graft/master/install.sh | sh
```

<img src="./assets/badges/windows.svg" alt="Windows" height="26"/>

```powershell
irm https://raw.githubusercontent.com/AEndrix03/Graft/master/install.ps1 | iex
```

```powershell
scoop install https://raw.githubusercontent.com/AEndrix03/Graft/master/bucket/graft.json
```

<img src="./assets/badges/macos.svg" alt="macOS" height="26"/>

```bash
brew tap AEndrix03/graft https://github.com/AEndrix03/Graft.git && brew install graft
```

The installer also copies the skills into every agent it finds. Then run
`/graft-init` inside your agent.

### 2. Or straight from your agent's marketplace

<img src="./assets/badges/claude-code.svg" alt="Claude Code" height="26"/>

```text
/plugin marketplace add AEndrix03/Graft
/plugin install graft@graft
/graft:graft-init
```

<img src="./assets/badges/codex.svg" alt="Codex" height="26"/>

```bash
codex plugin marketplace add AEndrix03/Graft
codex plugin add graft@graft
```

then ask Codex to *set up graft*.

`graft-init` installs the CLI if it is missing, asks whether graft should be on
everywhere or in this project only, and you are done.

Building from source, GPU builds, other options → [`docs/install/`](./docs/install/)

---

## See it work

First, the memory is empty:

```console
$ graft query "spring validation nested dto not working"
{
  "status": 0,
  "result": { "hit": "MISS" }
}
```

The agent investigates and solves the issue. Save the useful part:

```bash
graft insert \
  --title "Spring @Valid must also be applied to nested DTO fields" \
  --body "Without @Valid on the nested field, validation does not cascade into it." \
  --keyword spring-boot \
  --keyword validation \
  --keyword gotcha
```

Weeks later, with different wording:

```console
$ graft query "why are constraints inside my nested request object ignored?"
{
  "status": 0,
  "result": {
    "hit": "STRONG",
    "title": "Spring @Valid must also be applied to nested DTO fields",
    "body": "Without @Valid on the nested field, validation does not cascade into it."
  }
}
```

Different prompt. Same underlying problem.

**Graft surfaces the prior learning. The agent decides whether it is useful.**

---

## Browse your memory

<table>
<tr>
<td width="60%"><img src="./assets/graph-example.png" alt="Every memory in the graph and how they connect"/></td>
<td width="40%"><img src="./assets/graph-exploration.png" alt="An exploration lighting up the notes related to one memory"/></td>
</tr>
<tr>
<td align="center"><sub>Every memory and how it connects</sub></td>
<td align="center"><sub>From one note to the related ones</sub></td>
</tr>
</table>

`graft view` opens it in your browser.

---

## Works with your agent

| | |
|---|---|
| <img src="./assets/badges/claude-code.svg" alt="Claude Code" height="22"/> | plugin from the marketplace |
| <img src="./assets/badges/codex.svg" alt="Codex" height="22"/> | plugin from the marketplace |
| <img src="./assets/badges/opencode.svg" alt="OpenCode" height="22"/> | skills, installed by the CLI |
| <img src="./assets/badges/gemini-cli.svg" alt="Gemini CLI" height="22"/> | [`integrations/gemini-cli/`](./integrations/gemini-cli/) |
| <img src="./assets/badges/claude-desktop.svg" alt="Claude Desktop" height="22"/> | MCP · [`integrations/claude-ai/`](./integrations/claude-ai/) |
| <img src="./assets/badges/chatgpt.svg" alt="ChatGPT" height="22"/> | MCP · [`integrations/chatgpt/`](./integrations/chatgpt/) |
| <img src="./assets/badges/any-agent.svg" alt="Any agent" height="22"/> | anything that can run a command · [`docs/integrations/`](./docs/integrations/) |

---

## Project status

> **Alpha — v0.2.x.** The CLI and its JSON output may still change before 1.0.

Working today:

- one local binary and daemon, memory in a single SQLite file on your machine
- multilingual: a question in Italian finds a note written in English
- plugins for Claude Code and Codex, skills for OpenCode, MCP for Claude Desktop and ChatGPT
- set up once, then hands off: the agent bootstraps each new project, searches,
  fixes stale notes, saves what it learned and keeps the graph tidy on its own
- on Claude Code, recall without asking: the plugin looks up every prompt and
  hands the agent a close note when there is one
- a graph viewer to browse every memory, and usage stats to see what actually gets reused
- a reproducible benchmark in [`bench/`](./bench/)

Coming next:

- **Finding the right note more often.** In the benchmark every answer the agent
  still got wrong with graft came from a lookup that missed or picked a
  neighbouring note. Wrong notes it simply sets aside, so the work goes into
  recall, not into stricter gates.
- **Recall without asking beyond Claude Code.** The same per-prompt lookup for
  the other agents, where their hook systems allow it.
- **Signed Windows binaries** and prebuilt macOS archives.

---

## Documentation

| | |
|---|---|
| **Getting started** | [`docs/install/`](./docs/install/) |
| **Use cases** | [`docs/use-cases.md`](./docs/use-cases.md) |
| **Guide: persistent coding-agent memory** | [`docs/use-cases/persistent-memory-for-coding-agents.md`](./docs/use-cases/persistent-memory-for-coding-agents.md) |
| **Concepts** | [`docs/concepts.md`](./docs/concepts.md) |
| **Integrations** | [`docs/integrations/`](./docs/integrations/) |
| **Architecture** | [`docs/architecture/`](./docs/architecture/) |
| **CLI** | [`docs/cli/`](./docs/cli/) |
| **Retrieval** | [`docs/retrieval/`](./docs/retrieval/) |
| **Storage** | [`docs/storage/`](./docs/storage/) |
| **Embeddings** | [`docs/embeddings/`](./docs/embeddings/) |
| **Profiles** | [`docs/profiles/`](./docs/profiles/) |
| **HTTP API** | [`docs/http-api/`](./docs/http-api/) |

Full documentation → **[`docs/`](./docs/)**

---

## Contributing

```bash
git clone https://github.com/AEndrix03/Graft.git
cd Graft
bash scripts/build-from-source.sh
graft stats
```

Run tests with:

```bash
cmake --build build --target test
```

See [`CONTRIBUTING.md`](./CONTRIBUTING.md).

---

## License

[Apache License 2.0](./LICENSE).

You can use, modify, distribute and embed Graft in proprietary projects subject to the license terms.

---

<div align="center">

<img src="./assets/graft.png" alt="Graft" width="72"/>

### Let your agents keep what they learn.

[`docs`](./docs/) · [`install`](./docs/install/) · [`integrations`](./docs/integrations/) · [`releases`](https://github.com/AEndrix03/Graft/releases) · [`issues`](https://github.com/AEndrix03/Graft/issues)

</div>
