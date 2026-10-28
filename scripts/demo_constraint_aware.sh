#!/usr/bin/env bash
# scripts/demo_constraint_aware.sh — Phase 8 Demo 7: Novelty 1 (Constraint-Aware Optimizer)
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
echo "  SthirDB Demo 7: Novelty 1 — Constraint-Aware Optimizer"
echo "=========================================================="

echo "[1/3] Checking retail database and constraints..."
if [ ! -f "./data/retail/catalog.json" ] || ! grep -q "Invoices" "./data/retail/catalog.json" 2>/dev/null; then
    rm -rf ./data
    mkdir -p ./data
    "$BIN" sql scripts/retail_schema.sql > /dev/null
    "$BIN" retail-gen --constraints on --shoppers 500 --items 1000 --purchases 5000 --lpp 2 > /dev/null
fi

QUERY1="SELECT Total FROM Invoices WHERE PurchaseID = 2500"

echo
echo "[2/3] Demonstrating Constraint-Backed Index Acceleration (Novelty 1 R1):"
echo "Query: $QUERY1"
echo "--- Unconstrained / Forced-Seq Execution (No constraint index) ---"
"$BIN" optimizer --seq "$QUERY1"

echo "--- Constrained / Index Execution (Constraint-aware unique lookup) ---"
"$BIN" optimizer --index "$QUERY1"

echo
echo "[3/3] Demonstrating Constraint Contradiction Pruning (Novelty 1 R2):"
CHECK_QUERY="SELECT COUNT(*) FROM Items WHERE StockLevel < 0"
echo "Query: $CHECK_QUERY (Table has CHECK (StockLevel >= 0))"
"$BIN" explain "$CHECK_QUERY"

echo
echo ">>> Demo 7 (Novelty 1 Constraint-Aware Optimizer): PASS"
