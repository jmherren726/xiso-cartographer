#!/bin/sh
# Makes a throwaway self-signed certificate for serving HTTPS from this PC to the console over
# the LAN (docs/testing.md, "M5: TLS speed on the console"):
#
#   sh tools/testserver/make_lan_cert.sh <pc-ip> [out-dir]
#
# writes <out-dir>/lan-cert.pem and lan-key.pem, valid for 30 days, for the name <pc-ip> (as
# a DNS name too: the console's TLS matches the URL's host against DNS names only). Serve
# with rangeserver.py --tls-cert lan-cert.pem --tls-key lan-key.pem, and copy lan-cert.pem to
# the console as E:\Apps\xcat\cacert.pem: it then trusts exactly this certificate, directly,
# and nothing else, so delete it from the console after the test. Needs OpenSSL 1.1.1 or newer
# (Git for Windows ships one). Never commit the output.
set -eu
# Git Bash on Windows would rewrite "/CN=..." as a Windows path otherwise.
export MSYS_NO_PATHCONV=1
if [ $# -lt 1 ]; then
    echo "usage: $0 <pc-ip> [out-dir]" >&2
    exit 2
fi
ip=$1
out=${2:-.}
mkdir -p "$out"
openssl req -x509 -new -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes \
    -keyout "$out/lan-key.pem" -out "$out/lan-cert.pem" -days 30 -subj "/CN=$ip" \
    -addext "subjectAltName=DNS:$ip,IP:$ip" \
    -addext "basicConstraints=critical,CA:FALSE" \
    -addext "keyUsage=critical,digitalSignature" \
    -addext "extendedKeyUsage=serverAuth"
echo "wrote $out/lan-cert.pem and $out/lan-key.pem for $ip"
