#!/usr/bin/env node
/*
 * skrzat-server — a small local daemon that sits between your Mac and the
 * stick on your desk.
 *
 * It exists for three reasons that the plain CLI cannot cover:
 *
 *   1. Freshness. Usage limits otherwise only move when a Claude Code status
 *      line happens to render. The daemon refreshes them on a fixed interval.
 *   2. Addressing. mDNS (`skrzat.local`) occasionally fails to resolve. The
 *      daemon remembers the last good IP and falls back to it, so a momentary
 *      mDNS hiccup does not turn into a failed question.
 *   3. One door. Anything on the machine - a script, a Makefile, another
 *      agent - can reach the stick over plain localhost HTTP without knowing
 *      where it lives or how it is addressed.
 *
 * Binds to 127.0.0.1 only. Nothing here is reachable from the network.
 */

import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';

const PORT = Number(process.env.SKRZAT_SERVER_PORT || 8787);
const HOST_NAME = process.env.SKRZAT_HOST || 'skrzat.local';
const KEY = process.env.SKRZAT_KEY || '';
const USAGE_EVERY_MS = Number(process.env.SKRZAT_USAGE_INTERVAL_MS || 30000);

const STATE_DIR = path.join(os.homedir(), '.config', 'skrzat');
const STATE_FILE = path.join(STATE_DIR, 'state.json');

const log = (...a) => console.log(new Date().toISOString(), ...a);

// ---------------------------------------------------------------- state ----

let state = { lastIp: null, lastSeen: 0, asks: 0, usagePushes: 0, errors: 0 };
try { state = { ...state, ...JSON.parse(fs.readFileSync(STATE_FILE, 'utf8')) }; } catch {}
function saveState() {
  try {
    fs.mkdirSync(STATE_DIR, { recursive: true });
    fs.writeFileSync(STATE_FILE, JSON.stringify(state, null, 2));
  } catch {}
}

// -------------------------------------------------------------- stick io ---

function headers() {
  const h = { 'Content-Type': 'application/json' };
  if (KEY) h['X-Stick-Key'] = KEY;
  return h;
}

// Try the mDNS name first, then the last IP that worked. Whichever answers
// becomes the new preferred address.
async function stick(pathname, { method = 'GET', body, timeoutMs = 8000 } = {}) {
  const candidates = [];
  if (HOST_NAME) candidates.push(`http://${HOST_NAME}`);
  if (state.lastIp) candidates.push(`http://${state.lastIp}`);

  let lastErr;
  for (const base of candidates) {
    const ac = new AbortController();
    const t = setTimeout(() => ac.abort(), timeoutMs);
    try {
      const res = await fetch(base + pathname, {
        method, headers: headers(),
        body: body ? JSON.stringify(body) : undefined,
        signal: ac.signal,
      });
      const text = await res.text();
      let json; try { json = JSON.parse(text); } catch { json = { raw: text }; }
      if (!res.ok) throw new Error(json.error || `HTTP ${res.status}`);

      state.lastSeen = Date.now();
      if (pathname === '/status' && json.ip && json.ip !== state.lastIp) {
        state.lastIp = json.ip;
        log('stick address learned:', json.ip);
        saveState();
      }
      return json;
    } catch (e) {
      lastErr = e;
    } finally {
      clearTimeout(t);
    }
  }
  state.errors++;
  throw lastErr || new Error('stick unreachable');
}

// ------------------------------------------------------------ usage feed ---

