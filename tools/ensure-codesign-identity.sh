#!/bin/bash
#
# ensure-codesign-identity.sh
#
# Ensures a local, non-ad-hoc code-signing certificate ("RodeCasterVAD Local
# Dev") exists in this user's login keychain, creating it if missing. Idempotent
# -- safe to run on every build.
#
# WHY THIS EXISTS: plain ad-hoc signing (`codesign --sign -`) is not enough
# on recent macOS (confirmed on macOS 26.6.1) for a process to actually
# receive real-time CoreAudio HAL I/O data -- the process launches fine, the
# daemon's own log shows a completely healthy startup with IOProcs
# registered, but macOS's kernel-level code integrity check (AMFI) silently
# withholds the audio instead of raising an error:
#
#   amfid: rodevad-router not valid: Error Domain=AppleMobileFileIntegrityError
#   Code=-423 "The file is adhoc signed or signed by an unknown certificate chain"
#
# This produces the single most confusing failure mode in this whole
# project: a router daemon that looks perfectly healthy (no ERROR lines, all
# devices found, all IOProcs registered and started) yet every channel's
# level meter stays at exactly 0.000 forever, even with a test tone actively
# playing. Confirmed live: re-signing the daemon with a real (even
# self-signed, no paid Apple Developer ID needed) certificate instead of "-"
# resolves this immediately.
#
# This certificate is purely local and never leaves this machine -- it does
# not need to be trusted by macOS (no paid Developer ID / notarization
# required) since AMFI's specific complaint here is "no real certificate
# chain at all" (ad-hoc), not "chain isn't trusted", and codesign is happy
# to sign with an untrusted self-signed identity found by name in the
# keychain even when `security find-identity -p codesigning` reports 0
# "valid" (trust-chain-verified) identities.
#
# The FIRST time codesign actually uses the freshly-imported key, macOS may
# show a one-time "codesign wants to use a key in your keychain" dialog --
# click "Always Allow" so subsequent builds run non-interactively.

set -euo pipefail

IDENTITY_NAME="RodeCasterVAD Local Dev"
KEYCHAIN="$HOME/Library/Keychains/login.keychain-db"

if security find-certificate -c "$IDENTITY_NAME" "$KEYCHAIN" >/dev/null 2>&1; then
    exit 0
fi

echo "==> No local code-signing certificate found -- creating \"$IDENTITY_NAME\" in your login keychain (one-time, local-only, no Apple account needed)..."

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

cat > "$WORKDIR/codesign_ext.cnf" <<EOF
[req]
distinguished_name = dn
x509_extensions = v3_ext
prompt = no

[dn]
CN = $IDENTITY_NAME

[v3_ext]
basicConstraints = critical, CA:false
keyUsage = critical, digitalSignature
extendedKeyUsage = critical, codeSigning
EOF

openssl req -x509 -newkey rsa:2048 \
    -keyout "$WORKDIR/key.pem" -out "$WORKDIR/cert.pem" \
    -days 3650 -nodes -config "$WORKDIR/codesign_ext.cnf" >/dev/null 2>&1

# -legacy: modern OpenSSL 3.x defaults to AES-256 PKCS#12 encryption, which
# macOS's `security import` (still expecting the old RC2/3DES scheme) fails
# to parse ("MAC verification failed"). Falls back to explicit legacy PBE
# algorithms if this OpenSSL build has no legacy provider.
if ! openssl pkcs12 -export -legacy \
        -in "$WORKDIR/cert.pem" -inkey "$WORKDIR/key.pem" \
        -out "$WORKDIR/cert.p12" -passout pass:temp >/dev/null 2>&1; then
    openssl pkcs12 -export -certpbe PBE-SHA1-3DES -keypbe PBE-SHA1-3DES \
        -in "$WORKDIR/cert.pem" -inkey "$WORKDIR/key.pem" \
        -out "$WORKDIR/cert.p12" -passout pass:temp >/dev/null 2>&1
fi

security import "$WORKDIR/cert.p12" -k "$KEYCHAIN" -P temp -T /usr/bin/codesign -A

echo "==> Created. The next codesign step may show a one-time \"codesign wants"
echo "    to use a key in your keychain\" dialog -- click \"Always Allow\"."
