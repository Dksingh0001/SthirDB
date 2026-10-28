#!/usr/bin/env bash
# scripts/demo_optimizer.sh — Phase 8 Demo 4: Cost Model & Access Path Choice
set -euo pipefail
cd "$(dirname "$0")/.."

if [ -d "/c/msys64/ucrt64/bin" ]; then
    export PATH="/c/msys64/ucrt64/bin:$PATH"
fi

if [ -f "./build/sthirdb.exe" ]; then
    BIN="./build/sthirdb.exe"
else
    BIN="./build/sthirdb"
fi

echo "=========================================================="
echo "  SthirDB Demo 4: Cost-Based Optimizer & Access Path Choice"
echo "=========================================================="

echo "[1/3] Ensuring index and statistics exist..."
"$BIN" run "CREATE INDEX idx_price ON Items (price)" 2>/dev/null || true
"$BIN" analyze Items > /dev/null

echo
echo "[2/3] Query where IndexScan wins (highly selective equality filter):"
P_QUERY="SELECT name FROM Items WHERE price = 25.0"
echo "EXPLAIN $P_QUERY"
"$BIN" explain "$P_QUERY"
echo
echo "OPTIMIZER Chosen Execution Metrics:"
"$BIN" optimizer "$P_QUERY"

echo
echo "[3/3] Query where SeqScan wins (broad range filter with low selectivity):"
R_QUERY="SELECT name FROM Items WHERE price > 0.0"
echo "EXPLAIN $R_QUERY"
"$BIN" explain "$R_QUERY"
echo
echo "OPTIMIZER Chosen Execution Metrics:"
"$BIN" optimizer "$R_QUERY"

echo
echo ">>> Demo 4 (Optimizer & Cost Model): PASS"
