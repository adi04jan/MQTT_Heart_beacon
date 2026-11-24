# Heart Beacon v2 server: runbook

Target: the home server (Ubuntu 20.04). v2 runs its own broker `hb-mosquitto` (TLS 8883, router on loopback 1884)
and `hb-server` (API 8443, admin 127.0.0.1:8444). The shared Mosquitto on 1883 (water tank, Frigate, logger, the
v1 beacons and the Telegram bridge) is never touched: nothing under `/etc/mosquitto/`, no `mosquitto.service`.

Placeholders: `<IP>` the static public IP, `<server>` your ssh host for the server, `<LAN-IP>` its LAN address.
Server commands are bash on the server; dev PC commands are PowerShell in the repo checkout.

## 1. Router and firewall

- Port forwards, TCP only: `8883 -> <LAN-IP>:8883` and `8443 -> <LAN-IP>:8443`. Nothing else (8444 and 1884 stay
  on loopback).
- If `sudo ufw status` says active: `sudo ufw allow 8883/tcp && sudo ufw allow 8443/tcp`.

## 2. Install

```bash
git clone <repo-url> ~/hb-src && git -C ~/hb-src checkout <branch>
sudo bash ~/hb-src/deploy/install.sh <IP>            # dry run: read the plan, nothing changes
sudo bash ~/hb-src/deploy/install.sh --apply <IP>    # ends with "# installed; listening on 8883 8443 8444 1884"
systemctl status mosquitto --no-pager | head -3      # shared broker: still active, same "since" as before
```

