#!/bin/sh
# scripts/recovery_demo.sh — Phase 5 ARIES-Style Recovery Demo
# Runs all 8 ARIES verification tests (Tests A-H) and logs evidence.
set -e

DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$DIR"

echo "=== SthirDB Phase 5 ARIES Recovery Demo ==="
echo "Building test_recovery..."

if command -v mingw32-make >/dev/null 2>&1; then
    mingw32-make build/test_recovery.exe
    TEST_BIN="./build/test_recovery.exe"
else
    make build/test_recovery
    TEST_BIN="./build/test_recovery"
fi

echo "Running Tests A-H..."
mkdir -p docs/evidence
"$TEST_BIN" | tee docs/evidence/phase5_tests.txt

echo "=== Phase 5 ARIES Recovery Demo Passed ==="
