#!/bin/sh
# Regenerates the TLS test certificates in this folder (needs OpenSSL 3.4 or newer for
# -not_before/-not_after). The output is committed so the tests are deterministic; rerun
# only to change what the tests need. TEST USE ONLY: these private keys are public, and
# nothing outside the tests may trust these CAs.
#
#   ca.pem          EC P-256 test CA, valid 2026-01-01 .. 2046-01-01
#   server.pem      EC P-256 leaf for test.example and localhost, 2026-01-01 .. 2036-01-01,
#                   signed by ca.pem; server.key.der is its key; server-chain.pem = leaf + CA
#   other-ca.pem    an unrelated EC CA, for "unknown issuer" tests
#   rsa-ca.pem      RSA-2048 test CA, same dates as ca.pem
#   rsa-server.pem  RSA-2048 leaf like server.pem, signed by rsa-ca.pem; rsa-server.key.der
set -eu
cd "$(dirname "$0")"

EC="-newkey ec -pkeyopt ec_paramgen_curve:P-256"
RSA="-newkey rsa:2048"

make_ca() { # file cn keyopts
    # shellcheck disable=SC2086
    openssl req -x509 -new $3 -nodes -keyout "$1.key" -out "$1.pem" -subj "/CN=$2" \
        -not_before 20260101000000Z -not_after 20460101000000Z \
        -addext "basicConstraints=critical,CA:TRUE" \
        -addext "keyUsage=critical,keyCertSign,cRLSign"
}

make_leaf() { # file ca keyopts
    # shellcheck disable=SC2086
    openssl req -new $3 -nodes -keyout "$1.key" -out "$1.csr" -subj "/CN=test.example"
    printf '%s\n' "basicConstraints=critical,CA:FALSE" \
        "keyUsage=critical,digitalSignature,keyEncipherment" \
        "extendedKeyUsage=serverAuth" \
        "subjectAltName=DNS:test.example,DNS:localhost" >"$1.ext"
    openssl x509 -req -in "$1.csr" -CA "$2.pem" -CAkey "$2.key" -out "$1.pem" \
        -not_before 20260101000000Z -not_after 20360101000000Z -set_serial 2 -extfile "$1.ext"
    rm "$1.csr" "$1.ext"
}

make_ca ca "xcat test CA" "$EC"
make_ca other-ca "xcat other test CA" "$EC"
make_ca rsa-ca "xcat RSA test CA" "$RSA"
make_leaf server ca "$EC"
make_leaf rsa-server rsa-ca "$RSA"
cat server.pem ca.pem >server-chain.pem
# Only the leaf keys are needed (the tests and the test server present the leaves). They're
# kept as binary DER (PKCS#8), which BearSSL reads directly, so secret scanners don't mistake
# these test keys for leaked ones.
rm ca.key other-ca.key rsa-ca.key
for k in server rsa-server; do
    openssl pkey -in "$k.key" -outform DER -out "$k.key.der"
    rm "$k.key"
done