It refuses (exit 1, `REFUSED: ...`) without the IP, if 8883/8443/8444/1884 are taken, or if any step would touch
the shared broker. Re-running it is safe: it refreshes code, configs and units and keeps the DB, keys and CA.
Stopping or restarting `hb-mosquitto` takes about 10 s (Ubuntu's 1.6.9 is built with libdlt); that is not a hang.

## 3. Keys off the server

`install.sh` made the CA in `/root/hb-pki`. Its `ca.key` belongs on the dev PC only; `ca.crt` and `cfg-sign.pub`
are what the firmware and the app pin.

```bash
sudo tar -C /root -czf /tmp/hb-pki.tgz hb-pki && sudo chown "$USER" /tmp/hb-pki.tgz && sudo cp /etc/hb/cfg-sign.pub /tmp/
```
```powershell
mkdir -Force $HOME\hb-pki-backup | Out-Null
scp <server>:/tmp/hb-pki.tgz <server>:/tmp/cfg-sign.pub $HOME\hb-pki-backup\
tar -xzf $HOME\hb-pki-backup\hb-pki.tgz -C $HOME\hb-pki-backup      # hb-pki\ca.key stays offline, here only
```
```bash
rm /tmp/hb-pki.tgz /tmp/cfg-sign.pub && sudo shred -u /root/hb-pki/ca.key
```

The leaf is valid for 825 days. To renew it or move to a new IP, copy `ca.key` back into `/root/hb-pki/` and
re-run `install.sh --apply <IP>`: it re-issues the leaf when it does not name the IP or expires within 30 days.
The config-signing key `/etc/hb/cfg-sign.key` is never replaced.

## 4. Users and beacons

```bash
sudo /opt/hb/deploy/hb-admin adduser <uid1> "<Name 1>"      # prints the app password once: give it to that person
sudo /opt/hb/deploy/hb-admin adduser <uid2> "<Name 2>"
sudo /opt/hb/deploy/hb-admin register HB-XXXXXX --assign <uid1>
sudo /opt/hb/deploy/hb-admin register HB-YYYYYY --assign <uid2>
```

`uid` is `[a-z][a-z0-9_]{0,31}`; the `did` is the beacon's BLE name. Only the assigned user can claim a beacon from
the app. The running server picks these changes up within a second.

## 5. Admin page

```powershell
ssh -N -L 8444:127.0.0.1:8444 <server>      # then open http://127.0.0.1:8444
```

## 6. Smoke test

Once, on the server: two test users in their own circle `smoke` (real users never see them) and a test beacon.
Keep the two printed passwords for later runs.

```bash
sudo /opt/hb/deploy/hb-admin adduser smoke_a "Smoke A" --circle smoke
sudo /opt/hb/deploy/hb-admin adduser smoke_b "Smoke B" --circle smoke
sudo /opt/hb/deploy/hb-admin register HB-000001 --assign smoke_b
```

From the dev PC, preferably outside the home LAN (phone hotspot): many routers don't loop the public IP back
inside the LAN.

```powershell
$env:HB_SMOKE_A_PASS = '<smoke_a password>'; $env:HB_SMOKE_B_PASS = '<smoke_b password>'
server\.venv\Scripts\python.exe deploy\smoke.py <IP> --ca $HOME\hb-pki-backup\hb-pki\ca.crt
```

Pass = 15 `PASS` lines and `SMOKE PASS`, exit code 0. It checks TLS on 8883 and 8443 (pinned CA works; the system
trust store and a wrong IP are refused), API login, claim, heart -> `show` on the beacon topic -> `shown` -> `rcpt`,
and two hearts sent while the beacon is offline arriving as one `show` on reconnect.

## 7. Backup and restore

`age` is not packaged for Ubuntu 20.04: put its static binary in `/usr/local/bin` to encrypt, or the tarballs stay
plain (they hold the private keys). Nightly at 02:30, the newest 14 kept:

```bash
echo '30 2 * * * root HB_AGE_RECIPIENT=<age1...> sh /opt/hb/deploy/backup.sh /var/backups/hb' | sudo tee /etc/cron.d/hb-backup
sudo sh /opt/hb/deploy/backup.sh /var/backups/hb                  # run once now; copy the files off-box
```

Restore (same server, or a fresh one after step 2):

```bash
sudo systemctl stop hb-server hb-mosquitto
d=$(mktemp -d) && sudo tar -xzf hb-<stamp>.tar.gz -C "$d"      # .age: age -d -i key.txt hb-<stamp>.tar.gz.age | sudo tar -xz -C "$d"
sudo install -o hb -g hb -m 600 "$d/hb/hb.db" /var/lib/hb/hb.db && sudo rm -f /var/lib/hb/hb.db-wal /var/lib/hb/hb.db-shm
sudo cp -a "$d/hb/hb/." /etc/hb/ && sudo cp -a "$d/hb/mosquitto-hb/." /etc/mosquitto-hb/
sudo systemctl start hb-mosquitto hb-server && sudo rm -rf "$d"
```

The server rewrites the broker's password file from the DB on start. Then run step 6.

## 8. Rollback

- Bad update: `git -C ~/hb-src checkout <previous-commit> && sudo bash ~/hb-src/deploy/install.sh --apply <IP>`
  (keeps DB and keys).
- Remove v2: `sudo bash ~/hb-src/deploy/uninstall.sh` (dry run), then `--apply`. That keeps `/etc/hb` and
  `/var/lib/hb` for a later re-install; `--apply --purge` removes them and the `hb` user too. `/root/hb-pki` and
  the shared broker are always left. Drop the two port forwards.
- v1 (the current beacons and the Telegram bridge) runs on the shared broker the whole time and is not affected.

## 9. Health

```bash
systemctl status hb-mosquitto hb-server --no-pager
journalctl -u hb-server -u hb-mosquitto -f                  # broker log: who connected, why they dropped
sudo ss -ltnp | grep -E ':(8883|8443|8444|1884) '
sudo openssl x509 -in /etc/hb/leaf.crt -noout -enddate
```

Secrets live in `/etc/hb` (TLS and config-signing keys), `/var/lib/hb/router.pass` and `/var/lib/hb/hb.db`; all
are owned by `hb`, mode 600 or in 700 directories.

## Local proof without the server

`wsl -d Ubuntu-20.04 -e bash deploy/e2e-local.sh` runs the shipped broker config on WSL's own Mosquitto 1.6.9
(beside its 1883 broker) plus hb-server and `smoke.py` against 127.0.0.1, after the server tests. No root; it
cleans up after itself.
