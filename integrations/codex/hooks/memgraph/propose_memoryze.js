#!/usr/bin/env node
/*
 * Stop hook for memgraph (Claude Code + Codex).
 *
 * If the session has accumulated save-candidates from PostToolUse, write a
 * concise /memoryze proposal to <session>.proposal. The next UserPromptSubmit
 * hook surfaces it to the agent (so the proposal lands at the start of the
 * next turn, not as Stop hook stdout — which has client-specific contracts).
 *
 * Output silently; the proposal is consumed by query_inject.js on the next turn.
 */

const { readFileSync, existsSync, writeFileSync, unlinkSync, mkdirSync } = require('fs');
const path = require('path');
const os = require('os');

function inferStateDir() {
  if (process.env.MEMGRAPH_HOOK_STATE_DIR) return process.env.MEMGRAPH_HOOK_STATE_DIR;
  const scriptPath = __filename.toLowerCase();
  const agentDir = scriptPath.includes(`${path.sep}.codex${path.sep}`) ? '.codex' : '.claude';
  return path.join(os.homedir(), agentDir, 'hooks', 'memgraph', 'state');
}

const STATE_DIR = inferStateDir();

function safeMkdir(d) { try { mkdirSync(d, { recursive: true }); } catch (_) {} }
function readStdinSync() { try { return readFileSync(0, 'utf8'); } catch (_) { return ''; } }

(function main() {
  safeMkdir(STATE_DIR);

  const raw = readStdinSync().replace(/^﻿/, '');
  if (!raw) return;

  let p;
  try { p = JSON.parse(raw); } catch (_) { return; }

  const sessionId = p && p.session_id;
  if (!sessionId) return;

  const candidatesFile = path.join(STATE_DIR, `${sessionId}.candidates`);
  if (!existsSync(candidatesFile)) return;

  let entries = [];
  try {
    entries = readFileSync(candidatesFile, 'utf8')
      .split('\n').filter(Boolean)
      .map(l => { try { return JSON.parse(l); } catch (_) { return null; } })
      .filter(Boolean);
  } catch (_) { return; }

  if (!entries.length) {
    try { unlinkSync(candidatesFile); } catch (_) {}
    return;
  }

  const files = [...new Set(entries.map(e => e.file).filter(Boolean))];
  const tools = [...new Set(entries.map(e => e.tool))];

  let proposal = `This turn modified ${entries.length} entry/entries (${tools.join(', ')}) across ${files.length} file(s):\n`;
  for (const f of files.slice(0, 10)) proposal += `  - ${f}\n`;
  if (files.length > 10) proposal += `  - ... and ${files.length - 10} more\n`;
  proposal += `\nIf any of these solved a non-obvious problem or encoded a decision, run /memoryze now (1-3 atomic nodes). Skip if it was all mechanical work future-you wouldn't search for.`;

  const proposalFile = path.join(STATE_DIR, `${sessionId}.proposal`);
  try {
    writeFileSync(proposalFile, proposal);
    unlinkSync(candidatesFile);
  } catch (_) {}
})();
