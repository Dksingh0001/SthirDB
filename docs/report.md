# SthirDB: A Disk-Resident Relational Engine with ARIES-Style Crash Recovery, Evaluated on a Constraint-Driven Retail Workload

> **Single-report policy.** This file is the only report in the project. Each phase drafts its own section here while the work happens; Phase 8 merges, de-duplicates, and formats. Raw numbers live in `docs/evidence/`. No other report files may exist in the repository.

*Draft state: skeleton created in Phase 0. Section ownership follows the table in `plan.md`.*

---

## 0. Title Block

**Title:** SthirDB: A Disk-Resident Relational Engine with ARIES-Style Crash Recovery, Evaluated on a Constraint-Driven Retail Workload

**Team:** _(filled in Phase 8)_

**Abstract:** _(≈150 words; drafted in Phase 8 once headline numbers exist — problem, what was built, the two novelties, and measured results)_

**Keywords:** ARIES, write-ahead logging, crash recovery, query optimizer, semantic query optimization, integrity constraints, B+tree index, buffer pool.

---

## 1. Introduction

_Drafted in Phase 0 from `docs/scope.md`; expanded in Phase 8._

Problem, motivation, why ARIES, scope, and the two novelties in one paragraph. Placeholder — to be written when the evaluation numbers exist.

---

## 2. Background

_Drafted in Phases 0–1 from the papers in `docs/papers/`; expanded in Phase 8._

ACID, write-ahead logging (WAL), ARIES, optimizer basics, semantic query optimization; every term defined in one plain sentence at first use. Placeholder.

---

## 3. System Overview

_Drafted in Phase 2._

This section describes the end-to-end software architecture of SthirDB and follows the complete lifecycle of a SQL query from user input to disk retrieval. SthirDB processes declarative SQL text through a classical layered DBMS architecture: tokenization, AST generation, semantic validation against a persistent catalog, translation into relational algebra, and demand-driven iterator execution over a pinned buffer pool.

### 3.1 Architecture Diagram

Figure 1 outlines the layered subsystem hierarchy delivered across Phases 0–2.

```
       +-----------------------------------------------------------+
       |               Client CLI (sthirdb run "<sql>")            |
       +-----------------------------------------------------------+
                                     |
                                     v
       +-----------------------------------------------------------+
       | SQL Lexer & Parser (Hand-written recursive descent)       |
       |  - Tokenizer with source-location error reporting         |
       |  - AST: DDL (CreateTable), DML (Insert/Update/Delete),    |
       |        DQL (Select/Join/Agg/Sort), Control (Txn, Set)     |
       +-----------------------------------------------------------+
                                     |
                                     v
       +-----------------------------------------------------------+
       | Catalog (./data/retail/catalog.json)                      |
       |  - Table schemas, data types (INT, VARCHAR, DECIMAL)      |
       |  - Declared constraints (PK, UNIQUE, CHECK, FK, NOT NULL) |
       +-----------------------------------------------------------+
                                     |
                                     v
       +-----------------------------------------------------------+
       | Relational-Algebra Planner                                |
       |  - Semantic resolution & schema derivation                |
       |  - Plan nodes: SeqScan, Filter, Project, Aggregate,       |
       |    Sort, NestedLoopJoin                                   |
       +-----------------------------------------------------------+
                                     |
                                     v
       +-----------------------------------------------------------+
       | Volcano Execution Engine (Iterator: init / next / close)  |
       |  - Three-valued logic (3VL: TRUE, FALSE, UNKNOWN)         |
       |  - Hash aggregation (COUNT, SUM, AVG, MIN, MAX)           |
       |  - In-memory multi-key sort (ASC / DESC)                  |
       |  - Block nested-loop join with inner rescan               |
       |  - DML execution with slot tombstones                     |
       +-----------------------------------------------------------+
                                     |
                                     v
       +-----------------------------------------------------------+
       | Buffer Pool Manager (64 frames, LRU eviction replacer)    |
       |  - Frame array, page-to-frame table, pin/unpin protocol   |
       +-----------------------------------------------------------+
                                     |
                                     v
       +-----------------------------------------------------------+
       | Disk Manager (8 KB slotted pages in ./data/retail/*.dat)  |
       |  - Positional pread/pwrite with crash-injection hook      |
       +-----------------------------------------------------------+
```
**Figure 1.** SthirDB end-to-end query processing and storage architecture.

