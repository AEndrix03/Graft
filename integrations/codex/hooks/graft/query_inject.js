#!/usr/bin/env node
/*
 * UserPromptSubmit hook for graft (Claude Code + Codex).
 *
 * Reads { session_id, prompt, ... } from stdin, runs `graft query <prompt>`,
 * and emits a compact context block on stdout — both Claude Code and Codex
 * inject UserPromptSubmit stdout into the agent's prompt for the upcoming turn.
 *
 * Also surfaces any /memoryze proposal queued by the Stop hook on the previous
 * turn (the agent sees it before the user's actual prompt).
 *
 * Failures are silent (exit 0). Latency cap via 5s timeout on the CLI call.
 */

const { execFileSync } = require('child_process');
const { readFileSync, existsSync, unlinkSync, mkdirSync } = require('fs');
const path = require('path');
const os = require('os');

function inferStateDir() {
  if (process.env.GRAFT_HOOK_STATE_DIR) return process.env.GRAFT_HOOK_STATE_DIR;
  const scriptPath = __filename.toLowerCase();
  const agentDir = scriptPath.includes(`${path.sep}.codex${path.sep}`) ? '.codex' : '.claude';
  return path.join(os.homedir(), agentDir, 'hooks', 'graft', 'state');
}

const STATE_DIR = inferStateDir();
const MIN_WORDS_FOR_QUERY = 4;
const WEAK_EXPLORE_DEPTH = positiveInt(process.env.GRAFT_HOOK_WEAK_EXPLORE_DEPTH, 3);
const WEAK_EXPLORE_BEAM = positiveInt(process.env.GRAFT_HOOK_WEAK_EXPLORE_BEAM, 4);

function safeMkdir(d) { try { mkdirSync(d, { recursive: true }); } catch (_) {} }
function readStdinSync() { try { return readFileSync(0, 'utf8'); } catch (_) { return ''; } }
function positiveInt(raw, fallback) {
  const n = Number.parseInt(raw, 10);
  return Number.isFinite(n) && n > 0 ? n : fallback;
}

function runGraft(args) {
  try {
    return execFileSync('graft', args, {
      encoding: 'utf8',
      timeout: 5000,
      stdio: ['ignore', 'pipe', 'ignore'],
    });
  } catch (_) {
    return null;
  }
}

function parseGraft(args) {
  const out = runGraft(args);
  if (!out) return null;
  try { return JSON.parse(out); } catch (_) { return null; }
}

function classifyKeywords(title) {
  if (!title) return [];
  const resp = parseGraft(['classify', '--title', title]);
  const kws = resp && resp.result && resp.result.suggested_keywords;
  return Array.isArray(kws)
    ? kws.filter(k => typeof k === 'string' && k.trim()).slice(0, 5)
    : [];
}

function exploreWeak(title, keywords) {
  if (!title || !keywords.length) return [];
  const args = ['explore', title];
  for (const kw of keywords) args.push('--keyword', kw);
  args.push('--depth', String(WEAK_EXPLORE_DEPTH), '--beam', String(WEAK_EXPLORE_BEAM));
  const resp = parseGraft(args);
  const nodes = resp && resp.result && resp.result.nodes;
  return Array.isArray(nodes) ? nodes.filter(n => n && n.title).slice(0, 8) : [];
}

(function main() {
  safeMkdir(STATE_DIR);

  const raw = readStdinSync().replace(/^﻿/, '');
  if (!raw) return;

  let payload;
  try { payload = JSON.parse(raw); } catch (_) { return; }

  const prompt = (payload && typeof payload.prompt === 'string') ? payload.prompt.trim() : '';
  const sessionId = (payload && typeof payload.session_id === 'string') ? payload.session_id : '';

  // (1) Surface any pending /memoryze proposal from prior turn's Stop hook.
  if (sessionId) {
    const proposalFile = path.join(STATE_DIR, `${sessionId}.proposal`);
    if (existsSync(proposalFile)) {
      try {
        const proposal = readFileSync(proposalFile, 'utf8').trim();
        if (proposal) {
          process.stdout.write('<graft-proposal>\n');
          process.stdout.write(proposal + '\n');
          process.stdout.write('</graft-proposal>\n\n');
        }
        unlinkSync(proposalFile);
      } catch (_) {}
    }
  }

  // (2) Skip query for trivial / acknowledgement-only prompts.
  if (!prompt || prompt.split(/\s+/).filter(Boolean).length < MIN_WORDS_FOR_QUERY) return;

  // (3) Run graft query.
  const resp = parseGraft(['query', prompt]);
  const r = resp && resp.result;
  if (!r || !r.hit) return;

  if (r.hit === 'STRONG') {
    process.stdout.write('<graft-cache hit="STRONG">\n');
    if (r.id_hex) process.stdout.write(`id_hex: ${r.id_hex}\n`);
    if (r.title) process.stdout.write(`title: ${r.title}\n`);
    if (r.body) process.stdout.write(`body: ${r.body}\n`);
    process.stdout.write('</graft-cache>\n');
    return;
  }

  if (r.hit === 'WEAK') {
    const title = r.title || prompt;
    const keywords = classifyKeywords(title);
    const candidates = exploreWeak(title, keywords);

    process.stdout.write('<graft-cache hit="WEAK">\n');
    if (r.id_hex) process.stdout.write(`weak_id_hex: ${r.id_hex}\n`);
    if (r.title) process.stdout.write(`weak_title: ${r.title}\n`);
    if (keywords.length) process.stdout.write(`classified_keywords: ${keywords.join(', ')}\n`);
    process.stdout.write(`explore_depth: ${WEAK_EXPLORE_DEPTH}\n`);
    process.stdout.write(`explore_beam: ${WEAK_EXPLORE_BEAM}\n`);
    if (candidates.length) {
      process.stdout.write('explore_candidates:\n');
      for (const n of candidates) {
        const score = typeof n.score === 'number' ? ` score=${n.score.toFixed(3)}` : '';
        const id = n.id_hex ? ` id_hex=${n.id_hex}` : '';
        process.stdout.write(`- ${n.title}${id}${score}\n`);
      }
    }
    process.stdout.write('If you use this WEAK match as the basis for a different solution, save a new node that strengthens the memory.\n');
    process.stdout.write('</graft-cache>\n');
    return;
  }

  if (r.hit === 'MISS') {
    // Deliberately do NOT inject fallback_retrieve neighbors. The verify
    // pipeline declared the top-1 sub-threshold; surfacing those nodes would
    // contradict the system's own gating and feed retrieval-augmented
    // hallucination. Empirical: on a query whose answer was actually saved
    // in the graph, the top-5 fallback contained 1 tangentially relevant +
    // 4 unrelated nodes — 80% noise. The agent can call /recall explicitly
    // when it wants browsing.
    process.stdout.write('<graft-cache hit="MISS" queried="true"/>\n');
  }
})();
