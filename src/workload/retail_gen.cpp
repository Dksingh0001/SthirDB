#include "workload/retail_gen.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "exec/execution_engine.h"

namespace sthir {

namespace {

constexpr size_t kBatchRows = 100;

// --- deterministic randomness -------------------------------------------
// splitmix64 finalizer; combined with the seed and the row id it gives every
// generated row a value that depends only on (seed, id) — never on insertion
// order or on how batches happen to be cut — so two runs are identical.

uint64_t Mix(uint64_t x) {
  x ^= x >> 30;
  x *= 0xBF58476D1CE4E5B9ULL;
  x ^= x >> 27;
  x *= 0x94D049BB133111EBULL;
  x ^= x >> 31;
  return x;
}

struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed) {}
  uint64_t Next() {
    s += 0x9E3779B97F4A7C15ULL;
    return Mix(s);
  }
  uint32_t Below(uint32_t n) { return n == 0 ? 0 : static_cast<uint32_t>(Next() % n); }
};

uint64_t PurchaseState(uint64_t seed, uint32_t pid) {
  return Mix(seed ^ (0x9E3779B97F4A7C15ULL * (static_cast<uint64_t>(pid) + 1)));
}

// --- row shaping ----------------------------------------------------------

int64_t ItemPriceCents(uint32_t item_id) {
  return 100 + static_cast<int64_t>(item_id * 37u % 49900u);  // 1.00 .. 500.00
}

// Real calendar date as an int yyyymmdd: Jan 1 of `year` plus `off` days.
// The workload query filters on PurchaseDate, so the values must be genuine
// dates, not `20240101 + off` integer arithmetic.
bool IsLeap(int y) {
  return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

int DaysInMonth(int y, int m) {
  static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m == 2 && IsLeap(y)) return 29;
  return kDays[m - 1];
}

int DateAfter(int year, int off) {
  int y = year;
  int m = 1;
  while (true) {
    const int dim = DaysInMonth(y, m);
    if (off < dim) return y * 10000 + m * 100 + (off + 1);
    off -= dim;
    if (++m > 12) {
      m = 1;
      ++y;
    }
  }
}

std::string Money(int64_t cents) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%lld.%02lld", static_cast<long long>(cents / 100),
                static_cast<long long>(cents % 100));
  return buf;
}

const char* const kCategories[10] = {"Electronics", "Grocery",  "Apparel", "Home",    "Sports",
                                     "Toys",        "Beauty",   "Books",   "Garden",  "Auto"};
const char* const kCities[4] = {"Paris", "Rome", "Oslo", "Lisbon"};
const char* const kCities2[4] = {"Berlin", "Madrid", "Vienna", "Prague"};
const char* const kCountries[8] = {"DE", "FR", "IT", "ES", "NL", "SE", "PL", "CZ"};

const char* const kRetailTables[8] = {"Staff",     "Shoppers",            "Vendors",
                                      "Items",     "Purchases",           "Invoices",
                                      "Purchase_Line_Items", "Item_Vendor_Catalogs"};

// --- statement helpers ----------------------------------------------------

std::string Truncate(const std::string& s, size_t n = 160) {
  return s.size() <= n ? s : s.substr(0, n) + "...";
}

struct Batch {
  std::string table;
  std::string sql;
  size_t n = 0;

  explicit Batch(std::string t) : table(std::move(t)) { Reset(); }
  void Reset() {
    sql = "INSERT INTO " + table + " VALUES";
    n = 0;
  }
  void Add(const std::string& tuple) {
    sql += (n == 0 ? " " : ",");
    sql += tuple;
    ++n;
  }
};

bool Flush(ExecutionEngine* engine, Batch* b, std::string* err) {
  if (b->n == 0) return true;
  const auto res = engine->Execute(b->sql);
  if (!res.success) {
    if (err) *err = res.error_message + " | statement: " + Truncate(b->sql);
    return false;
  }
  b->Reset();
  return true;
}

bool Exec(ExecutionEngine* engine, const std::string& sql, std::string* err) {
  const auto res = engine->Execute(sql);
  if (res.success) return true;
  if (err) *err = res.error_message + " | statement: " + Truncate(sql);
  return false;
}

bool TableEmpty(ExecutionEngine* engine, const char* table, bool* out, std::string* err) {
  const auto res = engine->Execute(std::string("SELECT COUNT(*) FROM ") + table);
  if (!res.success) {
    if (err) *err = res.error_message + " | cannot count table " + table;
    return false;
  }
  *out = res.rows.size() == 1 && res.rows[0].size() == 1 && res.rows[0][0] == "0";
  return true;
}

