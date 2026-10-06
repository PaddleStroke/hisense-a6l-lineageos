#!/bin/bash
# Send the hi846 fix to linux-media. Run in a WSL terminal:
#   bash /mnt/c/Users/Pierre/Desktop/A6L/tools/upstream/send-hi846.sh
# The Gmail app password is typed at a hidden prompt and never stored.
set -e
PATCH=/mnt/c/Users/Pierre/Desktop/A6L/docs/upstream/hi846-set-ctrl.patch
ME="Pierre-Louis Boyer <pierrelouis.boyer@gmail.com>"
if ! git send-email --help >/dev/null 2>&1; then
  echo "Installing git-email (sudo password needed)..."; sudo apt-get install -y git-email
fi
ARGS=(--from="$ME" --envelope-sender=pierrelouis.boyer@gmail.com
  --to="Martin Kepplinger-Novakovic <martink@posteo.de>"
  --to=linux-media@vger.kernel.org
  --cc="Sakari Ailus <sakari.ailus@linux.intel.com>"
  --cc="Mauro Carvalho Chehab <mchehab@kernel.org>"
  --suppress-cc=self --no-annotate --confirm=never
  --smtp-server=smtp.gmail.com --smtp-server-port=587 --smtp-encryption=tls
  --smtp-user=pierrelouis.boyer@gmail.com)
echo "=== Dry run (nothing is sent) ==="
git send-email "${ARGS[@]}" --dry-run --smtp-pass=x "$PATCH" 2>&1 | grep -E '^(From|To|Cc|Subject|Dry-OK|Result)' || true
echo
read -rp "Send this for real? Type YES: " ok; [ "$ok" = YES ] || { echo "Not sent."; exit 0; }
read -rsp "Gmail app password (16 letters, hidden): " P; echo
git send-email "${ARGS[@]}" --smtp-pass="$P" "$PATCH"; unset P
echo "Done. Track it at https://patchwork.linuxtv.org/project/linux-media/list/?q=hi846"
