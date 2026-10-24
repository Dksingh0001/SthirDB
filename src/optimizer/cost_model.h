#pragma once

namespace sthir {

// Chosen cost constants (not measured values — documented in report §5):
//   cost(plan) = pages_read * kSeqPageCost + pages_random * kRandPageCost
inline constexpr double kSeqPageCost = 1.0;
inline constexpr double kRandPageCost = 4.0;

// seq_scan_cost = pages(table), read sequentially.
double SeqScanCost(double table_pages);

// Pages of the base table that hold `matching_rows` rows, packed at the
// table's average fill density: ceil(rows * pages / row_count), clamped to
// [1, table_pages] for a non-empty match. This assumes matching rows are
// physically co-located (true for the append-ordered load used here); for
// scattered data the true count approaches min(rows, table_pages).
// Stated as an assumption in docs/report.md §5.
double PagesOfMatchingRows(double matching_rows, double table_rows, double table_pages);

// index_scan_cost = index_height + pages_of_matching_rows: the descent is
// priced as a sequential walk over a few buffer-resident pages, the matching
// table pages as random RID fetches (rand_page_cost each).
double IndexScanCost(double index_height, double matching_rows, double table_rows,
                     double table_pages);

// Predicate evaluation is CPU only; the model counts page I/O, so a filter adds
// nothing on top of its child.
double FilterCost(double child_cost, double input_rows);

// External sort: nothing when the rows fit in the buffer pool, otherwise one
// write pass and one read pass over the data.
double SortCost(double rows, double row_bytes);

// In-memory hash aggregation reads its input once (already counted by the child)
// and writes no pages, so its own cost is zero.
double AggregateCost(double child_cost, double input_rows);

// nested_loop_join_cost = pages(outer) + rows(outer) * pages(inner)
// (the inner side is rescanned once per outer row).
double NestedLoopJoinCost(double outer_pages, double outer_rows, double inner_pages);

}  // namespace sthir
