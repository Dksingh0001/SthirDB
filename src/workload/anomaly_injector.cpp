#include "workload/anomaly_injector.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "exec/execution_engine.h"

namespace sthir {

namespace {

// Facts about the loaded database, read once up front: they size the rate
// mode and parameterise the generated statements.
struct Context {
  size_t shoppers = 0;
  size_t items = 0;
  size_t purchases = 0;
  size_t line_items = 0;
  std::vector<size_t> referenced_shoppers;  // shoppers that purchases reference
};

bool ScalarCount(ExecutionEngine* engine, const std::string& sql, size_t* out, std::string* err) {
  const auto res = engine->Execute(sql);
  if (!res.success) {
    if (err) *err = res.error_message + " | " + sql;
    return false;
  }
  if (res.rows.size() != 1 || res.rows[0].size() != 1) {
    if (err) *err = "unexpected result shape for " + sql;
    return false;
  }
  *out = std::strtoul(res.rows[0][0].c_str(), nullptr, 10);
  return true;
}

bool LoadContext(ExecutionEngine* engine, Context* ctx, std::string* err) {
  if (!ScalarCount(engine, "SELECT COUNT(*) FROM Shoppers", &ctx->shoppers, err)) return false;
  if (!ScalarCount(engine, "SELECT COUNT(*) FROM Items", &ctx->items, err)) return false;
  if (!ScalarCount(engine, "SELECT COUNT(*) FROM Purchases", &ctx->purchases, err)) return false;
  if (!ScalarCount(engine, "SELECT COUNT(*) FROM Purchase_Line_Items", &ctx->line_items, err))
    return false;
  if (ctx->shoppers == 0 || ctx->items == 0 || ctx->purchases == 0) {
    if (err) *err = "database looks empty; run `sthirdb retail-gen` first";
    return false;
  }

  // Deterministic list of shoppers that at least one purchase references —
  // the only ones whose deletion can hit ON DELETE RESTRICT.
  const auto res = engine->Execute("SELECT ShopperID FROM Purchases");
  if (!res.success) {
    if (err) *err = res.error_message;
    return false;
  }
  std::set<size_t> unique;
  for (const auto& row : res.rows) {
    if (!row.empty()) unique.insert(std::strtoul(row[0].c_str(), nullptr, 10));
  }
  ctx->referenced_shoppers.assign(unique.begin(), unique.end());
  return true;
}

// --- the anomaly classes --------------------------------------------------
//
// Fresh-id ranges are disjoint per class so that statements from different
// classes cannot collide with each other even when the unconstrained run
// accepts them:
//   shoppers 700000+i (dup email)  710000+i (null email)  1..n (dup PK)
//   purchases 900000+i (missing FK)
//   line items 800000+i (zero qty)  810000+i (orphan)

std::string MakeStatement(const std::string& name, size_t i, const Context& ctx) {
  const auto S = [](size_t v) { return std::to_string(v); };

  if (name == "duplicate_email") {
    const size_t victim = 1 + (i % ctx.shoppers);
    return "INSERT INTO Shoppers VALUES (" + S(700000 + i) + ",'Ghost " + S(i) + "','shopper" +
           S(victim) + "@retail.test','Nowhere',20240101)";
  }
  if (name == "negative_stock") {
    const size_t item = 1 + (i % ctx.items);
    return "UPDATE Items SET StockLevel = -" + S(1 + (i % 99)) + " WHERE ItemID = " + S(item);
  }
  if (name == "missing_fk") {
    return "INSERT INTO Purchases VALUES (" + S(900000 + i) + ",999999,1,20240101,10.00)";
  }
  if (name == "zero_quantity") {
    const size_t purchase = 1 + (i % ctx.purchases);
    const size_t item = 1 + (i % ctx.items);
    const long qty = (i % 2 == 0) ? 0 : -1;
    return "INSERT INTO Purchase_Line_Items VALUES (" + S(800000 + i) + "," + S(purchase) + "," +
           S(item) + "," + std::to_string(qty) + ",10.00,0.00)";
  }
  if (name == "duplicate_pk") {
    return "INSERT INTO Shoppers VALUES (" + S(1 + (i % ctx.shoppers)) + ",'Impostor " + S(i) +
           "','impostor" + S(i) + "@retail.test','Elsewhere',20240101)";
  }
  if (name == "orphan_line_item") {
    const size_t item = 1 + (i % ctx.items);
    return "INSERT INTO Purchase_Line_Items VALUES (" + S(810000 + i) + "," + S(950000 + i) + "," +
           S(item) + ",1,5.00,5.00)";
  }
  if (name == "null_not_null") {
    return "INSERT INTO Shoppers VALUES (" + S(710000 + i) + ",'NoEmail " + S(i) +
           "',NULL,'Nowhere',20240101)";
  }
  if (name == "restrict_delete") {
    const size_t shopper = ctx.referenced_shoppers[i % ctx.referenced_shoppers.size()];
    return "DELETE FROM Shoppers WHERE ShopperID = " + S(shopper);
  }
  return "";
}

struct ClassMeta {
  const char* name;
  const char* constraint;
  const char* target_table;
};

const ClassMeta kYuanClasses[] = {
    {"duplicate_email", "UNIQUE violation", "Shoppers"},
    {"negative_stock", "CHECK violation", "Items"},
    {"missing_fk", "FOREIGN KEY violation", "Purchases"},
};

const ClassMeta kExtendedClasses[] = {
    {"zero_quantity", "CHECK violation", "Purchase_Line_Items"},
    {"duplicate_pk", "PRIMARY KEY violation", "Shoppers"},
    {"orphan_line_item", "FOREIGN KEY violation", "Purchase_Line_Items"},
    {"null_not_null", "NOT NULL violation", "Shoppers"},
    {"restrict_delete", "ON DELETE RESTRICT violation", "Shoppers"},
};

const ClassMeta* FindMeta(const std::string& name) {
  for (const auto& m : kYuanClasses)
    if (name == m.name) return &m;
  for (const auto& m : kExtendedClasses)
    if (name == m.name) return &m;
  return nullptr;
}

std::unordered_map<std::string, size_t> TargetRows(const Context& ctx) {
  return {{"Shoppers", ctx.shoppers},
          {"Items", ctx.items},
          {"Purchases", ctx.purchases},
          {"Purchase_Line_Items", ctx.line_items}};
}

bool BuildClasses(const InjectConfig& cfg, const Context& ctx,
                  std::vector<InjectionClassDef>* out, std::string* err) {
  if (!cfg.classes.empty()) {
    for (const auto& c : cfg.classes) {
      if (FindMeta(c.name) == nullptr) {
        if (err) *err = "unknown anomaly class '" + c.name + "'";
        return false;
      }
      InjectionClassDef def = c;
      const ClassMeta* m = FindMeta(c.name);
      def.constraint = m->constraint;
      def.target_table = m->target_table;
      out->push_back(std::move(def));
    }
  } else {
    for (const auto& m : kYuanClasses)
      out->push_back({m.name, m.constraint, m.target_table, cfg.default_count});
    if (cfg.extended) {
      for (const auto& m : kExtendedClasses)
        out->push_back({m.name, m.constraint, m.target_table, cfg.default_count});
    }
  }

  if (cfg.rate_percent > 0) {
    const auto rows = TargetRows(ctx);
    for (auto& def : *out) {
      const auto it = rows.find(def.target_table);
      if (it == rows.end()) {
        if (err) *err = "rate mode: unknown target table " + def.target_table;
        return false;
      }
      def.count = std::max<size_t>(1, it->second * static_cast<size_t>(cfg.rate_percent) / 100);
    }
  }
  return true;
}

}  // namespace

bool RunAnomalyInjector(ExecutionEngine* engine, const InjectConfig& cfg, InjectResult* out,
                        std::string* err) {
  if (engine == nullptr) {
    if (err) *err = "null engine";
    return false;
  }
  const auto t0 = std::chrono::steady_clock::now();

  Context ctx;
  if (!LoadContext(engine, &ctx, err)) return false;

  std::vector<InjectionClassDef> defs;
  if (!BuildClasses(cfg, ctx, &defs, err)) return false;

  for (const auto& def : defs) {
    InjectionClassResult r;
    r.name = def.name;
    r.constraint = def.constraint;
    for (size_t i = 0; i < def.count; ++i) {
      const std::string sql = MakeStatement(def.name, i, ctx);
      if (sql.empty()) {
        if (err) *err = "no statement builder for class '" + def.name + "'";
        return false;
      }
      const auto res = engine->Execute(sql);
      ++r.injected;
      if (res.success) {
        ++r.accepted;
      } else {
        ++r.blocked;
        if (res.error_message.find(def.constraint) != std::string::npos) {
          if (r.first_block.empty()) r.first_block = res.error_message;
        } else {
          ++r.wrong_constraint;
          if (r.first_block.empty()) r.first_block = res.error_message;
        }
      }
    }
    out->injected += r.injected;
    out->blocked += r.blocked;
    out->accepted += r.accepted;
    out->classes.push_back(std::move(r));
  }

  out->elapsed_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return true;
}

std::string FormatInjectionTable(const InjectResult& result, const std::string& mode_label) {
  std::string out = "### Anomaly interception — " + mode_label + "\n\n";
  out += "| class | constraint | injected | blocked | accepted | interception % |\n";
  out += "|---|---|---:|---:|---:|---:|\n";

  auto row = [&out](const std::string& name, const std::string& constraint, size_t injected,
                    size_t blocked, size_t accepted) {
    const double rate = injected ? 100.0 * static_cast<double>(blocked) / static_cast<double>(injected) : 0.0;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", rate);
    out += "| " + name + " | " + constraint + " | " + std::to_string(injected) + " | " +
           std::to_string(blocked) + " | " + std::to_string(accepted) + " | " + buf + "% |\n";
  };

  for (const auto& r : result.classes) row(r.name, r.constraint, r.injected, r.blocked, r.accepted);
  row("TOTAL", "-", result.injected, result.blocked, result.accepted);

  for (const auto& r : result.classes) {
    if (r.wrong_constraint > 0) {
      out += "\n> note: " + std::to_string(r.wrong_constraint) + " of the " +
             std::to_string(r.blocked) + " blocks for `" + r.name +
             "` were rejected by a different constraint; first message: " + r.first_block + "\n";
    }
  }
  char ms[32];
  std::snprintf(ms, sizeof(ms), "%.0f", result.elapsed_ms);
  out += std::string("\n_generated in ") + ms + " ms_\n";
  return out;
}

}  // namespace sthir