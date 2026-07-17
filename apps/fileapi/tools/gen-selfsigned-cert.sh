#!/usr/bin/env bash
# Generate a self-signed TLS certificate + key for local development of fileapi.
# Writes certs/server.crt and certs/server.key relative to the current directory.
# NOT for production — browsers/clients will warn (use curl -k / --insecure).
set -euo pipefail

CERT_DIR="${1:-certs}"
DAYS="${2:-365}"
mkdir -p "$CERT_DIR"

openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout "$CERT_DIR/server.key" \
    -out "$CERT_DIR/server.crt" \
    -days "$DAYS" \
    -subj "/CN=localhost" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"

chmod 600 "$CERT_DIR/server.key"
echo "Wrote $CERT_DIR/server.crt and $CERT_DIR/server.key (valid $DAYS days)."
