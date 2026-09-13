#!/usr/bin/env bash
# Installs the skrzat CLI, the Claude Code skill, and (optionally) the
# background server as a launchd agent.
set -euo pipefail

PROJECT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN_DIR="$HOME/.local/bin"
SKILL_DIR="$HOME/.claude/skills"

mkdir -p "$BIN_DIR" "$SKILL_DIR"

chmod +x "$PROJECT/bridge/skrzat.mjs" "$PROJECT/bridge/stick-usage-feed.mjs" \
         "$PROJECT/bridge/stick-hook.sh" "$PROJECT/bridge/stick-statusline.sh" \
         "$PROJECT/server/skrzat-server.mjs"

ln -sfn "$PROJECT/bridge/skrzat.mjs" "$BIN_DIR/skrzat"
echo "✓ CLI    -> $BIN_DIR/skrzat"

ln -sfn "$PROJECT/skill/skrzat" "$SKILL_DIR/skrzat"
echo "✓ skill  -> $SKILL_DIR/skrzat"

if ! command -v skrzat >/dev/null 2>&1; then
  echo "! $BIN_DIR is not on PATH - add:  export PATH=\"\$HOME/.local/bin:\$PATH\""
fi

# --- optional: run the server at login ------------------------------------
if [ "${1:-}" = "--server" ]; then
  NODE="$(command -v node || true)"
  if [ -z "$NODE" ]; then
    echo "! node not found on PATH; skipping the launchd agent"
    exit 0
  fi
  AGENT="$HOME/Library/LaunchAgents/com.skrzat.server.plist"
  mkdir -p "$(dirname "$AGENT")"
  sed -e "s|__NODE__|$NODE|g" -e "s|__PROJECT__|$PROJECT|g" -e "s|__HOME__|$HOME|g" \
      "$PROJECT/server/com.skrzat.server.plist" > "$AGENT"
  launchctl unload "$AGENT" 2>/dev/null || true
  launchctl load "$AGENT"
  echo "✓ server -> launchd agent com.skrzat.server (log: ~/Library/Logs/skrzat-server.log)"
fi

echo
echo "Next:  skrzat status"
