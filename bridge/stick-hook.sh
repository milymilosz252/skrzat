#!/bin/bash
# Claude Code hook -> banner + beep on the stick.
# Best-effort by design: it must never fail or slow down a turn.
set -u

STICK="${SKRZAT_BIN:-$HOME/.local/bin/skrzat}"
TEXT="${1:-Claude Code}"
LEVEL="${2:-done}"

# Hooks inherit a minimal PATH, so find a node before running the CLI.
if ! command -v node >/dev/null 2>&1; then
  for d in "$HOME/Library/Application Support/Herd/config/nvm/versions/node"/*/bin \
           /opt/homebrew/bin /usr/local/bin; do
    if [ -x "$d/node" ]; then PATH="$d:$PATH"; break; fi
  done
  export PATH
fi

[ -x "$STICK" ] || exit 0
"$STICK" notify "$TEXT" --level "$LEVEL" --seconds 5 --quiet >/dev/null 2>&1 || true
# Refresh the limits panel while we are talking to the stick anyway.
"$STICK" usage --quiet >/dev/null 2>&1 || true
exit 0
