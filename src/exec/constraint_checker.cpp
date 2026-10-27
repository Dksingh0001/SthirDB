#include "exec/constraint_checker.h"

#include <algorithm>

#include "exec/expr_eval.h"
#include "parser/lexer.h"
#include "parser/parser.h"

namespace sthir {

namespace {

std::string LowerCopy(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Cache id for one key set: "table|col1|col2" (lower case).
std::string SetId(const std::string& table, const std::vector<std::string>& columns) {
  std::string id = LowerCopy(table);
  for (const auto& col : columns) id += "|" + LowerCopy(col);
  return id;
}

Value ValueAt(const TableSchema& schema, const Tuple& row, const std::string& col) {
  const int idx = schema.FindColumnIndex(col);
  if (idx < 0 || static_cast<size_t>(idx) >= row.size()) return Value::MakeNull();
  return row.GetValue(static_cast<size_t>(idx));
}

std::vector<Value> ValuesOf(const TableSchema& schema, const std::vector<std::string>& columns,
                            const Tuple& row) {
  std::vector<Value> values;
  values.reserve(columns.size());
  for (const auto& col : columns) values.push_back(ValueAt(schema, row, col));
  return values;
}

bool AnyNull(const std::vector<Value>& values) {
  for (const auto& v : values) {
    if (v.is_null()) return true;
  }
  return false;
}

// Composite key encoding shared by checks and the key-set loader.
std::string EncodeValues(const std::vector<Value>& values) {
  std::string key;
  for (const auto& v : values) {
    if (!key.empty()) key.push_back('\x1f');
    key += v.ToString();
  }
  return key;
}

std::string JoinColumns(const std::vector<std::string>& columns) {
  std::string s;
  for (const auto& c : columns) {
    if (!s.empty()) s += ",";
    s += c;
  }
  return s;
}

std::string ValuesText(const std::vector<Value>& values) {
  std::string s;
  for (const auto& v : values) {
    if (!s.empty()) s += ",";
    s += v.ToString();
  }
  return s;
}

bool SameValues(const std::vector<Value>& a, const std::vector<Value>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (!(a[i] == b[i])) return false;
  }
  return true;
}

}  // namespace

ConstraintChecker::ConstraintChecker(Catalog* catalog, KeySetLoader loader, KeyDeleter deleter)
    : catalog_(catalog), loader_(std::move(loader)), deleter_(std::move(deleter)) {}

void ConstraintChecker::SetEnabled(bool enabled) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (enabled_ == enabled) return;
  enabled_ = enabled;
  // Crossing the flag boundary re-reads reality on the next check.
  key_sets_.clear();
  rules_.clear();
}

bool ConstraintChecker::enabled() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return enabled_;
}

void ConstraintChecker::InvalidateAll() {
  std::lock_guard<std::mutex> lock(mutex_);
  key_sets_.clear();
  rules_.clear();
}

bool ConstraintChecker::KeyOf(const TableSchema& schema,
                              const std::vector<std::string>& columns, const Tuple& row,
                              std::string* out) {
  const std::vector<Value> values = ValuesOf(schema, columns, row);
  if (AnyNull(values)) return false;
  *out = EncodeValues(values);
  return true;
}

ConstraintChecker::TableRules ConstraintChecker::BuildRules(const TableSchema& schema) const {
  TableRules rules;
  if (!schema.primary_key_columns.empty()) {
    rules.unique_groups.push_back(schema.primary_key_columns);
  }
  for (const auto& group : schema.unique_constraints) {
    rules.unique_groups.push_back(group);
  }
  for (const auto& col : schema.columns) {
    if (col.not_null || col.primary_key) rules.not_null_columns.push_back(col.name);
  }
  rules.checks = schema.check_constraints;

  const std::string lower = LowerCopy(schema.table_name);
  if (catalog_ != nullptr) {
    for (const auto& edge : catalog_->GetForeignKeyInfo()) {
      if (LowerCopy(edge.child_table) == lower) rules.child_edges.push_back(edge);
      if (LowerCopy(edge.parent_table) == lower) rules.parent_edges.push_back(edge);
    }
  }
  rules.any = !rules.unique_groups.empty() || !rules.checks.empty() ||
              !rules.child_edges.empty() || !rules.parent_edges.empty() ||
              !rules.not_null_columns.empty();
  return rules;
}

