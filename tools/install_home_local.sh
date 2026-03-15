#!/bin/sh

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)

exec "$SCRIPT_DIR/install_local.sh" \
    --prefix "${HOME}/.local" \
    --config-root "${HOME}/.config" \
    "$@"
