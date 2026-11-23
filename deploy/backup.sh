#!/bin/sh
# Heart Beacon v2 backup: online SQLite backup (the sqlite3 backup API, as `.backup` uses) plus /etc/hb and
# /etc/mosquitto-hb (keys included), one tarball, encrypted with age when installed and HB_AGE_RECIPIENT is set.
# Keeps the newest 14. Push the output dir off-box separately.
#   cron (root): 30 2 * * * HB_AGE_RECIPIENT=age1... /opt/hb/deploy/backup.sh /var/backups/hb
# Restore: age -d -i key.txt hb-<stamp>.tar.gz.age | tar -xz -C <dir>   (plain: tar -xzf hb-<stamp>.tar.gz -C <dir>)
set -eu
out=${1:-/var/backups/hb}
db=${HB_DB:-/var/lib/hb/hb.db}
conf=${HB_CONF:-/etc/hb /etc/mosquitto-hb}
stamp=$(date -u +%Y%m%dT%H%M%SZ)
umask 077
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$out" "$tmp/hb"

python3 -c 'import sqlite3, sys; sqlite3.connect(sys.argv[1]).backup(sqlite3.connect(sys.argv[2]))' "$db" "$tmp/hb/hb.db"
for d in $conf; do cp -a "$d" "$tmp/hb/"; done
tar -C "$tmp" -czf "$tmp/hb.tar.gz" hb

if command -v age >/dev/null 2>&1 && [ -n "${HB_AGE_RECIPIENT:-}" ]; then
  f="$out/hb-$stamp.tar.gz.age"
  age -r "$HB_AGE_RECIPIENT" -o "$f" "$tmp/hb.tar.gz"
else
  f="$out/hb-$stamp.tar.gz"
  echo "backup.sh: age or HB_AGE_RECIPIENT missing; $f is NOT encrypted and holds private keys" >&2
  mv "$tmp/hb.tar.gz" "$f"
fi
ls -1t "$out"/hb-*.tar.gz* | tail -n +15 | while read -r old; do rm -f -- "$old"; done
echo "$f"
