# graft - long-term agent memory

This project uses `graft` for persistent graph memory.

Use `graft query "<problem restated>"` before solving non-trivial technical
problems. If it returns `STRONG`, reuse the cached answer. If it returns `WEAK`,
review the candidate before using it. If it returns `MISS`, continue normally
and use `retrieve` or `explore` only when broader context is useful.

After solving a non-obvious reusable problem, save a node. The `title` is a
short searchable summary. The `body` is Markdown with context, fix/decision,
why, and minimal snippets. Always run `graft classify --title "<title>"` before
`graft insert`, then insert with 2-5 good keywords.

Skip memory for trivial edits, secrets, and chit-chat.
