#!/usr/bin/env bash
# File-based command relay so Claude (which can only see the A6L folder) can run
# commands inside this WSL distro. Run it INSIDE WSL as the build user:
#   bash /mnt/c/Users/Pierre/Desktop/A6L/tools/claude-wsl-relay.sh
# Stop with Ctrl+C. It only executes scripts dropped in A6L/.relay/inbox.
set -u
R="$(cd "$(dirname "$0")/.." && pwd)/.relay"
mkdir -p "$R/inbox" "$R/outbox" "$R/done"
echo "relay up as $(whoami) on $(hostname); watching $R/inbox"
while true; do
  date +%s > "$R/heartbeat"
  for f in "$R"/inbox/*.sh; do
    [ -e "$f" ] || continue
    n="$(basename "$f" .sh)"
    mv "$f" "$R/done/$n.sh"
    echo "[$(date +%T)] run $n"
    ( cd "$HOME" && timeout "${RELAY_TIMEOUT:-3600}" bash "$R/done/$n.sh" ) > "$R/outbox/$n.out.tmp" 2>&1
    echo $? > "$R/outbox/$n.rc"
    mv "$R/outbox/$n.out.tmp" "$R/outbox/$n.out"
  done
  sleep 2
done
