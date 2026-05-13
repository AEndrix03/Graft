# Profiles (multi-tenancy)

A **profile** in graft is a full tenant: its own DB, its own daemon process, its own socket. Two profiles never touch each other unless you explicitly merge them. This is what makes it safe to share a single laptop between very different contexts ("personal notes", "the production rewrite at work", "this open-source project's gotchas") without queries from one bleeding into another.

The CLI is the only component that knows about profiles. Before connecting to the daemon, it sets `GRAFT_SOCKET` and `GRAFT_DB_PATH` in its own environment, which the daemon honours as overrides on top of its YAML config (`src/daemon/main.c::apply_env_overrides`).

```
$GRAFT_HOME/
├── profiles/
│   ├── default/    graft.db, graft.db-shm, graft.db-wal
│   ├── work/       graft.db, ...
│   └── personal/   graft.db, ...
└── sockets/        (Windows only — POSIX uses /tmp/graft-<name>.sock)
```

`$GRAFT_HOME` defaults to:

- POSIX: `~/.graft`
- Windows: `%USERPROFILE%\.graft` (falls back to `%LOCALAPPDATA%`)

## Commands

```bash
graft profile list           # JSON: home, active, profiles[]
graft profile current        # JSON: active
graft profile add <name>
graft profile remove <name> [--yes]
graft profile set <name> [--shell bash|zsh|fish|powershell|cmd]
graft profile export <name> --path <file>
graft profile import --name <name> --file <file> [--force]
graft profile merge --into <name> --from <file> [--overwrite]
graft profile remote bind   <name> --url <file-or-url> [--token T]
graft profile remote status <name>
graft profile remote sync   <name>
graft profile remote detach <name>
```

Profile names match `[a-zA-Z0-9_-]{1,64}`. `default` is reserved (it exists implicitly; you cannot remove it).

## How a profile is laid out on disk

| Path | Contents |
| ---- | -------- |
| `$GRAFT_HOME/profiles/<name>/graft.db` | The SQLite DB with all your nodes, embeddings, edges, FTS5 mirror. |
| `$GRAFT_HOME/profiles/<name>/remote.conf` | Optional remote-sync config (`url=...`, `token=...`). Only present after `profile remote bind`. |
| `/tmp/graft-<name>.sock` (POSIX) | Per-profile daemon socket. |
| `$GRAFT_HOME/sockets/<name>.sock` (Windows) | Same idea, different transport. |

Sockets are owned by the user that ran the daemon. The CLI checks "is a daemon already listening?" before destructive operations to avoid corrupting a live WAL.

## Switching profile for the current shell

`graft profile set <name>` does **not** mutate your shell. It prints the right env-var-export line for the detected shell, and you decide what to do with it:

```bash
# bash / zsh / fish
eval "$(graft profile set work)"

# PowerShell
graft profile set work | Out-String | Invoke-Expression

# cmd
graft profile set work
> set GRAFT_PROFILE=work
```

To make a profile persistent across shell sessions, add the printed line to your shell's rc file (e.g. `~/.bashrc`, `~/.zshrc`, `$PROFILE` for PowerShell).

The CLI also accepts an explicit hint:

```bash
graft profile set work --shell powershell
graft profile set work --shell fish
```

## Export / import

Profiles are plain SQLite files. Export is a `cp`:

```bash
graft profile export work --path work-2026-05.graftprofile
```

It refuses if a daemon is currently running for that profile (would copy a live WAL → inconsistent state). Stop the daemon first or work on a profile that's not the active one.

Import is the inverse:

```bash
graft profile import --name work-restored --file work-2026-05.graftprofile
```

The file is sanity-checked first — it must start with the SQLite "SQLite format 3" magic header. The importer refuses to overwrite an existing profile unless `--force` is passed.

## Merge (combine two profiles into one)

```bash
graft profile merge --into work --from /path/to/other.graftprofile
graft profile merge --into work --from /path/to/other.graftprofile --overwrite
```

Mechanics (`mg_storage_merge_from`):

- Open the source as a second SQLite DB.
- Walk its `nodes` table.
- For each row, check `content_hash` in the target.
  - If absent → insert (idempotent on hash anyway).
  - If present and `--overwrite` → replace title / body / keywords / edges on the target.
  - If present and not `--overwrite` → skip.
- Keyword ids are **remapped by text** (the keyword `text` column is `UNIQUE COLLATE NOCASE`), so the source's auto-increment ids don't leak into the target.
- The whole pass runs in one transaction; either everything goes in or nothing does.

Use cases:

- Onboarding a new machine — `import` once, then `merge` later snapshots.
- Sharing a thematic memory pack with a teammate — export the relevant profile, hand them the file, they `merge --into personal`.
- Building a project-specific profile from `default` — `export default`, `merge --into <project>` into a fresh empty profile.

## Remote (manual sync to a shared SQLite file)

When you want two machines to share a profile **without** running a managed service, you can bind a local profile to a remote SQLite file and sync manually:

```bash
graft profile remote bind   work --url /Volumes/sync/work.graft.db
graft profile remote status work
graft profile remote sync   work
graft profile remote sync   work --auto [--interval SEC]   # detached worker
graft profile remote sync   work --stop                    # kill the worker
graft profile remote detach work
```

