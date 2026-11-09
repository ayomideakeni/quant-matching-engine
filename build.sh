#!/usr/bin/env bash
# Builds Challenge B (pre-trade risk engine) into RiskEngine/build/risk_engine.
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/RiskEngine/build.sh" "$@"
