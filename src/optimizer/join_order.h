#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace sthir {

// Input for join-order enumeration: one summary per table.
struct JoinTableSummary {
  std::string table_name;
  double pages = 0.0;
  double rows = 0.0;
};

struct JoinOrderPlan {
  std::vector<std::string> order;
  double cost = 0.0;
};

// Enumerates every permutation of a left-deep nested-loop join (3! = 6 for
// three tables) and returns them cheapest first.
//
// Join selectivity is assumed to be 1 because the predicate is not modelled at
// this level, so the row counts (and therefore the costs) are upper bounds. The
// SQL grammar accepts a single JOIN, so this stretch item is exercised by unit
// tests rather than by a query end to end (stated in report §5).
std::vector<JoinOrderPlan> EnumerateJoinOrders(const std::vector<JoinTableSummary>& tables);

}  // namespace sthir