### 3.2 Journey of a Query

To understand how these components collaborate, consider the lifecycle of an acceptance query executed via `./build/sthirdb run`:

```sql
SELECT ItemName FROM Items WHERE RetailPrice > 5;
```

1. **Tokenization & Lexical Analysis:** The `Lexer` scans the input character stream and converts it into a sequence of strongly-typed tokens: `SELECT`, `IDENTIFIER("ItemName")`, `FROM`, `IDENTIFIER("Items")`, `WHERE`, `IDENTIFIER("RetailPrice")`, `GT(">")`, `INTEGER_LITERAL("5")`, `SEMICOLON`.
2. **Parsing & AST Construction:** The hand-written recursive-descent `Parser` processes tokens according to grammar rules, enforcing operator precedence. It outputs a `SelectStmt` AST node containing a projection list (`ItemName`), a table source (`Items`), and a binary comparison expression (`RetailPrice > 5`).
3. **Catalog Lookup & Semantic Analysis:** The `Planner` looks up `Items` in the persistent `Catalog` (`./data/retail/catalog.json`). It confirms the table exists, verifies that both `ItemName` and `RetailPrice` are valid columns, and determines their storage types (`VARCHAR(100)` and `DECIMAL`).
4. **Relational-Algebra Planning:** The planner produces an executable plan tree composed of:
   - `SeqScanPlan("Items")`: leaf node reading records from disk.
   - `FilterPlan(predicate: RetailPrice > 5, child: SeqScan)`: filters records.
   - `ProjectPlan(columns: [ItemName], child: Filter)`: extracts the requested output column.
5. **Physical Execution (Volcano Iterator Model):** The `ExecutionEngine` instantiates the corresponding executor tree (`ProjectExecutor` -> `FilterExecutor` -> `SeqScanExecutor`) and calls `init()` on the root.
6. **Buffer-Pool Access & Slotted-Page Deserialization:** When `FilterExecutor` calls `next()` on `SeqScanExecutor`:
   - `SeqScanExecutor` pins page 0 via `BufferPool::fetch_page(0)`.
   - For each active slot in the slotted page directory, it extracts the tuple bytes and deserializes the dynamic `Tuple` format, recording the record identifier (`RID{page_id, slot_id}`).
   - Once all slots on the page are read, `unpin_page(0, false)` decrements the pin count, and the executor advances to page 1.
7. **Predicate Evaluation with 3VL:** `FilterExecutor` evaluates `RetailPrice > 5` against each live tuple using three-valued logic. Comparisons involving `NULL` yield `UNKNOWN`, which the `WHERE` clause discards; only rows evaluating to `TRUE` pass to `ProjectExecutor`.
8. **Result Formatting:** `ProjectExecutor` yields a tuple containing only `ItemName = 'Widget'`. The CLI engine calculates column widths and prints a clean ASCII table (raw output verified in `docs/evidence/phase2_cli_verify.txt`):
   ```
   +----------+
   | ItemName |
   +----------+
   | Widget   |
   +----------+
   (1 row(s))
   ```

### 3.3 Definitions of Technical Terms

- **AST (Abstract Syntax Tree):** A tree representation of the syntactic structure of source SQL text, abstracting away punctuation and formatting.
- **Relational Algebra:** A formal mathematical query language consisting of relational operators (such as selection, projection, join, and aggregation) that define query semantics.
- **Volcano Iterator Model:** A pipelined query execution architecture where operators communicate through a uniform `init()`, `next()`, and `close()` interface, pulling tuples one at a time on demand.
- **Three-Valued Logic (3VL):** SQL's logical system with three truth states: `TRUE`, `FALSE`, and `UNKNOWN`. Comparisons with `NULL` evaluate to `UNKNOWN`, and `WHERE` filters preserve only rows evaluating to `TRUE`.
- **Slotted Page:** An 8 KB page storage layout that places a growing slot directory at the front and appends variable-length tuples backward from the end, enabling stable row identifiers via slot indices.
- **Constraint Declaration vs. Enforcement:** Storing declared data rules (such as `PRIMARY KEY`, `UNIQUE`, `CHECK`, and `FOREIGN KEY`) in the catalog during DDL parsing, while deferring runtime constraint violation checks to the transaction enforcement phase.