What `sync` does:

1. **Pull** the remote: read the remote file, insert rows the local doesn't have (as `origin = REMOTE`), apply delete-wins-remote rules to rows that aren't `LOCAL`.
2. **Push** the local: copy any rows marked local (origin = LOCAL) into the remote file as a `merge_from`.
3. Mark all just-pushed local rows as "pushed" so the next pull doesn't treat them as new.

Since 2026-05, `sync` is routed through the per-profile daemon (`remote_sync` op): the daemon is auto-started if down and uses its already-open storage handle. You no longer need to stop the daemon before running sync.

### Background autosync

`--auto` spawns a detached worker that re-runs the sync every `--interval` seconds (default `300`). Output is silent on the parent shell; everything the worker logs goes to a per-profile log file. Lifecycle files live next to the profile DB:

| Path | Purpose |
| ---- | ------- |
| `$GRAFT_HOME/profiles/<name>/autosync.pid` | PID of the running worker (auto-cleared on graceful stop) |
| `$GRAFT_HOME/profiles/<name>/autosync.log` | Append-only log: one line per tick with `pulled`, `deleted`, `pushed` (or error) |

A second `--auto` invocation while a worker is already running is refused — stop the old one with `--stop` first. If you `remote detach` while a worker is active it will notice on the next tick (`remote.conf missing`) and exit by itself; you don't have to stop it manually. The worker is not auto-restarted on host reboot — wrap it in tmux / systemd / Task Scheduler if you want that.

The expected `--url` is a **file path** to a SQLite DB. HTTP remote sync is reserved for future use — passing an `http://` URL today prints a clear "not available in this build" message. The shared file can live on:

- a synced folder (Dropbox / iCloud / Syncthing),
- a network mount,
- something written by a separate process you control.

`remote.conf` lives at `$GRAFT_HOME/profiles/<name>/remote.conf` and is a tiny key=value file (`url=...` and optionally `token=...`). It's not committed to git automatically.

## Per-profile daemons

When you call `graft <op>` with `GRAFT_PROFILE=work`, the CLI computes:

- `GRAFT_DB_PATH` → `$GRAFT_HOME/profiles/work/graft.db`
- `GRAFT_SOCKET`  → `/tmp/graft-work.sock` (POSIX) or `$GRAFT_HOME/sockets/work.sock` (Windows)

Then it tries to connect. If the socket isn't there yet, `autostart` spawns `graftd` next to the CLI binary with the right `--config` and waits for the socket to come up (up to 20 seconds).

So **each profile has its own daemon process** — there is no multiplexing at the daemon level. This is intentional: it makes profile isolation airtight (one corrupt DB cannot kill the other profiles' service), and the daemons are cheap (loading BGE-M3 is the only meaningful per-process cost, and that cost is paid once per profile per session).

Three profiles = three daemons = three model copies = ~1.8 GB resident if all three are warm. On machines with tight RAM, keep at most two active in parallel; `pkill graftd` stops them cleanly when idle.

## Validation rules

Names must match `[a-zA-Z0-9_-]{1,64}`. Anything else is rejected at the CLI with `invalid profile name`. Length is enforced because the socket path goes through `/tmp` and Linux truncates names there at ~104 chars.

`default` is reserved. You cannot `remove` it; you can `export` / `import` / `merge` against it like any other.

## When to actually use multiple profiles

| Use case | Recommendation |
| -------- | -------------- |
| One machine, one user, one project | Just use `default`. |
| Personal vs employer separation | Two profiles: `personal` and `work`. Switch via shell rc. |
| Per-project notes that must not bleed across | One profile per project. Helpful when projects use the same vocabulary (`auth`, `user`, `service`) but mean different things. |
| Trying out a new threshold tuning | `export default --path default.bak`, mess around, `import --name default --file default.bak --force` to roll back. |
| Sharing with a teammate | `export` and hand them the file; they `merge` into theirs. |

---

## What's missing and how to improve it

- **HTTP remote sync.** Today `remote bind --url http://...` is rejected. The protocol design is open: probably a pull-with-pagination + push-with-ETag against a small server holding the canonical SQLite file. The MCP gateway in `integrations/mcp-server/` could host this without much added surface.
- **Per-profile threads / config.** Each daemon loads the same global `config.yaml`. There is no `~/.graft/profiles/<name>/config.yaml` override. Adding that would let you, e.g., run the `work` profile on GPU and `personal` on CPU.
- **Daemon supervision.** No watchdog and no idle shutdown. A daemon stays alive until you `kill` it or the machine sleeps. Adding "if idle for N minutes, exit cleanly" would free RAM on machines with many profiles.
- **Snapshot / journal of profile ops.** `export` and `import` are atomic only at the SQLite level. There is no log of "profile X was overwritten by user Y on date Z". A small append-only meta file in `$GRAFT_HOME/audit.jsonl` would close that gap for shared / team environments.
- **GUI** for profile management. The viewer today is per-profile by virtue of being served by a per-profile daemon. A small profile-switcher in the viewer header would be friendlier than the CLI for casual users.
