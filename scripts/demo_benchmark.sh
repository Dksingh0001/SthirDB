#!/usr/bin/env bash
# scripts/demo_benchmark.sh — Phase 8 Demo 6: Retail Workload, Interception & Concurrency
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
echo "  SthirDB Demo 6: Retail Workload, Anomaly Interception & Benchmark"
echo "=========================================================="

echo "[1/3] Checking retail database..."
if [ ! -f "./data/retail/catalog.json" ] || ! grep -q "Staff" "./data/retail/catalog.json" 2>/dev/null; then
    echo "Creating retail schema..."
    rm -rf ./data
    mkdir -p ./data
    "$BIN" sql scripts/retail_schema.sql > /dev/null
    echo "Loading retail dataset..."
    "$BIN" retail-gen --constraints on --shoppers 500 --items 1000 --purchases 5000 --lpp 2 > /dev/null
fi

echo
echo "[2/3] Demonstrating Anomaly Injection & Interception (Constrained vs Unconstrained):"
echo "--- Constrained Mode (Interception Rate) ---"
"$BIN" inject --constraints on --count 10

echo "--- Unconstrained Mode (Interception Rate) ---"
"$BIN" inject --constraints off --count 10

echo
echo "[3/3] Running Concurrent Reporting Benchmark (Multi-threaded Latency):"
"$BIN" bench --constraints on --threads 10,25,50 --iters 2

echo
echo ">>> Demo 6 (Benchmark & Evaluation): PASS"
