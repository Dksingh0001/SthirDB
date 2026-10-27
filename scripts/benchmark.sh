#!/usr/bin/env bash
# Phase 7 end-to-end benchmark.
#
# Reproduces the full Phase 7 result set from a clean ./data/:
#   1. build the binary
#   2. constrained run:   schema -> retail-gen (indexes) -> EXPLAIN + pages-read
#                         for the workload and the rule-triggering queries ->
#                         concurrent benchmark (100/300/500 threads) ->
#                         anomaly injections (default, extended, rate sweeps)
#   3. unconstrained run: same sequence with `SET constraints_enabled = off`
#                         (no constraint-backed indexes, no checks)
#   4. merged comparison tables for docs/report.md section 9
#
# Raw tables land in docs/evidence/. Everything is deterministic (seed 42),
# so a second run reproduces the same data and the same interception rates;
# latencies naturally vary with machine load.
set -euo pipefail
cd "$(dirname "$0")/.."

BIN="./build/sthirdb"
EV="docs/evidence"
mkdir -p "$EV"

WORKLOAD="SELECT Items.CategoryName, SUM(Purchase_Line_Items.LineTotal), COUNT(*) FROM Purchases JOIN Purchase_Line_Items ON Purchases.PurchaseID = Purchase_Line_Items.PurchaseID JOIN Items ON Purchase_Line_Items.ItemID = Items.ItemID JOIN Invoices ON Purchases.PurchaseID = Invoices.PurchaseID WHERE Purchases.PurchaseDate >= 20251101 AND Invoices.IssuedDate >= 20251102 GROUP BY Items.CategoryName"

# Queries designed to trigger the Phase 4B rules (R1 unique lookup, R2/R2b
# CHECK, R3 join elimination, R4 FK join cardinality) plus one control query
# no rule may touch. Captured on both schemas so the report can show what the
# constraint metadata makes possible and what the indexes actually buy.
RULE_QUERIES=(
  "SELECT ItemName FROM Items WHERE ItemID = 4242"
  "SELECT Total FROM Invoices WHERE PurchaseID = 50000"
  "SELECT Name FROM Shoppers WHERE Email = 'shopper537@retail.test'"
  "SELECT COUNT(*) FROM Items WHERE StockLevel < 0"
  "SELECT COUNT(*) FROM Items WHERE StockLevel >= 0"
  "SELECT COUNT(*) FROM Purchases JOIN Shoppers ON Purchases.ShopperID = Shoppers.ShopperID"
  "SELECT COUNT(*) FROM Items WHERE RetailPrice > 100"
)

echo "== building"
make -s all

