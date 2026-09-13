#!/usr/bin/env node
// skrzat — talk to the M5StickC Plus2 sitting on the desk.
//
//   skrzat ask "Deploy to prod?" "Yes" "No" "Later"
//   skrzat notify "Tests passed" --level done
//   skrzat status
//
// `ask` blocks until a button is pressed and prints the chosen option on
// stdout. Exit codes: 0 answered, 2 dismissed/timed out, 3 stick unreachable.

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HOST = process.env.SKRZAT_HOST || process.env.CLAUDE_STICK_HOST || 'skrzat.local';
const KEY = process.env.SKRZAT_KEY || process.env.CLAUDE_STICK_KEY || '';

function parseArgs(argv) {
  const positional = [];
  const flags = {};
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (!a.startsWith('--')) { positional.push(a); continue; }
    // A flag followed by another flag (or nothing) is a boolean switch,
    // so --json / --quiet work in any position.
    const next = argv[i + 1];
    if (next === undefined || next.startsWith('--')) flags[a.slice(2)] = true;
    else flags[a.slice(2)] = argv[++i];
  }
  return { positional, flags };
}

const { positional, flags } = parseArgs(process.argv.slice(2));
const cmd = positional.shift();
const host = flags.host || HOST;
const key = flags.key || KEY;
const base = host.startsWith('http') ? host : `http://${host}`;

function headers() {
  const h = { 'Content-Type': 'application/json' };
  if (key) h['X-Stick-Key'] = key;
  return h;
}

