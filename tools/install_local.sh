#!/bin/sh

set -eu

usage() {
    cat <<'EOF'
Usage: tools/install_local.sh [options]

Build medit from this checkout and install it into a user-local prefix.

Options:
  --prefix DIR             Install binaries into DIR/bin and support files into DIR/share/medit
                           Default: ~/.local
  --config-root DIR        Install runtime config into DIR/meditrc and DIR/medit/
                           Default: ~/.config
  --bootstrap-tree-sitter  Build tree-sitter runtime assets into the target config tree
  --overwrite-config       Replace existing config files instead of only filling missing ones
  --skip-control-cli       Do not install the medit-ctl helper script
  --help                   Show this help text
EOF
}

log() {
    printf '%s\n' "$*"
}

warn() {
    printf 'warning: %s\n' "$*" >&2
}

copy_file_if_needed() {
    src=$1
    dst=$2
    if [ -e "$dst" ] && [ "$OVERWRITE_CONFIG" -ne 1 ]; then
        log "keeping existing $dst"
        return
    fi
    install -d "$(dirname "$dst")"
    install -m644 "$src" "$dst"
    log "installed $dst"
}

copy_tree_if_needed() {
    src_root=$1
    dst_root=$2
    find "$src_root" -type f | while IFS= read -r src; do
        rel=${src#"$src_root"/}
        copy_file_if_needed "$src" "$dst_root/$rel"
    done
}

PREFIX=${HOME}/.local
CONFIG_ROOT=${HOME}/.config
BOOTSTRAP_TREE_SITTER=0
OVERWRITE_CONFIG=0
INSTALL_CONTROL_CLI=1

while [ "$#" -gt 0 ]; do
    case "$1" in
        --prefix)
            PREFIX=$2
            shift 2
            ;;
        --config-root)
            CONFIG_ROOT=$2
            shift 2
            ;;
        --bootstrap-tree-sitter)
            BOOTSTRAP_TREE_SITTER=1
            shift
            ;;
        --overwrite-config)
            OVERWRITE_CONFIG=1
            shift
            ;;
        --skip-control-cli)
            INSTALL_CONTROL_CLI=0
            shift
            ;;
        --help)
            usage
            exit 0
            ;;
        *)
            usage >&2
            exit 1
            ;;
    esac
done

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
BIN_DIR=$PREFIX/bin
SHARE_DIR=$PREFIX/share/medit
TARGET_CONFIG_DIR=$CONFIG_ROOT/medit

if ! command -v make >/dev/null 2>&1; then
    warn "make is required"
    exit 1
fi
if ! command -v install >/dev/null 2>&1; then
    warn "install is required"
    exit 1
fi

log "building medit in $REPO_ROOT"
make -C "$REPO_ROOT" medit

install -d "$BIN_DIR" "$SHARE_DIR"
install -m755 "$REPO_ROOT/medit" "$BIN_DIR/medit"
log "installed $BIN_DIR/medit"

copy_file_if_needed "$REPO_ROOT/config/meditrc" "$CONFIG_ROOT/meditrc"
copy_tree_if_needed "$REPO_ROOT/config/medit" "$TARGET_CONFIG_DIR"

if [ "$INSTALL_CONTROL_CLI" -eq 1 ]; then
    install -m755 "$REPO_ROOT/tools/medit_ctl.py" "$BIN_DIR/medit-ctl"
    log "installed $BIN_DIR/medit-ctl"
fi

install -m644 "$REPO_ROOT/tools/tree_sitter_languages.json" "$SHARE_DIR/tree_sitter_languages.json"
log "installed $SHARE_DIR/tree_sitter_languages.json"

if [ "$BOOTSTRAP_TREE_SITTER" -eq 1 ]; then
    log "bootstrapping tree-sitter runtime into $TARGET_CONFIG_DIR"
    make -C "$REPO_ROOT" PYTHON="${PYTHON:-python3}" bootstrap-tree-sitter CONFIG_ROOT="$CONFIG_ROOT"
fi

case ":${PATH:-}:" in
    *:"$BIN_DIR":*)
        ;;
    *)
        warn "$BIN_DIR is not on PATH"
        ;;
esac

log
log "local install complete"
log "binary:   $BIN_DIR/medit"
if [ "$INSTALL_CONTROL_CLI" -eq 1 ]; then
    log "control:  $BIN_DIR/medit-ctl"
fi
log "config:   $CONFIG_ROOT/meditrc"
log "assets:   $TARGET_CONFIG_DIR"
