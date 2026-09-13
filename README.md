# Skrzat

A *skrzat* is the house sprite of Polish folklore — a small helper that lives in
your home, keeps an eye on things, and occasionally asks for a bowl of milk.

This one lives on an **M5StickC Plus2** and does two jobs:

1. **A remote for Home Assistant** — every controllable entity, grouped by
   domain and room, with live state and per-domain actions.
2. **A physical answer panel for Claude Code** — when Claude needs a decision,
   the question appears on the stick's screen and you pick with a button.
   The choice travels back to the terminal.

```
┌──────────────────────────────── 240×135 ─┐
│ Claude Code            ▂▄▆ ●HA  60%  21:37│
│  ▄▄▄▄▄▄▄▄▄▄▄   ┌──────────────────────┐  │
│  █ ██   ██ █  ◄┤ 5h   8%       3h 40m │  │
│  █         █   │ ███░░░░░░░░░░░░░░░░░ │  │
│  ▀▀▀▀▀▀▀▀▀▀▀   │ 7d  32%     Wed 11:00│  │
│   ██  ██  ██   │ ███████░░░░░░░░░░░░░ │  │
│                └──────────────────────┘  │
│ M5:szczegoly              M5(dl):wstecz  │
└──────────────────────────────────────────┘
```

---

## Why it exists

Claude Code asks a lot of small questions — *this branch or that one, apply the
migration or not, which file did you mean*. Answering them means switching back
to the terminal and breaking whatever you were doing.

A dedicated screen on the desk turns that into a glance and a click. And since
the thing was already on Wi-Fi with a screen and three buttons, it may as well
run the house too.

---

## Hardware

| | |
|---|---|
| Board | M5StickC Plus2 (ESP32-PICO-V3-02, 8 MB flash, 2 MB PSRAM) |
| Display | 240×135 ST7789V2, used in landscape |
| Buttons | top → up · side → down · big M5 → select · M5 held → back |
| Extras | buzzer, RTC, IMU, ~200 mAh battery |

Nothing is soldered and no expansion hat is required.

---

## How it works

### Talking to Home Assistant

The obvious approach — walk `/api/states` — returns entity ids and states but
knows nothing about the **area registry**, so every light comes back without
the room it lives in.

Skrzat instead renders one Jinja template through `POST /api/template`:

```jinja
{% for s in states if s.domain in [...] %}
{{s.entity_id}}|{{s.state}}|{{area_name(s.entity_id)}}|{{s.name}}|{{s.attributes.brightness}}
{% endfor %}
```

One round trip returns every controllable entity **with its friendly name, its
area and its state**, as plain pipe-delimited lines that parse without a JSON
document in RAM. Entities are discovered dynamically — nothing is hardcoded, so
anything you add in Home Assistant shows up on the next refresh (every 6 s, and
immediately after any action).

Actions are chosen per domain: lights get brightness steps, media players get
transport and volume, automations get *trigger* alongside enable/disable,
covers get open/close/stop.

### Talking to Claude Code

The stick runs an HTTP server and announces itself as `skrzat.local`. Your Mac
**pushes** to it, which matters: nothing has to listen for incoming connections
on the laptop, so macOS never shows a firewall prompt.

```
skrzat ask "Which branch?" "main" "develop" "cancel"
   │
   ├─ POST /claude/ask   → screen wakes, buzzer chirps twice
   │                       (and re-chirps every 20 s until answered)
   ├─ GET  /claude/answer → polled until a button is pressed
   └─ prints the chosen option on stdout, exit 0
```

Exit codes: `0` answered, `2` dismissed or timed out, `3` stick unreachable —
so a script can fall back to asking in the terminal when the stick is off.

### The usage bars

The limit bars mirror what `/usage` shows in Claude Code. Getting live numbers
without touching credentials takes a small detour.

Claude Code caches utilisation in `~/.claude.json`, but only refreshes it when
it happens to fetch — that cache is routinely **hours or days old**. The live
source is the **status line**: Claude Code hands status-line scripts a JSON
payload containing `rate_limits` with `used_percentage` and `resets_at`.

