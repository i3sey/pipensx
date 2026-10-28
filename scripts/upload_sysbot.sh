#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BOT_SCRIPT="${ROOT}/.agents/skills/switch-visual-test/scripts/switch_bot.py"

exec python3 "$BOT_SCRIPT" upload "$@"