async function call(path, { method = 'GET', body, timeoutMs = 6000 } = {}) {
  const ac = new AbortController();
  const timer = setTimeout(() => ac.abort(), timeoutMs);
  try {
    const res = await fetch(base + path, {
      method,
      headers: headers(),
      body: body ? JSON.stringify(body) : undefined,
      signal: ac.signal,
    });
    const text = await res.text();
    let json;
    try { json = JSON.parse(text); } catch { json = { raw: text }; }
    if (!res.ok) throw new Error(json.error || `HTTP ${res.status}`);
    return json;
  } finally {
    clearTimeout(timer);
  }
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function die(code, msg) {
  process.stderr.write(msg + '\n');
  process.exit(code);
}

async function cmdAsk() {
  const question = positional.shift();
  const options = positional;
  if (!question || options.length === 0)
    die(64, 'usage: skrzat ask "<question>" "<option>" ["<option>" ...]');

  const id = `${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 7)}`;
  const timeout = Number(flags.timeout || 300);

  try {
    await call('/claude/ask', {
      method: 'POST',
      body: { id, title: flags.title || '', question, options, timeout },
    });
  } catch (e) {
    die(3, `Stick niedostepny (${base}): ${e.message}`);
  }

  if (!flags.quiet) process.stderr.write(`⟳ czekam na wybor na sticku (${options.length} opcji, ${timeout}s)...\n`);

  const deadline = Date.now() + timeout * 1000 + 5000;
  let misses = 0;
  while (Date.now() < deadline) {
    await sleep(500);
    let r;
    try {
      r = await call(`/claude/answer?id=${encodeURIComponent(id)}`);
      misses = 0;
    } catch {
      if (++misses > 20) die(3, 'Stick przestal odpowiadac');
      continue;
    }
    if (r.status === 'answered') {
      process.stdout.write(String(r.value) + '\n');
      process.exit(0);
    }
    if (r.status === 'dismissed') die(2, 'Odrzucone na sticku');
    if (r.status === 'timeout') die(2, 'Czas minal');
    if (r.status === 'unknown') die(2, 'Pytanie zastapione nowym');
  }
  die(2, 'Czas minal');
}

async function cmdNotify() {
  const text = positional.join(' ');
  if (!text) die(64, 'usage: skrzat notify "<text>" [--level info|done|error]');
  try {
    await call('/claude/notify', {
      method: 'POST',
      body: { text, level: flags.level || 'info', seconds: Number(flags.seconds || 6) },
    });
    if (!flags.quiet) process.stderr.write('✓ wyslano na stick\n');
  } catch (e) {
    die(3, `Stick niedostepny (${base}): ${e.message}`);
  }
}

async function cmdStatus() {
  try {
    const s = await call('/status');
    if (flags.json) {
      process.stdout.write(JSON.stringify(s, null, 2) + '\n');
      return;
    }
    const up = `${Math.floor(s.uptime_s / 3600)}h ${Math.floor((s.uptime_s % 3600) / 60)}m`;
    process.stdout.write(
      [
        `stick     ${s.device} @ ${s.ip}  (RSSI ${s.rssi} dBm)`,
        `firmware  v${s.fw ?? '?'}  (${s.build ?? '?'})`,
        `bateria   ${s.battery}%${s.charging ? ' (ladowanie)' : ''}`,
        `HA        ${s.ha_online ? `ok, ${s.entities} encji` : `BLAD: ${s.ha_error}`}`,
        `pytania   ${s.ask_status} (obsluzonych: ${s.asks})`,
        `uptime    ${up}`,
      ].join('\n') + '\n'
    );
  } catch (e) {
    die(3, `Stick niedostepny (${base}): ${e.message}`);
  }
}

async function cmdCancel() {
  try {
    await call('/claude/cancel', { method: 'POST' });
    process.stderr.write('✓ anulowano\n');
  } catch (e) {
    die(3, e.message);
  }
}

// Claude Code caches the account's limit utilisation in ~/.claude.json after
// it fetches it. We only read that cache - never the credentials - so the
// numbers are exactly as fresh as Claude Code last made them.
function readUsage() {
  const home = process.env.HOME;

  // Preferred source: what the status line fed us, which Claude Code updates
  // live. Falls back to the ~/.claude.json cache, which only moves when
  // Claude Code itself refetches (often hours or days old).
  try {
    const fed = JSON.parse(
      fs.readFileSync(path.join(home, '.claude', 'stick-usage.json'), 'utf8'));
    const ageH = (Date.now() - (fed.at || 0)) / 3600000;
    const nowS = Math.floor(Date.now() / 1000);
    const rolled = fed.five_hour_reset && fed.five_hour_reset < nowS;
    if (!rolled && ageH < 3) {
      fed.fetched_h_ago = Number(ageH.toFixed(2));
      fed.stale = false;
      try {
        const stats = JSON.parse(
          fs.readFileSync(path.join(home, '.claude', 'stats-cache.json'), 'utf8'));
        const today = new Date().toISOString().slice(0, 10);
        const row = (stats.dailyModelTokens || []).find((r) => r.date === today);
        if (row) fed.tokens_today =
          Object.values(row.tokensByModel || {}).reduce((a, b) => a + Number(b), 0);
      } catch { /* optional */ }
      return fed;
    }
  } catch { /* no feed yet */ }

  const out = {
    five_hour_used: -1, five_hour_reset: 0,
    seven_day_used: -1, seven_day_reset: 0,
    tokens_today: 0, stale: true, fetched_h_ago: null,
  };

  try {
    const cfg = JSON.parse(fs.readFileSync(path.join(home, '.claude.json'), 'utf8'));
    const cache = cfg.cachedUsageUtilization;
    if (cache?.utilization) {
      const epoch = (iso) => (iso ? Math.floor(Date.parse(iso) / 1000) : 0);
      const u = cache.utilization;
      if (u.five_hour) {
        out.five_hour_used = Math.round(Number(u.five_hour.utilization ?? 0));
        out.five_hour_reset = epoch(u.five_hour.resets_at);
      }
      if (u.seven_day) {
        out.seven_day_used = Math.round(Number(u.seven_day.utilization ?? 0));
        out.seven_day_reset = epoch(u.seven_day.resets_at);
      }
      const ageH = (Date.now() - (cache.fetchedAtMs || 0)) / 3600000;
      out.fetched_h_ago = Number(ageH.toFixed(1));
      const nowS = Math.floor(Date.now() / 1000);
      // A rolled-over window makes the cached percentage meaningless.
      const rolled = out.five_hour_reset && out.five_hour_reset < nowS;
      out.stale = rolled || ageH > 3;
    }
  } catch { /* no cache yet - stays stale */ }

  try {
    const stats = JSON.parse(
      fs.readFileSync(path.join(home, '.claude', 'stats-cache.json'), 'utf8'));
    const today = new Date().toISOString().slice(0, 10);
    const row = (stats.dailyModelTokens || []).find((r) => r.date === today);
    if (row) out.tokens_today =
      Object.values(row.tokensByModel || {}).reduce((a, b) => a + Number(b), 0);
  } catch { /* optional */ }

  return out;
}

async function cmdUsage() {
  const u = readUsage();

  if (flags.json) {
    process.stdout.write(JSON.stringify(u, null, 2) + '\n');
    return;
  }

  try {
    await call('/claude/usage', { method: 'POST', body: u });
  } catch (e) {
    die(3, `Stick niedostepny (${base}): ${e.message}`);
  }

  if (flags.quiet) return;
  if (u.stale) {
    const age = u.fetched_h_ago === null ? 'brak cache' : `${u.fetched_h_ago} h temu`;
    process.stderr.write(
      `! dane o limitach nieaktualne (${age}) - stick pokaze "nieaktualne".\n` +
      `  Odswiez je, otwierajac /usage w interaktywnej sesji Claude Code.\n`);
  } else {
    process.stderr.write(
      `✓ 5h: ${u.five_hour_used}% zuzyte, 7d: ${u.seven_day_used}% zuzyte\n`);
  }
}

async function cmdFlash() {
  const here = path.dirname(fileURLToPath(import.meta.url));
  const file = positional.shift() ||
    path.join(here, '..', '.pio', 'build', 'm5stick-c-plus2', 'firmware.bin');

  if (!fs.existsSync(file)) die(66, `Nie ma pliku: ${file}`);
  const bin = fs.readFileSync(file);
  if (bin.length < 100000) die(65, `Plik wyglada za maly (${bin.length} B) - to na pewno firmware.bin?`);

  const boundary = '----stick' + Math.random().toString(36).slice(2);
  const head = Buffer.from(
    `--${boundary}\r\nContent-Disposition: form-data; name="firmware"; ` +
    `filename="firmware.bin"\r\nContent-Type: application/octet-stream\r\n\r\n`);
  const tail = Buffer.from(`\r\n--${boundary}--\r\n`);
  const body = Buffer.concat([head, bin, tail]);

  process.stderr.write(`⟳ wysylam ${(bin.length / 1024).toFixed(0)} KB na ${base} ...\n`);

  const ac = new AbortController();
  const timer = setTimeout(() => ac.abort(), 180000);
  try {
    const headers = {
      'Content-Type': `multipart/form-data; boundary=${boundary}`,
      'X-Firmware-Size': String(bin.length),
    };
    if (key) headers['X-Stick-Key'] = key;
    const res = await fetch(base + '/update', { method: 'POST', headers, body, signal: ac.signal });
    const text = await res.text();
    if (!res.ok) die(1, `Stick odrzucil firmware: HTTP ${res.status} ${text}`);
    process.stderr.write('✓ wgrane, stick sie restartuje\n');
  } catch (e) {
    die(3, `Nie udalo sie wgrac przez Wi-Fi: ${e.message}`);
  } finally {
    clearTimeout(timer);
  }
}

const commands = { ask: cmdAsk, notify: cmdNotify, status: cmdStatus, cancel: cmdCancel,
                   usage: cmdUsage, flash: cmdFlash };

if (!commands[cmd]) {
  process.stdout.write(
    `skrzat — pilot M5StickC Plus2

  skrzat ask "<pytanie>" "<opcja>" ...   [--title T] [--timeout 300]
  skrzat notify "<tekst>"               [--level info|done|error] [--seconds 6]
  skrzat status                         [--json]
  skrzat cancel
  skrzat usage                          [--json]  # limity -> stick
  skrzat flash [firmware.bin]            # aktualizacja OTA przez Wi-Fi

Host: ${base}  (zmien przez --host lub SKRZAT_HOST)
`
  );
  process.exit(cmd ? 64 : 0);
}

commands[cmd]().catch((e) => die(1, e.stack || String(e)));