const Expr* ConstraintChecker::CheckExpression(const std::string& table,
                                               const std::string& expression) {
  const std::string id = SetId(table, {expression});
  auto it = check_exprs_.find(id);
  if (it != check_exprs_.end()) return it->second.get();

  Lexer lexer(expression);
  auto tokens = lexer.Tokenize();
  if (lexer.has_error()) return nullptr;
  Parser parser(std::move(tokens));
  auto expr = parser.ParseExpression();
  if (parser.has_error() || !expr) return nullptr;
  auto* raw = expr.get();
  check_exprs_[id] = std::move(expr);
  return raw;
}

bool ConstraintChecker::KeySetFor(const std::string& table,
                                  const std::vector<std::string>& columns,
                                  std::unordered_map<std::string, uint32_t>** set,
                                  std::string* err) {
  const std::string id = SetId(table, columns);
  auto it = key_sets_.find(id);
  if (it == key_sets_.end()) {
    std::unordered_map<std::string, uint32_t> fresh;
    if (loader_ && !loader_(table, columns, &fresh, err)) return false;
    it = key_sets_.emplace(id, std::move(fresh)).first;
  }
  *set = &it->second;
  return true;
}

std::vector<ConstraintChecker::RowKey> ConstraintChecker::KeysForRow(
    const TableSchema& schema, const TableRules& rules, const Tuple& row) const {
  std::vector<RowKey> keys;
  auto add = [&](const std::string& table, const std::vector<std::string>& columns) {
    std::string key;
    if (!KeyOf(schema, columns, row, &key)) return;  // NULL component: not a key
    const std::string id = SetId(table, columns);
    for (const auto& existing : keys) {
      if (existing.id == id) return;  // already counted once for this set
    }
    keys.push_back({id, std::move(key)});
  };

  // Uniqueness groups (PRIMARY KEY first).
  for (const auto& group : rules.unique_groups) add(schema.table_name, group);
  // FK child-column sets: this table's rows ARE the multiset the parent's
  // RESTRICT check probes.
  for (const auto& edge : rules.child_edges) add(schema.table_name, edge.child_columns);
  // Parent-key sets of FKs pointing here (covers a referenced non-key column;
  // for key columns this coincides with the uniqueness group above).
  for (const auto& edge : rules.parent_edges) add(schema.table_name, edge.parent_columns);
  return keys;
}

// ------------------------------------------------------------- INSERT

