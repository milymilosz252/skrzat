#!/usr/bin/env node
// Reads a Claude Code status-line payload on stdin and forwards the rate-limit
// window to the stick. Runs detached from the status line itself, throttled,
// and never fails loudly - the status line must stay fast and unbroken.

import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';

const CACHE = path.join(os.homedir(), '.claude', 'stick-usage.json');
const MIN_INTERVAL_MS = 20000;

function readStdin() {
  try { return fs.readFileSync(0, 'utf8'); } catch { return ''; }
}

function pickWindow(rl, ...names) {
  for (const n of names) if (rl?.[n]) return rl[n];
  return null;
}

const raw = readStdin();
let payload;
try { payload = JSON.parse(raw); } catch { process.exit(0); }

const rl = payload.rate_limits;
if (!rl) process.exit(0);

const five = pickWindow(rl, 'five_hour', 'fiveHour', '5h');
const seven = pickWindow(rl, 'seven_day', 'sevenDay', '7d');

const epoch = (v) => {
  if (!v) return 0;
  const t = typeof v === 'number' ? v * (v > 1e12 ? 0.001 : 1) * 1000 : Date.parse(v);
  return Number.isFinite(t) ? Math.floor(t / 1000) : 0;
};
// Passed through exactly as Claude Code reports it - no inversion, so the
// stick shows the same number as /usage and the status line.
const used = (w) => {
  const v = w?.used_percentage ?? w?.usedPercentage ?? w?.utilization;
  return Number.isFinite(Number(v)) ? Math.round(Number(v)) : -1;
};

const usage = {
  five_hour_used: used(five),
  five_hour_reset: epoch(five?.resets_at ?? five?.resetsAt),
  seven_day_used: used(seven),
  seven_day_reset: epoch(seven?.resets_at ?? seven?.resetsAt),
  tokens_today: 0,
  stale: false,
  source: 'statusline',
  at: Date.now(),
};

// Nothing usable -> leave the last good value alone.
if (usage.five_hour_used < 0 && usage.seven_day_used < 0) process.exit(0);

try { fs.writeFileSync(CACHE, JSON.stringify(usage)); } catch { /* best effort */ }

// Throttle the network push; the cache file above is always current.
try {
  const stamp = CACHE + '.pushed';
  let last = 0;
  try { last = fs.statSync(stamp).mtimeMs; } catch { /* first run */ }
  if (Date.now() - last < MIN_INTERVAL_MS) process.exit(0);
  fs.writeFileSync(stamp, '');
} catch { /* best effort */ }

const host = process.env.SKRZAT_HOST || process.env.CLAUDE_STICK_HOST || 'skrzat.local';
const base = host.startsWith('http') ? host : `http://${host}`;
const headers = { 'Content-Type': 'application/json' };
const key = process.env.SKRZAT_KEY || process.env.CLAUDE_STICK_KEY;
if (key) headers['X-Stick-Key'] = key;

const ac = new AbortController();
setTimeout(() => ac.abort(), 4000).unref?.();
fetch(base + '/claude/usage', {
  method: 'POST', headers, body: JSON.stringify(usage), signal: ac.signal,
}).catch(() => {});
