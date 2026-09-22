#!/usr/bin/env sh
set -eu

ARCHIVE=${1:?usage: run.sh /path/to/hik-sdk-http-bridge-linux-amd64.oci.tar.gz [host_port]}
HOST_PORT=${2:-28080}
IMAGE_NAME=${IMAGE_NAME:-hik-sdk-http-bridge-linux:1.0.0}

docker load -i "$ARCHIVE"
exec docker run --rm --init --name hik-sdk-http-bridge \
  -p "${HOST_PORT}:28080" \
  "$IMAGE_NAME"