bool ConstraintChecker::CheckInsert(const TableSchema& schema,
                                    const std::vector<Tuple>& rows, std::string* err) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!enabled_) return true;
  auto rules_it = rules_.find(LowerCopy(schema.table_name));
  if (rules_it == rules_.end()) {
    rules_it = rules_.emplace(LowerCopy(schema.table_name), BuildRules(schema)).first;
  }
  const TableRules& rules = rules_it->second;
  if (!rules.any || rows.empty()) return true;

  // Keys already claimed earlier in this same statement (intra-batch dups).
  std::vector<std::unordered_set<std::string>> batch(rules.unique_groups.size());

  for (const Tuple& row : rows) {
    // 1. NOT NULL (PK columns count as NOT NULL even when not declared so).
    for (const auto& col : rules.not_null_columns) {
      if (ValueAt(schema, row, col).is_null()) {
        if (err) {
          *err = std::string(kNotNullViolation) + " (" + schema.table_name + "." + col +
                 " is NULL)";
        }
        return false;
      }
    }

    // 2. CHECK — passes when the predicate is not FALSE (SQL 3VL: NULL passes).
    for (const auto& check : rules.checks) {
      const Expr* expr = CheckExpression(schema.table_name, check.expression);
      if (expr == nullptr) continue;
      if (EvaluatePredicate(expr, row, schema) == Tribool::False) {
        if (err) {
          *err = std::string(kCheckViolation) + " (" + schema.table_name + ": " +
                 check.expression + ")";
        }
        return false;
      }
    }

    // 3. PRIMARY KEY / UNIQUE (against this statement and the stored keys).
    for (size_t g = 0; g < rules.unique_groups.size(); ++g) {
      const auto& group = rules.unique_groups[g];
      std::string key;
      if (!KeyOf(schema, group, row, &key)) continue;  // NULL key: covered by NOT NULL
      if (batch[g].find(key) != batch[g].end()) {
        if (err) {
          const bool is_pk = (g == 0 && !schema.primary_key_columns.empty() &&
                              group == schema.primary_key_columns);
          *err = std::string(is_pk ? kPrimaryKeyViolation : kUniqueViolation) + " (" +
                 schema.table_name + " (" + JoinColumns(group) + ")=" + ValuesText(ValuesOf(schema, group, row)) +
                 " already exists)";
        }
        return false;
      }
      std::unordered_map<std::string, uint32_t>* set = nullptr;
      if (!KeySetFor(schema.table_name, group, &set, err)) return false;
      if (set->find(key) != set->end()) {
        if (err) {
          const bool is_pk = (g == 0 && !schema.primary_key_columns.empty() &&
                              group == schema.primary_key_columns);
          *err = std::string(is_pk ? kPrimaryKeyViolation : kUniqueViolation) + " (" +
                 schema.table_name + " (" + JoinColumns(group) + ")=" +
                 ValuesText(ValuesOf(schema, group, row)) + " already exists)";
        }
        return false;
      }
      batch[g].insert(std::move(key));
    }

    // 4. FOREIGN KEY — the referenced key must exist in the parent table.
    for (const auto& edge : rules.child_edges) {
      const std::vector<Value> values = ValuesOf(schema, edge.child_columns, row);
      if (AnyNull(values)) continue;  // NULL FK is allowed unless NOT NULL says otherwise
      std::unordered_map<std::string, uint32_t>* set = nullptr;
      if (!KeySetFor(edge.parent_table, edge.parent_columns, &set, err)) return false;
      if (set->find(EncodeValues(values)) == set->end()) {
        if (err) {
          *err = std::string(kForeignKeyViolation) + " (" + schema.table_name + "." +
                 JoinColumns(edge.child_columns) + "=" + ValuesText(values) +
                 " not found in " + edge.parent_table + "." +
                 JoinColumns(edge.parent_columns) + ")";
        }
        return false;
      }
    }
  }
  return true;
}

void ConstraintChecker::RecordInsert(const TableSchema& schema, const Tuple& row) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!enabled_) return;
  auto rules_it = rules_.find(LowerCopy(schema.table_name));
  if (rules_it == rules_.end()) return;  // never checked: nothing cached yet
  for (const auto& row_key : KeysForRow(schema, rules_it->second, row)) {
    auto set_it = key_sets_.find(row_key.id);
    if (set_it != key_sets_.end()) ++set_it->second[row_key.key];
  }
}

// ------------------------------------------------------------- UPDATE

