#!/usr/bin/env bash
# Removes what install.sh created; the shared Mosquitto (/etc/mosquitto, mosquitto.service) is never touched.
#   sudo bash deploy/uninstall.sh [--purge]            dry run: prints every action
#   sudo bash deploy/uninstall.sh --apply [--purge]    performs them
# Default keeps the data and keys ($VAR: DB; $ETC: TLS + config-signing keys) so a re-install carries on where it
# stopped. --purge removes those and the hb user too. The CA directory ($HB_PKI) is always left: it holds ca.key.
set -euo pipefail
. "$(dirname "${BASH_SOURCE[0]}")/install.sh"

purge=0
for a in "$@"; do
  case $a in --apply) APPLY=1 ;; --purge) purge=1 ;; *) die "usage: uninstall.sh [--apply] [--purge]" ;; esac
done
if [ "$APPLY" = 1 ] && [ "$(id -u)" != 0 ]; then die "--apply needs root"; fi
[ "$APPLY" = 1 ] || echo "# dry run: nothing is changed; add --apply to perform these"
run "systemctl disable --now hb-server hb-mosquitto 2>/dev/null || true"
run "rm -f $UNITS/hb-server.service $UNITS/hb-mosquitto.service && systemctl daemon-reload"
run "rm -rf $OPT $MQ $MQVAR"
if [ "$purge" = 1 ]; then
  run "rm -rf $ETC $VAR"
  if id hb >/dev/null 2>&1; then run "userdel hb"; fi
else
  echo "# kept: $ETC (keys, settings) and $VAR (DB); --purge removes them"
fi
echo "# left: $PKI (CA key) and the shared broker"