So Skrzat installs a status-line *wrapper*: it captures that payload, forwards
it to the stick, and then runs your own status-line script with the same input
and prints its output unchanged. Your status line keeps working; Skrzat just
listens in.

**No credentials are read and the Anthropic API is never called.** If the feed
goes quiet, the stick says *"Limity nieaktualne"* rather than showing a stale
number as if it were current.

The countdown to reset is computed on the device from its NTP clock — only the
reset timestamp is transmitted.

### Updates over Wi-Fi

After the first USB flash, everything else goes over the air:

```bash
pio run && skrzat flash
```

The stick shows a progress bar and reboots itself. `upload_speed` is pinned to
460800 because 1.5 Mbaud corrupts the stream on many cables.

---

## Quick start

```bash
git clone https://github.com/<you>/skrzat.git && cd skrzat
pio run -t upload --upload-port /dev/cu.usbserial-XXXXXXXX
./install.sh --server
```

On first boot the stick opens a Wi-Fi access point called **`Skrzat-Setup`**.
Join it from your phone, open `http://192.168.4.1`, and fill in:

* your Wi-Fi network,
* the Home Assistant URL — **with scheme and port**, e.g. `http://homeassistant.local:8123`,
* a Home Assistant **long-lived access token** (Profile → Security).

The token goes straight from your phone into the device's NVS. It is never
written to a file on your computer and never leaves your network.

If Home Assistant advertises itself over mDNS (`_home-assistant._tcp`), Skrzat
finds the address on its own — on first boot when the field is empty, or on
demand from **Settings → Find Home Assistant**. Only the token has to be typed.

To reconfigure later: hold the side button while powering on, or use
**Settings → Change Wi-Fi**.

### Settings menu

| Entry | What it does |
|---|---|
| Refresh from HA | re-reads every entity now |
| Brightness / Sleep / Sound / Volume | display and buzzer |
| Power saving | `off` / `norm` / `max`, see below |
| Language | **PL / EN**, switched live — every string goes through one table in `src/i18n.cpp` |
| Swap up/down · Rotate screen | button and orientation fixes for however you hold it |
| Find Home Assistant | mDNS discovery, fills the URL in |
| Wi-Fi networks | live scan with RSSI, your own network marked `*` — for diagnosing a weak link |
| Change Wi-Fi | reopens the provisioning portal |
| Factory reset | erases the whole NVS partition **including Wi-Fi credentials**, asks first |
| Reboot | restart |

---

## The Mac side

### CLI

```bash
skrzat ask "Deploy to prod?" "yes" "no" --title "release" --timeout 300
skrzat notify "Tests passed" --level done
skrzat status
skrzat usage          # push current limits to the device
skrzat flash          # OTA firmware update
skrzat cancel
```

### Skill

`install.sh` links a Claude Code skill so Claude reaches for the stick on its
own when a decision has 2–8 discrete options — and keeps asking in the terminal
for anything open-ended, since the stick has no keyboard.

### Hooks

Two hooks are suggested (you add them to `~/.claude/settings.json` yourself):

| Event | Effect |
|---|---|
| `Stop` | banner + chirp when a turn finishes, and a usage refresh |
| `Notification` | banner when Claude is waiting on you |

`bridge/stick-hook.sh` is deliberately best-effort: it finds `node` on its own
and exits `0` even when the stick is unreachable, so it can never block or slow
down a turn.

### Server

`install.sh --server` registers a launchd agent that runs a small daemon on
`127.0.0.1:8787`. It is not required — the CLI works without it — but it solves
three things the CLI cannot:

* **Freshness.** Limits refresh on a timer instead of only when a status line
  happens to render.
* **Addressing.** mDNS occasionally fails to resolve. The daemon remembers the
  last IP that worked and falls back to it, so a hiccup doesn't become a failed
  question.