// One PRIMARY KEY / UNIQUE declaration, one constraint-backed index.
// INT/DECIMAL only: the B+tree is int64-keyed (IndexKeyFromInt/Decimal), so the
// VARCHAR UNIQUE(Email) constraints are enforced by the constraint checker but
// are not index-backed — documented as a limitation in docs/report.md §9.4.
struct IndexSpec {
  const char* name;
  const char* table;
  const char* column;
};

const IndexSpec kConstraintIndexes[] = {
    {"idx_staff_pk", "Staff", "StaffID"},
    {"idx_shoppers_pk", "Shoppers", "ShopperID"},
    {"idx_vendors_pk", "Vendors", "VendorID"},
    {"idx_items_pk", "Items", "ItemID"},
    {"idx_purchases_pk", "Purchases", "PurchaseID"},
    {"idx_invoices_pk", "Invoices", "InvoiceID"},
    {"idx_invoices_purchase", "Invoices", "PurchaseID"},
    {"idx_pli_pk", "Purchase_Line_Items", "LineItemID"},
    {"idx_ivc_pk", "Item_Vendor_Catalogs", "CatalogID"},
};

bool ConstraintsFlagOff(const std::string& val) {
  return val == "off" || val == "0" || val == "false" || val == "no";
}

// A line item drawn from (seed, purchase id): item, quantity, line total.
struct LineGen {
  uint32_t item = 0;
  uint32_t qty = 0;
  int64_t total_cents = 0;
};

std::vector<LineGen> LinesFor(const RetailGenConfig& cfg, uint32_t pid) {
  Rng rng(PurchaseState(cfg.seed, pid));
  std::vector<LineGen> lines;
  lines.reserve(cfg.line_items_per_purchase);
  for (uint32_t j = 0; j < cfg.line_items_per_purchase; ++j) {
    LineGen ln;
    ln.item = 1 + rng.Below(cfg.items);
    ln.qty = 1 + rng.Below(5);  // CHECK (Quantity >= 1): always valid
    ln.total_cents = static_cast<int64_t>(ln.qty) * ItemPriceCents(ln.item);
    lines.push_back(ln);
  }
  return lines;
}

}  // namespace

