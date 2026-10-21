# SthirDB — Scope & Locked Decisions

SthirDB is a disk-resident relational engine with ARIES-style crash recovery, evaluated on a constraint-driven retail workload. This file records the decisions locked at Phase 0. Changing any locked item mid-project invalidates tests and offsets; revisit only with a written note here.

## Locked decisions

| Decision | Choice | Rationale |
|---|---|---|
| Language | C++17 | Chosen once at Phase 0; switching mid-project is the top schedule risk. |
| Page size | 8192 bytes (8 KB) | Every on-disk offset depends on it. Locked now, never revisited. |
| Buffer pool | 64 frames by default | Small enough to force eviction in demos; tests also use 8-frame pools. |
| Build | Plain `make` → `./build/sthirdb` | No external build dependency beyond g++ and GNU make. |
| Tests | Assert-style binaries under `test/`, run by `make test` | No external test framework. |
| Storage layout | One file per table (`./data/retail/<table>.dat`); page *i* at offset `i * 8192` | Simple addressing; ARIES (Phase 5) layers WAL on top. |
| Row format (Phase 1 demo table `Items`) | `[int32 id][char[50] name][double price][bool deleted]` — 63 bytes fixed | Fixed-size rows keep the Phase 1 slotted-page work small; schema generalizes in Phase 2. |

## Trim budget (what gets dropped first if time runs out)

1. Concurrency / two-phase locking (Phase 6).
2. Join-order optimization (Phase 4 stretch).
3. Full 8-table retail schema → 4-table subset (Phase 7).

## Report policy

The project produces exactly one report for the whole project: `docs/report.md`. Every phase writes its own section into that same file; raw numbers go to `docs/evidence/`. No per-phase or per-member report files. See the Single-Report Policy at the top of `plan.md`.

## Anchor papers (citations)

- **ARIES** — C. Mohan, F. Haderle, B. Lindsay, H. Pirahesh, P. Schwarz. "ARIES: A Transaction Recovery Method Supporting Fine-Granularity Locking and Partial Rollbacks Using Write-Ahead Logging." *ACM Transactions on Database Systems* 17(1), 1992. DOI: 10.1145/128765.128770. PDF: `docs/papers/aries.pdf`.
- **Yuan 2026** — "A Constraint-Driven Relational Database Framework for Robust Digital Retail Management." *ITM Web of Conferences* 91, 04015 (2026). DOI: 10.1051/itmconf/20269104015. PDF: `docs/papers/yuan2026.pdf`.

Further references (Leis 2015, LEON 2023, textbooks) are listed in `plan.md` § Reference List and will be filed in `docs/report.md` §12.
