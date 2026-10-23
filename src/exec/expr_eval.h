#pragma once

#include "exec/value.h"
#include "parser/ast.h"
#include "catalog/catalog.h"

namespace sthir {

// Evaluates an expression over a single tuple.
Value EvaluateExpr(const Expr* expr, const Tuple& tuple, const TableSchema& schema);

// Evaluates a predicate expression and returns SQL 3VL Tribool (True, False, Unknown).
Tribool EvaluatePredicate(const Expr* expr, const Tuple& tuple, const TableSchema& schema);

}  // namespace sthir
