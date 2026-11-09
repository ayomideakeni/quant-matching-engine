#!/usr/bin/env bash
# Usage: ./run.sh --state-dir /absolute/path/to/state
# Builds on first use (compiler output goes to stderr, never stdout).
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$DIR/build/risk_engine"
if [ ! -x "$BIN" ] || [ "$DIR/src/risk_engine.cpp" -nt "$BIN" ]; then
  "$DIR/build.sh" >&2
fi
exec "$BIN" "$@"
