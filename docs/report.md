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

Architecture diagram: storage → buffer pool → executors → optimizer (including the constraint-aware rewrite) → WAL/recovery (plus crash-injection layer) → workload. Placeholder.

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

A stress test writes 5,000 rows through the buffer pool and reads every row back, checking each field. Two pool sizes were run: the specified 64-frame pool, and an 8-frame pool that forces continuous eviction (write-back and reload on every pass over the data). Results:

| Test | Frames | Rows written | Rows verified | Outcome |
|---|---|---|---|---|
| Stress A (spec pool) | 64 | 5000 | 5000 | PASS |
| Stress B (forced eviction) | 8 | 5000 | 5000 | PASS |

Raw output: `docs/evidence/phase1_stress.txt`; unit-test log: `docs/evidence/phase1_tests.txt`.

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