* **One door.** Anything on the machine can reach the stick over localhost
  without knowing how it is addressed:

```bash
curl -s localhost:8787/ask -d '{"question":"Ship it?","options":["yes","no"]}'
curl -s localhost:8787/status | jq .usage
```

It binds to loopback only. Logs land in `~/Library/Logs/skrzat-server.log`.

---

## Device HTTP API

| Method | Path | Purpose |
|---|---|---|
| `GET` | `/status` | IP, RSSI, battery, HA state, idle timer, sleep stage |
| `POST` | `/claude/ask` | `{id,title,question,options[],timeout}` — max 8 options |
| `GET` | `/claude/answer?id=` | `pending` / `answered` / `dismissed` / `timeout` |
| `POST` | `/claude/cancel` | withdraw the pending question |
| `POST` | `/claude/notify` | `{text,level,seconds}` |
| `POST` | `/claude/usage` | limit windows to display |
| `POST` | `/update` | OTA: multipart, field `firmware` |

---

## Power management

**Ustawienia → Oszczedzanie** has three modes:

| Mode | Behaviour |
|---|---|
| `wyl` | screen always on, CPU at 240 MHz |
| `norm` | dim at half the timeout, then `SLPIN` to the panel controller; CPU drops to 80 MHz |
| `max` | half the timeout, backlight capped, CPU pinned at 80 MHz, HA polled every 15 s |

Below 15 % battery on no charger the timeout halves again. The radio runs in
DTIM modem sleep, and while asleep the main loop draws nothing.

**Sleep never applies to a pending question** — once one arrives the screen
stays lit until you answer.

Measured: with the panel asleep and the CPU at 80 MHz, an HTTP request is
served in **0.11 s**. The link to Claude Code is not affected by sleep.

A watchdog checks the connection every 5 s (including while asleep), calls
`WiFi.reconnect()`, re-announces mDNS after recovery, and reboots as a last
resort if the network is gone for three minutes.

---

## Security

Worth being explicit, because some of this is on you:

* **The `/claude/*` and `/update` endpoints are open on your LAN by default.**
  `/update` accepts firmware, which means arbitrary code execution on the
  device. Set `stickKey` in NVS and pass it via `SKRZAT_KEY` if your network
  isn't trusted.
* **The Home Assistant token never leaves the device.** It is entered on the
  provisioning portal and stored in NVS; no host-side tool reads it.
* **Nothing reads your Claude credentials.** Usage data comes only from what
  Claude Code already writes locally and hands to status-line scripts.

---

## Layout

| Path | Role |
|---|---|
| `src/main.cpp` | screen state machine, buttons, boot, watchdog |
| `src/ui.cpp` | rendering into a 240×135 PSRAM sprite, power stages |
| `src/ha_client.cpp` | Home Assistant REST, entity model, per-domain actions |
| `src/claude_link.cpp` | device HTTP server, mDNS, OTA |
| `src/settings.cpp` | NVS-backed settings |
| `bridge/skrzat.mjs` | the CLI |
| `bridge/stick-statusline.sh` | status-line wrapper that feeds live limits |
| `bridge/stick-hook.sh` | Claude Code hook adapter |
| `server/skrzat-server.mjs` | localhost daemon |
| `skill/skrzat/SKILL.md` | Claude Code skill |

---

## Known limitations

* **The UI is ASCII-folded Polish.** M5GFX's built-in fonts are Latin-1 only,
  so `Światła` renders as `Swiatla`. Swapping in a U8g2 Latin Extended-A font
  was tried and reverted — those glyphs are taller than `Font2` and the rows
  overlapped. Doing it properly means recomputing `ROW_H` and the line stepping,
  not just changing a `#define`.
* **Battery life is hours, not days.** ~200 mAh with Wi-Fi associated. Fine on
  a desk with USB; not a pocket device in this configuration.
* **BLE is not used.** Wi-Fi and BLE coexist poorly on one ESP32 radio and the
  binary is already 1.33 MB.

---

## Licence

MIT.