> **Plain-English summary**
> - SQL text is parsed into an AST without external dependencies and validated against a JSON table catalog.
> - Queries are converted into relational algebra trees and executed using the demand-driven Volcano iterator model.
> - SeqScan retrieves pages through the pinned 64-frame buffer pool and deserializes slotted-page records.
> - Filtering strictly implements SQL three-valued logic (NULL comparisons yield UNKNOWN).
> - All six Phase 2 acceptance statements execute successfully from the CLI with clean tabular output (`docs/evidence/phase2_cli_verify.txt`).

---

## 4. Storage, Indexing & Query Execution

_Drafted in Phases 1–3. This section holds the Phase 1 first draft (page format and buffer pool); the B+tree and SQL execution layers arrive in Phases 2–3._

### 4.1 Page format

SthirDB stores every table in one file. Page *i* of that file lives at byte offset `i × 8192`. The page size is **8 KB (8192 bytes)** and is locked for the whole project.

Each page is a *slotted page*: a fixed header, then a slot directory that grows forward from the header, then free space, then tuples that grow backward from the end of the page. The slot directory lets a tuple be found (by slot number), deleted (tombstoned), and later compacted without moving every other tuple by hand.

Page header layout (16 bytes, stored at the front of the page image):

| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | 4 B | `page_id` | Identifier of this page within its table file. |
| 4 | 8 B | `page_lsn` | Log sequence number of the last log record that modified this page. **Reserved in Phase 1; written from Phase 5** so redo can skip already-applied records (idempotent recovery). |
| 12 | 2 B | `slot_count` | Number of entries in the slot directory. |
| 14 | 2 B | `free_space_ptr` | Offset of the lowest-addressed tuple. Free space is the byte range `[16 + slot_count × 4, free_space_ptr)`. |

Each slot-directory entry is 4 bytes: `uint16 offset` (tuple position in the page) and `uint16 length` (tuple length; **0 means tombstoned** — the slot is free but the space is reclaimed only by compaction).

The Phase 1 demo table `Items` uses one fixed row layout of 63 bytes:

```
[int32 id][char[50] name][double price][bool deleted]
```

`serialize_row` / `deserialize_row` convert between this layout and the in-memory `Row` struct. The `deleted` flag supports logical delete (Phase 2 `DELETE`); slot tombstones support physical reuse.

### 4.2 Buffer pool

The buffer pool keeps a fixed number of in-memory *frames* (64 by default) that mirror 8 KB pages. Its jobs: reuse frames, respect pins, and write dirty pages back before reusing their frames.

- **Frame array** — `pool_size` pages of 8 KB each.
- **Page table** — hash map `page_id → frame_id`.
- **Pin counts** — a page may be evicted only when its pin count is 0. Callers `fetch_page` (pin +1) and `unpin_page(page_id, is_dirty)` (pin −1, optionally mark dirty).
- **Dirty flags** — set on unpin when the caller modified the page; on eviction of a dirty frame the pool writes the page back through the disk manager.
- **Free list** — frames never assigned; used before eviction is considered.
- **Replacer (LRU)** — tracks evictable frames (pin count 0) and returns the least-recently-used one on eviction. Contract: `record_access`, `set_evictable`, `evict`, `remove`, `size`.
- **Latch** — one mutex guards the whole pool, so `fetch_page` / `unpin_page` are thread-safe.

Pin protocol (the invariant every caller must follow): every successful `fetch_page` is matched by exactly one `unpin_page`; holding a pin across an operation keeps the page resident; marking dirty happens at unpin time, not at modify time.

### 4.3 Disk manager and crash-injection hook

