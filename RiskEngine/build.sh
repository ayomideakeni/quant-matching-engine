#!/usr/bin/env bash
# Builds the risk engine into build/risk_engine. All output goes to stderr.
# Safe to run concurrently: each build links to a private temp file and
# atomically renames it into place.
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CXX="${CXX:-g++}"
mkdir -p "$DIR/build"
TMP="$(mktemp "$DIR/build/.risk_engine.XXXXXX")"
trap 'rm -f "$TMP"' EXIT
"$CXX" -std=c++17 -O2 -Wall -Wextra -Wpedantic -o "$TMP" "$DIR/src/risk_engine.cpp" "$@" 1>&2
chmod 755 "$TMP"
mv -f "$TMP" "$DIR/build/risk_engine"
trap - EXIT
