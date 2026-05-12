#!/usr/bin/env node
/*
 * Stop hook for graft (Claude Code + Codex).
 *
 * If the session has accumulated save-candidates from PostToolUse, write a
 * concise /memoryze proposal to <session>.proposal. The next UserPromptSubmit
 * hook surfaces it to the agent (so the proposal lands at the start of the
 * next turn, not as Stop hook stdout — which has client-specific contracts).
 *
 * Output silently; the proposal is consumed by query_inject.js on the next turn.
 */

const { readFileSync, existsSync, writeFileSync, unlinkSync, mkdirSync, readdirSync } = require('fs');
const path = require('path');
const os = require('os');

function inferStateDir() {
  if (process.env.GRAFT_HOOK_STATE_DIR) return process.env.GRAFT_HOOK_STATE_DIR;
  const scriptPath = __filename.toLowerCase();
  const agentDir = scriptPath.includes(`${path.sep}.codex${path.sep}`) ? '.codex' : '.claude';
  return path.join(os.homedir(), agentDir, 'hooks', 'graft', 'state');
}

const STATE_DIR = inferStateDir();

// Restrict the hook state dir to the current user (no-op on Windows, but
// harmless). State files containing session ids and per-turn file paths
// should not be readable by other local accounts.
function safeMkdir(d) { try { mkdirSync(d, { recursive: true, mode: 0o700 }); } catch (_) {} }
function readStdinSync() { try { return readFileSync(0, 'utf8'); } catch (_) { return ''; } }

(function main() {
  safeMkdir(STATE_DIR);

  const raw = readStdinSync().replace(/^﻿/, '');
  if (!raw) return;

  let p;
  try { p = JSON.parse(raw); } catch (_) { return; }

  const sessionId = p && p.session_id;
  if (!sessionId) return;

  // mark_candidate.js writes per-PID files: ${sessionId}.${pid}.candidates.
  // Merge every file matching this session before processing, then delete
  // each one after consumption.
  const prefix = `${sessionId}.`;
  const suffix = '.candidates';
  let candidateFiles = [];
  try {
    candidateFiles = readdirSync(STATE_DIR)
      .filter(name => name.startsWith(prefix) && name.endsWith(suffix))
      .map(name => path.join(STATE_DIR, name));
  } catch (_) { return; }

  if (!candidateFiles.length) return;

  let entries = [];
  for (const f of candidateFiles) {
    try {
      const lines = readFileSync(f, 'utf8').split('\n').filter(Boolean);
      for (const l of lines) {
        try { entries.push(JSON.parse(l)); } catch (_) {}
      }
    } catch (_) {}
  }
  entries = entries.filter(Boolean);

  if (!entries.length) {
    for (const f of candidateFiles) { try { unlinkSync(f); } catch (_) {} }
    return;
  }

  const files = [...new Set(entries.map(e => e.file).filter(Boolean))];
  const tools = [...new Set(entries.map(e => e.tool))];

  let proposal = `This turn modified ${entries.length} entry/entries (${tools.join(', ')}) across ${files.length} file(s):\n`;
  for (const f of files.slice(0, 10)) proposal += `  - ${f}\n`;
  if (files.length > 10) proposal += `  - ... and ${files.length - 10} more\n`;
  proposal += `\nDecide automatically whether these changes solved a non-obvious problem or encoded a reusable decision. If yes, save 1-3 focused memory nodes now using /memoryze when available. Otherwise write a short searchable title, write the body in Markdown with context, fix/decision, why, and minimal snippets, run graft classify --title "<title>", then graft insert with 2-5 good keywords. If the edits were mechanical, incomplete, or not reusable, skip saving.`;

  const proposalFile = path.join(STATE_DIR, `${sessionId}.proposal`);
  try {
    writeFileSync(proposalFile, proposal, { mode: 0o600 });
  } catch (_) {}
  for (const f of candidateFiles) { try { unlinkSync(f); } catch (_) {} }
})();
