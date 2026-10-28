#!/usr/bin/env bash
# scripts/demo_storage.sh — Phase 8 Demo 1: Storage & Slotted Page Engine
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
echo "  SthirDB Demo 1: Storage, Slotted Pages & Buffer Pool"
echo "=========================================================="

echo "[1/3] Initializing clean database schema for Items table..."
rm -rf ./data
mkdir -p ./data
"$BIN" init_schema

echo
echo "[2/3] Loading 5,000 disk-resident records into Items..."
"$BIN" load_data --rows 5000

echo
echo "[3/3] Querying live row count directly from slotted pages..."
"$BIN" count

echo
echo "Executing SQL verification via Volcano SeqScan:"
"$BIN" run "SELECT COUNT(*) FROM Items;"

echo
echo ">>> Demo 1 (Storage Engine): PASS"
