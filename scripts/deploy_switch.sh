#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MTP_DIR="${MTP_DIR:-}"
NRO_SRC="${NRO_SRC:-${ROOT}/build-switch/pipensx.nro}"
DEPLOY_CLEAN="${DEPLOY_CLEAN:-0}"

if [[ -z "$MTP_DIR" ]]; then
    echo "Set MTP_DIR to the target directory, for example:" >&2
    echo "  make deploy MTP_DIR='mtp://DEVICE/1: SD Card/switch/pipensx'" >&2
    exit 2
fi
if [[ "$DEPLOY_CLEAN" != "0" && "$DEPLOY_CLEAN" != "1" ]]; then
    echo "DEPLOY_CLEAN must be 0 or 1." >&2
    exit 2
fi

# Cached artwork lives here (see GameMetadataService::imageRoot_); keep it
# across deploys so covers/screenshots don't have to re-download every time.
KEEP_RELPATHS=("catalog/images")

if [[ ! -f "$NRO_SRC" ]]; then
    echo "ERROR: $NRO_SRC not found" >&2
    exit 1
fi

is_kept_path() {
    local relpath="$1"
    local keep
    for keep in "${KEEP_RELPATHS[@]}"; do
        [[ "$relpath" == "$keep" ]] && return 0
    done
    return 1
}

# Removes everything under $dir except kept paths. Returns 1 (and leaves
# $dir itself in place) if anything inside was preserved, so a parent dir
# left non-empty knows not to remove itself either.
gio_rm_recursive() {
    local dir="$1"
    local relprefix="$2"
    local anything_kept=0
    while IFS= read -r item; do
        [[ -z "$item" ]] && continue
        local full="$dir/$item"
        local rel="${relprefix:+$relprefix/}$item"
        if is_kept_path "$rel"; then
            echo "  keep: $full"
            anything_kept=1
            continue
        fi
        if gio info "$full" 2>/dev/null | grep -q "type: directory"; then
            if ! gio_rm_recursive "$full" "$rel"; then
                echo "  keep: $full (non-empty)"
                anything_kept=1
                continue
            fi
        fi
        echo "  rm: $full"
        gio remove "$full"
    done < <(gio list "$dir")
    [[ "$anything_kept" == 0 ]]
}

if [[ "$DEPLOY_CLEAN" == "1" ]]; then
    echo "==> Cleaning $MTP_DIR (keeping: ${KEEP_RELPATHS[*]})"
    gio_rm_recursive "$MTP_DIR" "" || true
else
    echo "==> Replacing only pipensx.nro (set DEPLOY_CLEAN=1 for a clean deploy)"
    gio remove "$MTP_DIR/pipensx.nro" 2>/dev/null || true
fi

echo "==> Copying pipensx.nro"
gio copy "$NRO_SRC" "$MTP_DIR/pipensx.nro"

echo "==> Done"
