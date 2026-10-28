#!/usr/bin/env bash
# scripts/demo_query.sh — Phase 8 Demo 2: SQL-Subset Execution & 3VL
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
echo "  SthirDB Demo 2: SQL-Subset Query Engine & 3VL"
echo "=========================================================="

echo "[1/4] Ensuring schema and data exist..."
if [ ! -f "./data/retail/items.dat" ]; then
    rm -rf ./data
    mkdir -p ./data
    "$BIN" init_schema
    "$BIN" load_data --rows 1000
fi

echo
echo "[2/4] Range filter query with projection:"
"$BIN" run "SELECT id, name, price FROM Items WHERE price > 25.0 AND price < 26.0;"

echo
echo "[3/4] Aggregate query (COUNT, AVG, MIN, MAX):"
"$BIN" run "SELECT COUNT(*), AVG(price), MIN(price), MAX(price) FROM Items;"

echo
echo "[4/4] Inserting tuple with NULL price & validating Three-Valued Logic (3VL):"
"$BIN" run "INSERT INTO Items VALUES (99999, 'NullPriceItem', NULL);"
echo "Querying with IS NULL:"
"$BIN" run "SELECT id, name, price FROM Items WHERE price IS NULL;"
echo "Querying with standard comparison (price > 50 discards NULL under 3VL):"
"$BIN" run "SELECT id, name FROM Items WHERE id = 99999 AND price > 50;"

echo
echo ">>> Demo 2 (Query Execution & 3VL): PASS"
