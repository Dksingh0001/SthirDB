# SthirDB

**SthirDB** ("*Sthir*" — स्थिर — steady, unwavering) is a disk-resident relational database engine with ARIES-style crash recovery and semantic query optimization, evaluated on a constraint-driven retail workload.

---

## Key Features

1. **Storage & Buffer Pool:** 8 KB slotted-page layout with LRU buffer pool management (64 frames by default).
2. **Volcano Query Execution:** Demand-driven iterator engine supporting SeqScan, IndexScan, Filter, Project, HashJoin, NestedLoopJoin, Hash Aggregation, and In-Memory Sort.
3. **Cost-Based Optimizer & Histograms:** Equi-width histograms, cardinality estimation, and cost modeling for optimal access path selection.
4. **Novelty 1 — Constraint-Aware Optimizer:** Semantic Query Optimization leveraging `PRIMARY KEY`, `UNIQUE`, `CHECK`, and `FOREIGN KEY` constraints to prune contradictory subtrees (0 page reads), eliminate redundant joins, and replace sequential scans with $O(\log N)$ B+Tree seeks (up to 463x speedup).
5. **Strict 2PL Concurrency:** Row-level Strict Two-Phase Locking with Shared/Exclusive modes, FIFO fairness, and global lock ordering deadlock avoidance.
6. **ARIES Crash Recovery:** Write-Ahead Logging (WAL) under steal/no-force policies, physiological logging, Compensation Log Records (CLRs) with idempotent recovery, and fuzzy checkpoints.
7. **Novelty 2 — Adversarial Crash-Injection Harness:** Fault-injection framework testing 7 named ARIES hazards with hard terminations and 100% recovery pass rate across 1,000+ trials.
8. **Retail Workload & Anomaly Injector:** 8-table normalized schema evaluated against 100,000 purchases, 300,000 line items, and multi-threaded concurrency (100, 300, 500 threads).

---

## Requirements & Building

- **Compiler:** C++17 compatible compiler (`g++` or `clang++`)
- **Build System:** GNU Make (`make` or `mingw32-make`)
- **Platform:** Linux, macOS, or Windows (MinGW-w64 / MSYS2 / UCRT64)

### Build Commands
```bash
# Build the main engine executable (./build/sthirdb or ./build/sthirdb.exe)
make
# Or on Windows MinGW:
mingw32-make

# Run the complete test suite (unit, integration, stress, recovery, concurrency, retail)
make test
# Or on Windows MinGW:
mingw32-make test
```

---

## Phase 8 Consolidated Demonstration Suite

Every demo is executable with a single command from the repository root:

```bash
# Demo 1: Storage, Slotted Pages & Buffer Pool
bash scripts/demo_storage.sh

# Demo 2: SQL-Subset Query Engine & Three-Valued Logic (3VL)
bash scripts/demo_query.sh

# Demo 3: B+Tree IndexScan vs SeqScan Comparison
bash scripts/demo_index.sh

# Demo 4: Cost-Based Optimizer & Access Path Choice
bash scripts/demo_optimizer.sh

# Demo 5: ARIES Crash Recovery & Durability (Tests A-H)
bash scripts/demo_recovery.sh

# Demo 6: Retail Workload, Anomaly Interception & Concurrency
bash scripts/demo_benchmark.sh

# Demo 7: Novelty 1 — Constraint-Aware Semantic Optimizer
bash scripts/demo_constraint_aware.sh

# Demo 8: Novelty 2A — Adversarial Crash-Injection Harness
bash scripts/demo_crash_injection.sh

# Demo 9: Novelty 2B — Checkpoint Frequency Trade-off Sweep
bash scripts/demo_checkpoint.sh

# Run all 9 demos in sequence:
bash scripts/demo_all.sh
```

---

## CLI Usage

```bash
# Execute SQL statements (DDL, DML, DQL, Control)
./build/sthirdb run "<sql>"

# Display plan tree with estimated rows, costs, and rejected paths
./build/sthirdb explain "<sql>"

# Execute with detailed optimizer telemetry (metrics and actual runtime)
./build/sthirdb optimizer "<sql>"
./build/sthirdb optimizer --seq "<sql>"    # Force sequential scan
./build/sthirdb optimizer --index "<sql>"  # Force B+Tree index scan

# Run ARIES restart recovery over the WAL
./build/sthirdb recover

# Execute a multi-statement SQL script file
./build/sthirdb sql scripts/retail_schema.sql

# Deterministically generate the retail benchmark dataset
./build/sthirdb retail-gen --constraints on --shoppers 1000 --items 5000 --purchases 100000

# Run the anomaly injection harness
./build/sthirdb inject --constraints on
./build/sthirdb inject --constraints off

# Run the concurrent multi-threaded reporting query benchmark
./build/sthirdb bench --threads 100,300,500 --iters 3 --constraints on
```

---

## Research Report

The complete, unified project research paper is documented in:
- **Report:** [`docs/report.md`](docs/report.md)
- **Empirical Evidence & Tables:** [`docs/evidence/`](docs/evidence/)
- **Retail Entity-Relationship Diagram:** [`docs/er_diagram.md`](docs/er_diagram.md)