The disk manager exposes `read_page`, `write_page`, and `allocate_page`. Every page write funnels through one low-level function, which calls `crash_point(name)`. In normal runs `crash_point` is a no-op; under the test flag (`STHIRDB_CRASH_POINT=<name>`) it hard-exits the process with no cleanup — the hook Novelty 2 (Phase 5B) will use to inject crashes without touching call sites.

### 4.4 Stress verification (Phase 1)

A stress test writes 5,000 rows through the buffer pool and reads every page back, verifying `id`, `name`, `price`, and the delete flag on every tuple. Two pool sizes were run: the specified 64-frame pool (41 pages of data, so the pool is large enough to hold the whole table), and an 8-frame pool that forces continuous eviction — every pass over the data writes dirty victims back and reloads them (Table 1).

**Table 1.** Phase 1 stress results (raw log: `docs/evidence/phase1_tests.txt`).

| Test | Frames | Rows | Pages | Write (ms) | Read+verify (ms) | Verified | Mismatches | Outcome |
|---|---|---|---|---|---|---|---|---|
| Stress A (spec pool) | 64 | 5000 | 41 | 0.81 | 0.23 | 5000 | 0 | PASS |
| Stress B (forced eviction) | 8 | 5000 | 41 | 0.65 | 0.25 | 5000 | 0 | PASS |

The end-to-end CLI check (`init_schema`, `load_data --rows 5000`, `count`) exits 0 and `count` returns exactly 5000 (`docs/evidence/phase1_cli_verify.txt`). All 21 unit tests pass (`docs/evidence/phase1_tests.txt`). One honest note: with 5,000 rows the table occupies only 41 pages, so the 64-frame run never evicts — the 8-frame run exists specifically to exercise eviction cycles and dirty writeback.

> **Plain-English summary**
> - A page is an 8 KB block with a 16-byte header, a slot directory up front, and tuples packed at the back.
> - `page_lsn` is in the header now but only starts being written in Phase 5, when ARIES lands.
> - The buffer pool holds 64 pinned-or-not frames, evicts only unpinned frames (LRU), and writes dirty pages back first.
> - Every disk write goes through one function with a crash hook, so crash tests later need no call-site changes.
> - The 5,000-row stress test passed with both a 64-frame and an 8-frame pool.

---

## 5. Optimizer

_Drafted in Phase 4._

Cost equations, cardinality rules and assumptions, plan enumeration, estimated vs. actual results. Placeholder.

---

## 6. Constraint-Aware Optimizer (Novelty 1)

_Drafted in Phase 4B._

Rules R1–R4, safety conditions, on/off A-vs-B results, and the read-benefit vs write-cost trade-off. Placeholder.

---

## 7. ARIES Crash Recovery

_Drafted in Phase 5._

Log format, per-page LSN, the three passes with pseudo-code, checkpointing, steal/no-force; Tests A–H results. Placeholder.

---

## 8. Testing Methodology (Novelty 2)

_Drafted in Phase 5B._

Crash-injection harness design, 1,000+ trial results and invariants, mutation-testing evidence, and the checkpoint-frequency trade-off graph. Placeholder.

---

## 9. Benchmark & Evaluation

_Drafted in Phase 7._

Anomaly tables (constrained vs unconstrained), latency tables, `EXPLAIN` evidence, comparison with Yuan 2026. Placeholder.

---

## 10. Syllabus Mapping

_Drafted in Phase 8._

The 7-row module table with a sentence per module on the delivered component. Placeholder.

---

## 11. Limitations & Future Work

_Drafted continuously; finalized in Phase 8._

Deferred stretch items, honest estimation-error findings, and negative results. Placeholder.

---

## 12. References

_Accumulated continuously._

1. C. Mohan, F. Haderle, B. Lindsay, H. Pirahesh, P. Schwarz. "ARIES: A Transaction Recovery Method Supporting Fine-Granularity Locking and Partial Rollbacks Using Write-Ahead Logging." *ACM TODS* 17(1), 1992. DOI: 10.1145/128765.128770.
2. "A Constraint-Driven Relational Database Framework for Robust Digital Retail Management." *ITM Web of Conferences* 91, 04015 (2026). DOI: 10.1051/itmconf/20269104015.

_(Full list — Leis 2015, LEON 2023, textbooks — accumulated through Phase 8.)_
