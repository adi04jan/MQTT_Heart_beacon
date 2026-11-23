#!/bin/sh
# Private PKI for Heart Beacon v2. Run on the dev machine, never on the server.
#   mkca.sh <public-ip> [out-dir]      e.g. mkca.sh 203.0.113.10 ~/hb-pki
# Makes (EC P-256): ca.key + ca.crt (root, 20 y, made once and reused), leaf.key + leaf.crt (server, IP SAN,
# 825 d, re-issued on every run, e.g. after an IP change), cfg-sign.key + cfg-sign.pub (config signing, made once).
# Copy to the server's /etc/hb (owner hb, mode 600 for keys): ca.crt leaf.crt leaf.key cfg-sign.key.
# ca.crt and cfg-sign.pub are pinned in the firmware and the app. ca.key stays offline.
set -eu
export MSYS_NO_PATHCONV=1 # Git Bash: keep "/CN=..." as is
ip=${1:?usage: mkca.sh <public-ip> [out-dir]}
out=${2:-./hb-pki}
umask 077
mkdir -p "$out"
cd "$out"

if [ ! -f ca.key ]; then
  openssl ecparam -name prime256v1 -genkey -noout -out ca.key
  openssl req -x509 -new -key ca.key -sha256 -days 7300 -subj "/CN=Heart Beacon Root CA" \
    -addext "basicConstraints=critical,CA:TRUE,pathlen:0" -addext "keyUsage=critical,keyCertSign,cRLSign" \
    -out ca.crt
fi

openssl ecparam -name prime256v1 -genkey -noout -out leaf.key
openssl req -new -key leaf.key -subj "/CN=$ip" -out leaf.csr
printf 'basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\nsubjectAltName=IP:%s\n' \
  "$ip" > leaf.ext
openssl x509 -req -in leaf.csr -CA ca.crt -CAkey ca.key -CAcreateserial -days 825 -sha256 -extfile leaf.ext \
  -out leaf.crt
rm -f leaf.csr leaf.ext

[ -f cfg-sign.key ] || openssl ecparam -name prime256v1 -genkey -noout -out cfg-sign.key
openssl ec -in cfg-sign.key -pubout -out cfg-sign.pub 2>/dev/null
chmod 644 ca.crt leaf.crt cfg-sign.pub

openssl verify -CAfile ca.crt leaf.crt
openssl x509 -in leaf.crt -noout -ext subjectAltName