bool ConstraintChecker::CheckUpdate(const TableSchema& schema, const Tuple& old_row,
                                    const Tuple& new_row, std::string* err) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!enabled_) return true;
  auto rules_it = rules_.find(LowerCopy(schema.table_name));
  if (rules_it == rules_.end()) {
    rules_it = rules_.emplace(LowerCopy(schema.table_name), BuildRules(schema)).first;
  }
  const TableRules& rules = rules_it->second;
  if (!rules.any) return true;

  // 1. NOT NULL on the resulting row.
  for (const auto& col : rules.not_null_columns) {
    if (ValueAt(schema, new_row, col).is_null()) {
      if (err) {
        *err = std::string(kNotNullViolation) + " (" + schema.table_name + "." + col +
               " is NULL)";
      }
      return false;
    }
  }

  // 2. CHECK on the resulting row.
  for (const auto& check : rules.checks) {
    const Expr* expr = CheckExpression(schema.table_name, check.expression);
    if (expr == nullptr) continue;
    if (EvaluatePredicate(expr, new_row, schema) == Tribool::False) {
      if (err) {
        *err = std::string(kCheckViolation) + " (" + schema.table_name + ": " +
               check.expression + ")";
      }
      return false;
    }
  }

  // 3. Uniqueness only for keys that actually change (a row keeps its own key).
  for (size_t g = 0; g < rules.unique_groups.size(); ++g) {
    const auto& group = rules.unique_groups[g];
    std::string old_key;
    std::string new_key;
    const bool had_old = KeyOf(schema, group, old_row, &old_key);
    const bool has_new = KeyOf(schema, group, new_row, &new_key);
    if (!has_new || (had_old && old_key == new_key)) continue;
    std::unordered_map<std::string, uint32_t>* set = nullptr;
    if (!KeySetFor(schema.table_name, group, &set, err)) return false;
    if (set->find(new_key) != set->end()) {
      if (err) {
        const bool is_pk = (g == 0 && !schema.primary_key_columns.empty() &&
                            group == schema.primary_key_columns);
        *err = std::string(is_pk ? kPrimaryKeyViolation : kUniqueViolation) + " (" +
               schema.table_name + " (" + JoinColumns(group) + ")=" +
               ValuesText(ValuesOf(schema, group, new_row)) + " already exists)";
      }
      return false;
    }
  }

  // 4. FK values that change must exist in the parent table.
  for (const auto& edge : rules.child_edges) {
    const std::vector<Value> old_values = ValuesOf(schema, edge.child_columns, old_row);
    const std::vector<Value> new_values = ValuesOf(schema, edge.child_columns, new_row);
    if (SameValues(old_values, new_values)) continue;
    if (AnyNull(new_values)) continue;
    std::unordered_map<std::string, uint32_t>* set = nullptr;
    if (!KeySetFor(edge.parent_table, edge.parent_columns, &set, err)) return false;
    if (set->find(EncodeValues(new_values)) == set->end()) {
      if (err) {
        *err = std::string(kForeignKeyViolation) + " (" + schema.table_name + "." +
               JoinColumns(edge.child_columns) + "=" + ValuesText(new_values) +
               " not found in " + edge.parent_table + "." +
               JoinColumns(edge.parent_columns) + ")";
      }
      return false;
    }
  }
  return true;
}

void ConstraintChecker::RecordUpdate(const TableSchema& schema, const Tuple& old_row,
                                     const Tuple& new_row) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!enabled_) return;
  auto rules_it = rules_.find(LowerCopy(schema.table_name));
  if (rules_it == rules_.end()) return;

  // Keys this row had before and has after the update, paired by set id.
  std::unordered_map<std::string, std::string> before;
  std::unordered_map<std::string, std::string> after;
  for (const auto& row_key : KeysForRow(schema, rules_it->second, old_row)) {
    before[row_key.id] = row_key.key;
  }
  for (const auto& row_key : KeysForRow(schema, rules_it->second, new_row)) {
    after[row_key.id] = row_key.key;
  }
  for (const auto& entry : before) {
    auto set_it = key_sets_.find(entry.first);
    if (set_it == key_sets_.end()) continue;  // not cached: a later load sees it
    auto after_it = after.find(entry.first);
    if (after_it != after.end() && after_it->second == entry.second) continue;
    auto key_it = set_it->second.find(entry.second);
    if (key_it != set_it->second.end() && --key_it->second == 0) {
      set_it->second.erase(key_it);
    }
  }
  for (const auto& entry : after) {
    if (before.find(entry.first) != before.end() &&
        before.at(entry.first) == entry.second) {
      continue;  // unchanged: the decrement above was skipped too
    }
    auto set_it = key_sets_.find(entry.first);
    if (set_it == key_sets_.end()) continue;
    ++set_it->second[entry.second];
  }
}

