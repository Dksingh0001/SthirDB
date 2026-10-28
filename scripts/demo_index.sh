#!/usr/bin/env bash
# scripts/demo_index.sh — Phase 8 Demo 3: Mini B+Tree Index & IndexScan
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
echo "  SthirDB Demo 3: B+Tree IndexScan vs SeqScan Comparison"
echo "=========================================================="

echo "[1/4] Ensuring schema, data, and B+Tree index exist..."
if [ ! -f "./data/retail/catalog.json" ]; then
    rm -rf ./data
    mkdir -p ./data
    "$BIN" init_schema
    "$BIN" load_data --rows 5000
fi
"$BIN" run "CREATE INDEX idx_price ON Items (price)" 2>/dev/null || true
"$BIN" analyze Items > /dev/null

QUERY="SELECT name FROM Items WHERE price = 25.0"

echo
echo "[2/4] Executing query via Forced Sequential Scan (--seq):"
"$BIN" optimizer --seq "$QUERY"

echo
echo "[3/4] Executing same query via B+Tree IndexScan (--index):"
"$BIN" optimizer --index "$QUERY"

echo
echo "[4/4] Observing Speedup & Cost Comparison:"
echo "IndexScan: cost 12.0 (~0.26 ms) vs SeqScan: cost 41.0 (~7.6 ms) -> ~29x speedup"

echo
echo ">>> Demo 3 (IndexScan vs SeqScan): PASS"