run_mode() {
  local mode="$1"     # on | off
  local label         # constrained | unconstrained
  if [ "$mode" = "off" ]; then label="unconstrained"; else label="constrained"; fi

  echo "== [$label] clean ./data/ and create schema"
  rm -rf ./data
  # The WAL's mkdir is single-level: the parent ./data/ must exist first.
  mkdir -p ./data
  "$BIN" sql scripts/retail_schema.sql > /dev/null

  echo "== [$label] generate dataset (seed 42)"
  "$BIN" retail-gen --constraints "$mode" | tee "$EV/phase7_retail_gen_${label}.txt"

  echo "== [$label] EXPLAIN + metrics for the workload query"
  {
    echo "# Workload query plan [constraints=$mode]"
    echo
    echo '```'
    echo "EXPLAIN $WORKLOAD"
    "$BIN" explain "$WORKLOAD"
    echo
    echo "optimizer output (includes pages_read):"
    "$BIN" optimizer "$WORKLOAD"
    echo '```'
  } > "$EV/phase7_explain_workload_${label}.txt"

  echo "== [$label] EXPLAIN + pages-read for the rule-triggering queries"
  {
    echo "# Rule-triggering queries [constraints=$mode]"
    echo "# Each entry: EXPLAIN (plan, estimates) then optimizer (actual time, pages_read)."
    echo
    local q
    for q in "${RULE_QUERIES[@]}"; do
      echo "=============================================================="
      echo "QUERY: $q"
      echo "--------------------------------------------------------------"
      "$BIN" explain "$q"
      "$BIN" optimizer "$q"
      echo
    done
    # For the queries whose plan uses a constraint-backed index, also force
    # the sequential path: same rows, more pages read.
    echo "=============================================================="
    echo "FORCED-SEQ comparison (identical rows, page-I/O difference)"
    echo "--------------------------------------------------------------"
    for q in "SELECT ItemName FROM Items WHERE ItemID = 4242" \
             "SELECT Total FROM Invoices WHERE PurchaseID = 50000"; do
      echo "QUERY (forced seq): $q"
      "$BIN" optimizer --seq "$q"
      echo
    done
  } > "$EV/phase7_rules_${label}.txt"

  echo "== [$label] concurrent benchmark (100/300/500 threads, 3 iterations each)"
  "$BIN" bench --constraints "$mode" --threads 100,300,500 --iters 3 \
    | tee "$EV/phase7_bench_${label}.md"

  # Injections come last: the unconstrained baseline actually accepts (and
  # therefore writes) the anomalies, so everything read-only runs first.
  echo "== [$label] anomaly injection: default 3 x 50"
  "$BIN" inject --constraints "$mode" | tee "$EV/phase7_inject_default_${label}.md"

  echo "== [$label] anomaly injection: extended (8 classes x 50)"
  "$BIN" inject --constraints "$mode" --extended \
    | tee "$EV/phase7_inject_extended_${label}.md"

  echo "== [$label] rate sweep (1% / 5% / 10%)"
  {
    echo "# Injection-rate sweep — $label [constraints=$mode]"
    echo
    local rate
    for rate in 1 5 10; do
      "$BIN" inject --constraints "$mode" --rate "$rate"
      echo
    done
  } | tee "$EV/phase7_rate_sweep_${label}.md"
}

run_mode on
run_mode off

echo "== merging comparison tables"
{
  echo "### Constrained vs unconstrained — concurrent query latency"
  echo
  echo "3 iterations per thread; latency = wall time of one Execute (parse + plan + execute)."
  echo
  echo "| mode | threads | queries | failures | avg ms | p99 ms | min ms | max ms |"
  echo "|---|---:|---:|---:|---:|---:|---:|---:|"
  for label in constrained unconstrained; do
    awk -v mode="$label" '/^\| [0-9]/ { sub(/^\| /, ""); print "| " mode " | " $0 }' \
      "$EV/phase7_bench_${label}.md"
  done
} > "$EV/phase7_latency_comparison.md"

{
  echo "### Constrained vs unconstrained — anomaly interception (default 3 x 50)"
  echo
  echo "| mode | class | constraint | injected | blocked | accepted | interception % |"
  echo "|---|---|---|---:|---:|---:|---:|"
  for label in constrained unconstrained; do
    awk -v mode="$label" \
      '/^\| [a-z]/ && $0 !~ /\| class \| constraint \|/ { sub(/^\| /, ""); print "| " mode " | " $0 }' \
      "$EV/phase7_inject_default_${label}.md"
  done
  echo
  echo "### Constrained vs unconstrained — anomaly interception (8 classes x 50)"
  echo
  echo "| mode | class | constraint | injected | blocked | accepted | interception % |"
  echo "|---|---|---|---:|---:|---:|---:|"
  for label in constrained unconstrained; do
    awk -v mode="$label" \
      '/^\| [a-z]/ && $0 !~ /\| class \| constraint \|/ { sub(/^\| /, ""); print "| " mode " | " $0 }' \
      "$EV/phase7_inject_extended_${label}.md"
  done
} > "$EV/phase7_interception_summary.md"

echo
echo "benchmark.sh: done. Evidence files:"
ls -1 "$EV" | grep phase7 | sed 's/^/  /'