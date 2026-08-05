#!/usr/bin/env sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
IMAGE_NAME=${IMAGE_NAME:-hik-sdk-http-bridge-linux:1.0.0}
OUTPUT=${OUTPUT:-"$ROOT/dist/hik-sdk-http-bridge-linux-amd64.oci.tar.gz"}

command -v docker >/dev/null 2>&1 || { echo "Docker Engine is required to build the portable OCI archive." >&2; exit 1; }
mkdir -p "$(dirname -- "$OUTPUT")"
docker build --pull -t "$IMAGE_NAME" "$ROOT"
docker save "$IMAGE_NAME" | gzip -9 > "$OUTPUT"
sha256sum "$OUTPUT" > "$OUTPUT.sha256"
printf 'created: %s\nsha256: %s\n' "$OUTPUT" "$OUTPUT.sha256"
