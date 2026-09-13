#!/bin/bash
# Wraps the user's own status line: forwards the rate-limit payload to the
# stick in the background, then runs the original and prints it unchanged.
INPUT=$(cat)

# Your own status line, if you had one before Skrzat. Set it in
# ~/.config/skrzat/statusline or via SKRZAT_INNER_STATUSLINE.
ORIGINAL="${SKRZAT_INNER_STATUSLINE:-}"
CONF="$HOME/.config/skrzat/statusline"
[ -z "$ORIGINAL" ] && [ -f "$CONF" ] && ORIGINAL="$(cat "$CONF")"
FEED="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/stick-usage-feed.mjs"

if ! command -v node >/dev/null 2>&1; then
  for d in "$HOME/Library/Application Support/Herd/config/nvm/versions/node"/*/bin \
           /opt/homebrew/bin /usr/local/bin; do
    if [ -x "$d/node" ]; then PATH="$d:$PATH"; break; fi
  done
  export PATH
fi

[ -f "$FEED" ] && printf '%s' "$INPUT" | node "$FEED" >/dev/null 2>&1 &

if [ -n "$ORIGINAL" ] && [ -f "$ORIGINAL" ]; then
  printf '%s' "$INPUT" | node "$ORIGINAL"
fi
