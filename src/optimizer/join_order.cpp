#include "optimizer/join_order.h"

#include <algorithm>
#include <numeric>

#include "optimizer/cost_model.h"

namespace sthir {

namespace {

// Left-deep nested-loop cost for one table order:
//   pages(first) + rows(first)*pages(second) + rows(first)*rows(second)*pages(third) + ...
// with join selectivity assumed to be 1.
double OrderCost(const std::vector<size_t>& perm, const std::vector<JoinTableSummary>& tables) {
  double cost = tables[perm[0]].pages * kSeqPageCost;
  double rows = tables[perm[0]].rows;
  for (size_t i = 1; i < perm.size(); ++i) {
    cost += rows * tables[perm[i]].pages * kSeqPageCost;
    rows *= tables[perm[i]].rows;
  }
  return cost;
}

}  // namespace

std::vector<JoinOrderPlan> EnumerateJoinOrders(const std::vector<JoinTableSummary>& tables) {
  std::vector<JoinOrderPlan> plans;
  if (tables.empty()) return plans;

  std::vector<size_t> perm(tables.size());
  std::iota(perm.begin(), perm.end(), 0);
  do {
    JoinOrderPlan plan;
    plan.cost = OrderCost(perm, tables);
    plan.order.reserve(perm.size());
    for (size_t index : perm) plan.order.push_back(tables[index].table_name);
    plans.push_back(std::move(plan));
  } while (std::next_permutation(perm.begin(), perm.end()));

  std::stable_sort(plans.begin(), plans.end(),
                   [](const JoinOrderPlan& a, const JoinOrderPlan& b) { return a.cost < b.cost; });
  return plans;
}

}  // namespace sthir
