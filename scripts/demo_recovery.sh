#!/usr/bin/env bash
# scripts/demo_recovery.sh — Phase 8 Demo 5: ARIES Crash Recovery (Tests A-H)
set -euo pipefail
cd "$(dirname "$0")/.."

if [ -d "/c/msys64/ucrt64/bin" ]; then
    export PATH="/c/msys64/ucrt64/bin:$PATH"
fi

echo "=========================================================="
echo "  SthirDB Demo 5: ARIES Crash Recovery & Durability"
echo "=========================================================="

echo "Running ARIES Recovery Verification Suite (Tests A-H)..."
if [ -f "./build/test_recovery.exe" ]; then
    TEST_BIN="./build/test_recovery.exe"
else
    TEST_BIN="./build/test_recovery"
fi

"$TEST_BIN"

echo
echo ">>> Demo 5 (ARIES Recovery): PASS"