// Reads whatever Claude Code has already written locally. Never touches
// credentials and never calls the Anthropic API.
function readUsage() {
  const home = os.homedir();
  const withTokens = (u) => {
    try {
      const stats = JSON.parse(
        fs.readFileSync(path.join(home, '.claude', 'stats-cache.json'), 'utf8'));
      const today = new Date().toISOString().slice(0, 10);
      const row = (stats.dailyModelTokens || []).find((r) => r.date === today);
      if (row) u.tokens_today =
        Object.values(row.tokensByModel || {}).reduce((a, b) => a + Number(b), 0);
    } catch {}
    return u;
  };

  // 1. Live feed written by the status-line wrapper.
  try {
    const fed = JSON.parse(
      fs.readFileSync(path.join(home, '.claude', 'stick-usage.json'), 'utf8'));
    const ageH = (Date.now() - (fed.at || 0)) / 3600000;
    const rolled = fed.five_hour_reset && fed.five_hour_reset < Date.now() / 1000;
    if (!rolled && ageH < 3) return withTokens({ ...fed, stale: false });
  } catch {}

  // 2. Claude Code's own cache - correct, but often hours old.
  const out = {
    five_hour_used: -1, five_hour_reset: 0,
    seven_day_used: -1, seven_day_reset: 0,
    tokens_today: 0, stale: true,
  };
  try {
    const cfg = JSON.parse(fs.readFileSync(path.join(home, '.claude.json'), 'utf8'));
    const u = cfg.cachedUsageUtilization?.utilization;
    const epoch = (iso) => (iso ? Math.floor(Date.parse(iso) / 1000) : 0);
    if (u?.five_hour) {
      out.five_hour_used = Math.round(Number(u.five_hour.utilization ?? 0));
      out.five_hour_reset = epoch(u.five_hour.resets_at);
    }
    if (u?.seven_day) {
      out.seven_day_used = Math.round(Number(u.seven_day.utilization ?? 0));
      out.seven_day_reset = epoch(u.seven_day.resets_at);
    }
    const ageH = (Date.now() - (cfg.cachedUsageUtilization?.fetchedAtMs || 0)) / 3600000;
    const rolled = out.five_hour_reset && out.five_hour_reset < Date.now() / 1000;
    out.stale = rolled || ageH > 3;
  } catch {}
  return withTokens(out);
}

async function pushUsage() {
  try {
    await stick('/claude/usage', { method: 'POST', body: readUsage() });
    state.usagePushes++;
  } catch (e) {
    // Expected while the stick is off or rebooting; not worth shouting about.
  }
}

// ------------------------------------------------------------------ api ----

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function askAndWait({ question, options, title = '', timeout = 300 }) {
  const id = `${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 7)}`;
  await stick('/claude/ask', {
    method: 'POST', body: { id, title, question, options, timeout },
  });
  state.asks++;

  const deadline = Date.now() + timeout * 1000 + 5000;
  let misses = 0;
  while (Date.now() < deadline) {
    await sleep(500);
    try {
      const r = await stick(`/claude/answer?id=${encodeURIComponent(id)}`);
      misses = 0;
      if (r.status === 'answered') return { status: 'answered', index: r.index, value: r.value };
      if (r.status !== 'pending') return { status: r.status };
    } catch {
      if (++misses > 20) return { status: 'unreachable' };
    }
  }
  return { status: 'timeout' };
}

function readBody(req) {
  return new Promise((resolve) => {
    let b = '';
    req.on('data', (c) => { b += c; if (b.length > 1e6) req.destroy(); });
    req.on('end', () => { try { resolve(JSON.parse(b || '{}')); } catch { resolve(null); } });
  });
}

const send = (res, code, obj) => {
  res.writeHead(code, { 'Content-Type': 'application/json' });
  res.end(JSON.stringify(obj));
};

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, 'http://127.0.0.1');
  try {
    if (req.method === 'POST' && url.pathname === '/ask') {
      const body = await readBody(req);
      if (!body?.question || !Array.isArray(body.options) || !body.options.length)
        return send(res, 400, { error: 'question and options[] required' });
      return send(res, 200, await askAndWait(body));
    }

    if (req.method === 'POST' && url.pathname === '/notify') {
      const body = await readBody(req);
      if (!body?.text) return send(res, 400, { error: 'text required' });
      await stick('/claude/notify', { method: 'POST', body });
      return send(res, 200, { status: 'ok' });
    }

    if (req.method === 'POST' && url.pathname === '/usage/refresh') {
      await pushUsage();
      return send(res, 200, { status: 'ok', usage: readUsage() });
    }

    if (url.pathname === '/status') {
      let device = null, error = null;
      try { device = await stick('/status'); } catch (e) { error = e.message; }
      return send(res, 200, {
        server: { port: PORT, uptime_s: Math.floor(process.uptime()), ...state },
        usage: readUsage(),
        device, error,
      });
    }

    send(res, 404, { error: 'not found' });
  } catch (e) {
    send(res, 502, { error: String(e.message || e) });
  }
});

server.listen(PORT, '127.0.0.1', () => {
  log(`skrzat-server on http://127.0.0.1:${PORT} -> ${HOST_NAME}`);
  pushUsage();
  setInterval(pushUsage, USAGE_EVERY_MS);
});

for (const sig of ['SIGINT', 'SIGTERM']) {
  process.on(sig, () => { saveState(); server.close(() => process.exit(0)); });
}
