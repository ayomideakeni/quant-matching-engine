#!/usr/bin/env bash
# Challenge B (pre-trade risk engine) entry point: ./run.sh --state-dir /abs/path
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/RiskEngine/run.sh" "$@"
