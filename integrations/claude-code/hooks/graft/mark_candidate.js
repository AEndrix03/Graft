#!/usr/bin/env node
/*
 * PostToolUse hook for graft (Claude Code + Codex).
 *
 * Records non-trivial side-effects in a per-session candidate file. The Stop
 * hook later compiles these into a /memoryze proposal that the next
 * UserPromptSubmit hook surfaces.
 *
 * Recognises Claude Code edit tools (Edit/Write/MultiEdit/NotebookEdit) via
 * tool_input.file_path, and Codex apply_patch via the unified-diff text in
 * tool_input.input (extracts file paths from `+++ b/<path>` lines).
 *
 * Outputs nothing — silent record-keeping.
 */

const { readFileSync, appendFileSync, mkdirSync } = require('fs');
const path = require('path');
const os = require('os');

function inferStateDir() {
  if (process.env.GRAFT_HOOK_STATE_DIR) return process.env.GRAFT_HOOK_STATE_DIR;
  const scriptPath = __filename.toLowerCase();
  const agentDir = scriptPath.includes(`${path.sep}.codex${path.sep}`) ? '.codex' : '.claude';
  return path.join(os.homedir(), agentDir, 'hooks', 'graft', 'state');
}

const STATE_DIR = inferStateDir();
// Tool names span multiple agent clients:
//   Claude Code: Edit, Write, MultiEdit, NotebookEdit
//   Codex:       apply_patch
const TOOLS_OF_INTEREST = new Set([
  'Edit', 'Write', 'MultiEdit', 'NotebookEdit',
  'apply_patch',
]);

function safeMkdir(d) { try { mkdirSync(d, { recursive: true }); } catch (_) {} }
function readStdinSync() { try { return readFileSync(0, 'utf8'); } catch (_) { return ''; } }

(function main() {
  safeMkdir(STATE_DIR);

  const raw = readStdinSync().replace(/^﻿/, '');
  if (!raw) return;

  let p;
  try { p = JSON.parse(raw); } catch (_) { return; }

  const sessionId = p && p.session_id;
  const tool = p && p.tool_name;
  if (!sessionId || !TOOLS_OF_INTEREST.has(tool)) return;

  const ti = (p && p.tool_input) || {};
  const ts = new Date().toISOString();

  // Claude Code edit tools carry file_path / notebook_path directly.
  // Codex apply_patch carries the unified diff in tool_input.input — extract
  // touched paths from `+++ b/<path>` lines (and rename targets).
  let files = [];
  if (ti.file_path) files = [ti.file_path];
  else if (ti.notebook_path) files = [ti.notebook_path];
  else if (typeof ti.input === 'string') {
    const seen = new Set();
    let m;
    const patchHeader = /^\*\*\*\s+(?:Add|Update|Delete) File:\s+(.+)$/gm;
    while ((m = patchHeader.exec(ti.input)) !== null) {
      seen.add(m[1].trim());
    }
    const moveHeader = /^\*\*\*\s+Move to:\s+(.+)$/gm;
    while ((m = moveHeader.exec(ti.input)) !== null) {
      seen.add(m[1].trim());
    }
    const unifiedHeader = /^\+\+\+\s+(?:b\/)?([^\s]+)/gm;
    while ((m = unifiedHeader.exec(ti.input)) !== null) {
      if (m[1] !== '/dev/null') seen.add(m[1]);
    }
    files = [...seen];
  }

  const stateFile = path.join(STATE_DIR, `${sessionId}.candidates`);
  try {
    if (!files.length) {
      appendFileSync(stateFile, JSON.stringify({ ts, tool, file: '' }) + '\n');
    } else {
      for (const f of files) {
        appendFileSync(stateFile, JSON.stringify({ ts, tool, file: f }) + '\n');
      }
    }
  } catch (_) {}

  process.stdout.write('<graft-proposal timing="post-tool">\n');
  process.stdout.write(`This turn modified ${files.length || 1} candidate path(s) via ${tool}:\n`);
  for (const f of files.slice(0, 10)) process.stdout.write(`  - ${f}\n`);
  if (!files.length) process.stdout.write('  - <path unavailable>\n');
  if (files.length > 10) process.stdout.write(`  - ... and ${files.length - 10} more\n`);
  process.stdout.write('\nBefore the final response, decide automatically whether this solved a non-obvious problem or encoded a reusable decision. If yes, save 1-3 focused memory nodes now using /memoryze when available, otherwise graft classify + graft insert. If the edits were mechanical or not yet resolved, skip saving.\n');
  process.stdout.write('</graft-proposal>\n');
})();