// ------------------------------------------------------------- DELETE

bool ConstraintChecker::CheckDelete(const TableSchema& schema, const Tuple& row,
                                    std::string* err) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!enabled_) return true;
  auto rules_it = rules_.find(LowerCopy(schema.table_name));
  if (rules_it == rules_.end()) {
    rules_it = rules_.emplace(LowerCopy(schema.table_name), BuildRules(schema)).first;
  }
  const TableRules& rules = rules_it->second;
  if (!rules.any) return true;

  for (const auto& edge : rules.parent_edges) {
    if (edge.on_delete != ForeignKey::OnDelete::RESTRICT) continue;
    const std::vector<Value> key_values = ValuesOf(schema, edge.parent_columns, row);
    if (AnyNull(key_values)) continue;
    std::unordered_map<std::string, uint32_t>* set = nullptr;
    if (!KeySetFor(edge.child_table, edge.child_columns, &set, err)) return false;
    if (set->find(EncodeValues(key_values)) != set->end()) {
      if (err) {
        *err = std::string(kRestrictViolation) + " (" + schema.table_name + "." +
               JoinColumns(edge.parent_columns) + "=" + ValuesText(key_values) +
               " is referenced by " + edge.child_table + "." +
               JoinColumns(edge.child_columns) + ")";
      }
      return false;
    }
  }
  return true;
}

void ConstraintChecker::RecordDelete(const TableSchema& schema, const Tuple& row) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!enabled_) return;
  auto rules_it = rules_.find(LowerCopy(schema.table_name));
  if (rules_it == rules_.end()) return;
  for (const auto& row_key : KeysForRow(schema, rules_it->second, row)) {
    auto set_it = key_sets_.find(row_key.id);
    if (set_it == key_sets_.end()) continue;
    auto key_it = set_it->second.find(row_key.key);
    if (key_it != set_it->second.end() && --key_it->second == 0) {
      set_it->second.erase(key_it);
    }
  }
}

std::vector<ConstraintChecker::CascadeTarget> ConstraintChecker::CascadeTargets(
    const TableSchema& schema, const Tuple& row) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<CascadeTarget> targets;
  if (!enabled_) return targets;
  auto rules_it = rules_.find(LowerCopy(schema.table_name));
  if (rules_it == rules_.end()) {
    rules_it = rules_.emplace(LowerCopy(schema.table_name), BuildRules(schema)).first;
  }
  for (const auto& edge : rules_it->second.parent_edges) {
    if (edge.on_delete != ForeignKey::OnDelete::CASCADE) continue;
    const std::vector<Value> key_values = ValuesOf(schema, edge.parent_columns, row);
    if (AnyNull(key_values)) continue;
    CascadeTarget target;
    target.child_table = edge.child_table;
    target.child_columns = edge.child_columns;
    target.key = key_values;
    targets.push_back(std::move(target));
  }
  return targets;
}

bool ConstraintChecker::CascadeDelete(const TableSchema& schema, const Tuple& row,
                                      uint32_t txn_id, std::string* err) {
  // CascadeTargets locks mutex_ briefly; deleter_ re-enters the engine (and
  // therefore this checker), so it must run WITHOUT the lock held.
  const std::vector<CascadeTarget> targets = CascadeTargets(schema, row);
  for (const auto& target : targets) {
    if (err) err->clear();
    deleter_(target.child_table, target.child_columns, target.key, txn_id, err);
    if (err && !err->empty()) return false;
  }
  return true;
}

}  // namespace sthir