bool GenerateRetailData(ExecutionEngine* engine, const RetailGenConfig& cfg,
                        RetailGenResult* out, std::string* err) {
  if (engine == nullptr) {
    if (err) *err = "null engine";
    return false;
  }
  const auto t0 = std::chrono::steady_clock::now();

  // Refuse to append to a database that already has retail data: PK/UNIQUE
  // enforcement would fail the load halfway, and without enforcement it would
  // silently produce duplicates.
  for (const char* table : kRetailTables) {
    bool empty = false;
    if (!TableEmpty(engine, table, &empty, err)) return false;
    if (!empty) {
      if (err) *err = std::string("table '") + table + "' is not empty; start from a clean ./data/";
      return false;
    }
  }

  size_t total = 0;
  auto record = [&](const std::string& table, size_t rows) {
    out->rows_by_table.emplace_back(table, rows);
    total += rows;
  };

  // --- dimension tables (formula-driven, no shared random stream) ---------
  {
    Batch b("Staff");
    for (uint32_t i = 1; i <= cfg.staff; ++i) {
      b.Add("(" + std::to_string(i) + ",'Staff " + std::to_string(i) + "','staff" +
            std::to_string(i) + "@retail.test')");
    }
    if (!Flush(engine, &b, err)) return false;
    record("Staff", cfg.staff);
  }
  {
    Batch b("Vendors");
    for (uint32_t i = 1; i <= cfg.vendors; ++i) {
      b.Add("(" + std::to_string(i) + ",'Vendor " + std::to_string(i) + "','" +
            kCountries[(i - 1) % 8] + "')");
    }
    if (!Flush(engine, &b, err)) return false;
    record("Vendors", cfg.vendors);
  }
  {
    Batch b("Shoppers");
    for (uint32_t i = 1; i <= cfg.shoppers; ++i) {
      const char* city = ((i - 1) % 2 == 0) ? kCities[(i - 1) % 4] : kCities2[(i - 1) % 4];
      b.Add("(" + std::to_string(i) + ",'Shopper " + std::to_string(i) + "','shopper" +
            std::to_string(i) + "@retail.test','" + city + "'," +
            std::to_string(DateAfter(2020, static_cast<int>(i % 365))) + ")");
    }
    if (!Flush(engine, &b, err)) return false;
    record("Shoppers", cfg.shoppers);
  }
  {
    Batch b("Items");
    for (uint32_t i = 1; i <= cfg.items; ++i) {
      b.Add("(" + std::to_string(i) + ",'Item " + std::to_string(i) + "','" +
            kCategories[(i - 1) % 10] + "'," + Money(ItemPriceCents(i)) + "," +
            std::to_string((i * 13u) % 1000u) + ")");
    }
    if (!Flush(engine, &b, err)) return false;
    record("Items", cfg.items);
  }

  // --- transactions: purchases and their line items in one pass ----------
  {
    Batch pb("Purchases");
    Batch lb("Purchase_Line_Items");
    std::vector<int64_t> totals(cfg.purchases + 1, 0);
    size_t purchase_rows = 0;
    size_t line_rows = 0;
    uint64_t line_id = 1;

    for (uint32_t pid = 1; pid <= cfg.purchases; ++pid) {
      const auto lines = LinesFor(cfg, pid);
      int64_t total = 0;
      for (const auto& ln : lines) total += ln.total_cents;
      totals[pid] = total;

      Rng rng(PurchaseState(cfg.seed, pid));
      const uint32_t shopper = 1 + rng.Below(cfg.shoppers);
      const uint32_t staff = 1 + rng.Below(cfg.staff);
      const int date = DateAfter(2024, static_cast<int>((static_cast<uint64_t>(pid) * 7) % 730));

      pb.Add("(" + std::to_string(pid) + "," + std::to_string(shopper) + "," +
             std::to_string(staff) + "," + std::to_string(date) + "," + Money(total) + ")");
      ++purchase_rows;
      for (const auto& ln : lines) {
        lb.Add("(" + std::to_string(line_id++) + "," + std::to_string(pid) + "," +
               std::to_string(ln.item) + "," + std::to_string(ln.qty) + "," +
               Money(ItemPriceCents(ln.item)) + "," + Money(ln.total_cents) + ")");
        ++line_rows;
      }

      // Flush purchases *before* the line items that reference them.
      if (pb.n >= kBatchRows && !Flush(engine, &pb, err)) return false;
      if (lb.n >= kBatchRows * cfg.line_items_per_purchase && !Flush(engine, &lb, err))
        return false;
    }
    if (!Flush(engine, &pb, err)) return false;
    if (!Flush(engine, &lb, err)) return false;
    record("Purchases", purchase_rows);
    record("Purchase_Line_Items", line_rows);

    // --- invoices (one per purchase, total copied from the purchase) ------
    Batch ib("Invoices");
    size_t invoice_rows = 0;
    for (uint32_t pid = 1; pid <= cfg.purchases; ++pid) {
      const int issued = DateAfter(2024, static_cast<int>((static_cast<uint64_t>(pid) * 7) % 730) + 1);
      ib.Add("(" + std::to_string(1000000 + pid) + "," + std::to_string(pid) + "," +
             Money(totals[pid]) + "," + std::to_string(issued) + ")");
      ++invoice_rows;
      if (ib.n >= kBatchRows && !Flush(engine, &ib, err)) return false;
    }
    if (!Flush(engine, &ib, err)) return false;
    record("Invoices", invoice_rows);
  }

  // --- vendor catalogs: two suppliers per item ---------------------------
  {
    Batch b("Item_Vendor_Catalogs");
    uint64_t cid = 1;
    size_t rows = 0;
    for (uint32_t i = 1; i <= cfg.items; ++i) {
      const int64_t price = ItemPriceCents(i);
      const uint32_t v1 = 1 + ((i - 1) % cfg.vendors);
      const uint32_t v2 = 1 + ((i * 7u) % cfg.vendors);
      b.Add("(" + std::to_string(cid++) + "," + std::to_string(i) + "," + std::to_string(v1) +
            "," + Money(price * 80 / 100) + "," + std::to_string(1 + (i % 50)) + ")");
      ++rows;
      b.Add("(" + std::to_string(cid++) + "," + std::to_string(i) + "," + std::to_string(v2) +
            "," + Money(price * 75 / 100) + "," + std::to_string(1 + ((i * 3u) % 50)) + ")");
      ++rows;
      if (b.n >= kBatchRows && !Flush(engine, &b, err)) return false;
    }
    if (!Flush(engine, &b, err)) return false;
    record("Item_Vendor_Catalogs", rows);
  }

  // --- statistics for the cost model -------------------------------------
  if (cfg.analyze) {
    for (const char* table : kRetailTables) {
      if (!Exec(engine, std::string("ANALYZE ") + table, err)) return false;
    }
  }

  // --- constraint-backed indexes -----------------------------------------
  const bool constraints_on =
      !ConstraintsFlagOff(engine->GetSessionFlag("constraints_enabled"));
  if (cfg.create_indexes && constraints_on) {
    for (const IndexSpec& spec : kConstraintIndexes) {
      const std::string sql = std::string("CREATE INDEX ") + spec.name + " ON " + spec.table +
                               "(" + spec.column + ")";
      if (!Exec(engine, sql, err)) return false;
      ++out->indexes_created;
    }
  }

  out->total_rows = total;
  out->elapsed_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  return true;
}

}  // namespace sthir