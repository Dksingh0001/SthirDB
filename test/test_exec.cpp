#include "exec/executors.h"
#include "exec/expr_eval.h"
#include "exec/value.h"

#include <filesystem>
#include <string>
#include <vector>

#include "test_macros.h"

namespace fs = std::filesystem;

namespace {

void test_3vl_logic() {
  sthir::Value v_int5 = sthir::Value::MakeInt(5);
  sthir::Value v_int10 = sthir::Value::MakeInt(10);
  sthir::Value v_null = sthir::Value::MakeNull(sthir::DataType::INT);

  // Normal comparisons
  CHECK_EQ(static_cast<int>(v_int5.Compare(sthir::BinaryOp::LT, v_int10)),
           static_cast<int>(sthir::Tribool::True));
  CHECK_EQ(static_cast<int>(v_int5.Compare(sthir::BinaryOp::EQ, v_int10)),
           static_cast<int>(sthir::Tribool::False));

  // Comparisons with NULL must yield UNKNOWN
  CHECK_EQ(static_cast<int>(v_int5.Compare(sthir::BinaryOp::EQ, v_null)),
           static_cast<int>(sthir::Tribool::Unknown));
  CHECK_EQ(static_cast<int>(v_int5.Compare(sthir::BinaryOp::LT, v_null)),
           static_cast<int>(sthir::Tribool::Unknown));
  CHECK_EQ(static_cast<int>(v_null.Compare(sthir::BinaryOp::EQ, v_null)),
           static_cast<int>(sthir::Tribool::Unknown));

  // 3VL AND / OR / NOT
  CHECK_EQ(static_cast<int>(sthir::TriboolAnd(sthir::Tribool::True, sthir::Tribool::Unknown)),
           static_cast<int>(sthir::Tribool::Unknown));
  CHECK_EQ(static_cast<int>(sthir::TriboolAnd(sthir::Tribool::False, sthir::Tribool::Unknown)),
           static_cast<int>(sthir::Tribool::False));
  CHECK_EQ(static_cast<int>(sthir::TriboolOr(sthir::Tribool::True, sthir::Tribool::Unknown)),
           static_cast<int>(sthir::Tribool::True));
  CHECK_EQ(static_cast<int>(sthir::TriboolOr(sthir::Tribool::False, sthir::Tribool::Unknown)),
           static_cast<int>(sthir::Tribool::Unknown));
  CHECK_EQ(static_cast<int>(sthir::TriboolNot(sthir::Tribool::Unknown)),
           static_cast<int>(sthir::Tribool::Unknown));

  test_passed("3vl_logic");
}

void test_tuple_serialization_roundtrip() {
  sthir::TableSchema schema;
  schema.table_name = "T";
  schema.columns = {
      {"id", sthir::DataType::INT, 4, true, true, false},
      {"name", sthir::DataType::VARCHAR, 50, false, false, false},
      {"price", sthir::DataType::DECIMAL, 8, false, false, false},
      {"note", sthir::DataType::VARCHAR, 100, false, false, false}
  };

  sthir::Tuple t1({
      sthir::Value::MakeInt(42),
      sthir::Value::MakeVarchar("Special Widget"),
      sthir::Value::MakeDecimal(19.95),
      sthir::Value::MakeNull(sthir::DataType::VARCHAR)
  });

  auto bytes = t1.Serialize(schema);
  sthir::Tuple t2 = sthir::Tuple::Deserialize(bytes.data(), static_cast<uint16_t>(bytes.size()), schema);

  CHECK_EQ(t2.size(), 4u);
  CHECK_EQ(t2.GetValue(0).as_int(), 42);
  CHECK_EQ(t2.GetValue(1).as_string(), "Special Widget");
  CHECK(std::abs(t2.GetValue(2).as_decimal() - 19.95) < 1e-6);
  CHECK(t2.GetValue(3).is_null());

  test_passed("tuple_serialization_roundtrip");
}

class MockExecutor : public sthir::Executor {
 public:
  MockExecutor(sthir::TableSchema schema, std::vector<sthir::Tuple> tuples)
      : schema_(std::move(schema)), tuples_(std::move(tuples)) {}

  void init() override { idx_ = 0; }
  std::optional<sthir::Tuple> next() override {
    if (idx_ < tuples_.size()) return tuples_[idx_++];
    return std::nullopt;
  }
  void close() override { idx_ = 0; }
  const sthir::TableSchema& output_schema() const override { return schema_; }

 private:
  sthir::TableSchema schema_;
  std::vector<sthir::Tuple> tuples_;
  size_t idx_ = 0;
};

void test_filter_and_project_executors() {
  sthir::TableSchema schema;
  schema.table_name = "Items";
  schema.columns = {
      {"id", sthir::DataType::INT, 4, false, false, false},
      {"price", sthir::DataType::DECIMAL, 8, false, false, false}};

  std::vector<sthir::Tuple> data = {
      sthir::Tuple({sthir::Value::MakeInt(1), sthir::Value::MakeDecimal(3.50)}),
      sthir::Tuple({sthir::Value::MakeInt(2), sthir::Value::MakeDecimal(12.00)}),
      sthir::Tuple({sthir::Value::MakeInt(3), sthir::Value::MakeNull(sthir::DataType::DECIMAL)}),
      sthir::Tuple({sthir::Value::MakeInt(4), sthir::Value::MakeDecimal(8.50)})
  };

  auto mock = std::make_unique<MockExecutor>(schema, data);

  // WHERE price > 5.0  (NULL row must be skipped!)
  auto pred = std::make_unique<sthir::BinaryExpr>(
      sthir::BinaryOp::GT,
      std::make_unique<sthir::ColumnRefExpr>("", "price"),
      sthir::LiteralExpr::MakeDecimal(5.0));

  auto filter = std::make_unique<sthir::FilterExecutor>(std::move(mock), std::move(pred));

  // Project: id
  sthir::TableSchema proj_schema;
  proj_schema.table_name = "Items";
  proj_schema.columns = {{"id", sthir::DataType::INT, 4, false, false, false}};

  std::vector<std::pair<std::unique_ptr<sthir::Expr>, std::string>> projs;
  projs.push_back({std::make_unique<sthir::ColumnRefExpr>("", "id"), "id"});

  auto project = std::make_unique<sthir::ProjectExecutor>(
      std::move(filter), proj_schema, std::move(projs));

  project->init();
  auto r1 = project->next();
  CHECK(r1.has_value());
  CHECK_EQ(r1->GetValue(0).as_int(), 2);

  auto r2 = project->next();
  CHECK(r2.has_value());
  CHECK_EQ(r2->GetValue(0).as_int(), 4);

  auto r3 = project->next();
  CHECK(!r3.has_value());
  project->close();

  test_passed("filter_and_project_executors");
}

void test_aggregate_executor() {
  sthir::TableSchema schema;
  schema.table_name = "Sales";
  schema.columns = {
      {"category", sthir::DataType::VARCHAR, 20, false, false, false},
      {"amount", sthir::DataType::DECIMAL, 8, false, false, false}};

  std::vector<sthir::Tuple> data = {
      sthir::Tuple({sthir::Value::MakeVarchar("A"), sthir::Value::MakeDecimal(10.0)}),
      sthir::Tuple({sthir::Value::MakeVarchar("B"), sthir::Value::MakeDecimal(20.0)}),
      sthir::Tuple({sthir::Value::MakeVarchar("A"), sthir::Value::MakeNull(sthir::DataType::DECIMAL)}),
      sthir::Tuple({sthir::Value::MakeVarchar("A"), sthir::Value::MakeDecimal(30.0)}),
      sthir::Tuple({sthir::Value::MakeVarchar("B"), sthir::Value::MakeDecimal(40.0)})
  };

  auto mock = std::make_unique<MockExecutor>(schema, data);

  // GROUP BY category: COUNT(*), COUNT(amount), SUM(amount), AVG(amount), MIN(amount), MAX(amount)
  std::vector<sthir::AggEntry> aggs;
  aggs.push_back({sthir::AggFunc::COUNT, true, nullptr, "cnt_star"});
  aggs.push_back({sthir::AggFunc::COUNT, false, std::make_unique<sthir::ColumnRefExpr>("", "amount"), "cnt_amt"});
  aggs.push_back({sthir::AggFunc::SUM, false, std::make_unique<sthir::ColumnRefExpr>("", "amount"), "sum_amt"});
  aggs.push_back({sthir::AggFunc::AVG, false, std::make_unique<sthir::ColumnRefExpr>("", "amount"), "avg_amt"});
  aggs.push_back({sthir::AggFunc::MIN, false, std::make_unique<sthir::ColumnRefExpr>("", "amount"), "min_amt"});
  aggs.push_back({sthir::AggFunc::MAX, false, std::make_unique<sthir::ColumnRefExpr>("", "amount"), "max_amt"});

  sthir::TableSchema out_schema;
  out_schema.columns = {
      {"category", sthir::DataType::VARCHAR, 20, false, false, false},
      {"cnt_star", sthir::DataType::INT, 4, false, false, false},
      {"cnt_amt", sthir::DataType::INT, 4, false, false, false},
      {"sum_amt", sthir::DataType::DECIMAL, 8, false, false, false},
      {"avg_amt", sthir::DataType::DECIMAL, 8, false, false, false},
      {"min_amt", sthir::DataType::DECIMAL, 8, false, false, false},
      {"max_amt", sthir::DataType::DECIMAL, 8, false, false, false}};

  auto agg_exec = std::make_unique<sthir::AggregateExecutor>(
      std::move(mock), std::move(aggs), std::vector<std::string>{"category"}, out_schema);

  agg_exec->init();
  std::vector<sthir::Tuple> res;
  while (auto opt = agg_exec->next()) {
    res.push_back(std::move(*opt));
  }
  agg_exec->close();

  CHECK_EQ(res.size(), 2u);
  // Group A: 3 rows, 2 non-null amounts (10, 30): sum = 40, avg = 20, min = 10, max = 30
  // Group B: 2 rows, 2 amounts (20, 40): sum = 60, avg = 30, min = 20, max = 40
  for (const auto& r : res) {
    if (r.GetValue(0).as_string() == "A") {
      CHECK_EQ(r.GetValue(1).as_int(), 3);  // COUNT(*)
      CHECK_EQ(r.GetValue(2).as_int(), 2);  // COUNT(amount) skips NULL
      CHECK(std::abs(r.GetValue(3).as_decimal() - 40.0) < 1e-6); // SUM
      CHECK(std::abs(r.GetValue(4).as_decimal() - 20.0) < 1e-6); // AVG
      CHECK(std::abs(r.GetValue(5).as_decimal() - 10.0) < 1e-6); // MIN
      CHECK(std::abs(r.GetValue(6).as_decimal() - 30.0) < 1e-6); // MAX
    } else if (r.GetValue(0).as_string() == "B") {
      CHECK_EQ(r.GetValue(1).as_int(), 2);  // COUNT(*)
      CHECK_EQ(r.GetValue(2).as_int(), 2);  // COUNT(amount)
      CHECK(std::abs(r.GetValue(3).as_decimal() - 60.0) < 1e-6); // SUM
      CHECK(std::abs(r.GetValue(4).as_decimal() - 30.0) < 1e-6); // AVG
    }
  }

  // Scalar aggregate over empty set
  auto empty_mock = std::make_unique<MockExecutor>(schema, std::vector<sthir::Tuple>{});
  std::vector<sthir::AggEntry> scalar_aggs;
  scalar_aggs.push_back({sthir::AggFunc::COUNT, true, nullptr, "cnt"});
  scalar_aggs.push_back({sthir::AggFunc::SUM, false, std::make_unique<sthir::ColumnRefExpr>("", "amount"), "sum"});

  sthir::TableSchema scalar_schema;
  scalar_schema.columns = {
      {"cnt", sthir::DataType::INT, 4, false, false, false},
      {"sum", sthir::DataType::DECIMAL, 8, false, false, false}};

  auto scalar_exec = std::make_unique<sthir::AggregateExecutor>(
      std::move(empty_mock), std::move(scalar_aggs), std::vector<std::string>{}, scalar_schema);

  scalar_exec->init();
  auto scalar_res = scalar_exec->next();
  CHECK(scalar_res.has_value());
  CHECK_EQ(scalar_res->GetValue(0).as_int(), 0);  // COUNT(*) on empty returns 0
  CHECK(scalar_res->GetValue(1).is_null());       // SUM on empty returns NULL
  CHECK(!scalar_exec->next().has_value());
  scalar_exec->close();

  test_passed("aggregate_executor");
}

void test_sort_executor() {
  sthir::TableSchema schema;
  schema.table_name = "T";
  schema.columns = {
      {"id", sthir::DataType::INT, 4, false, false, false},
      {"val", sthir::DataType::INT, 4, false, false, false}};

  std::vector<sthir::Tuple> data = {
      sthir::Tuple({sthir::Value::MakeInt(1), sthir::Value::MakeInt(20)}),
      sthir::Tuple({sthir::Value::MakeInt(2), sthir::Value::MakeInt(10)}),
      sthir::Tuple({sthir::Value::MakeInt(3), sthir::Value::MakeInt(30)})
  };

  auto mock = std::make_unique<MockExecutor>(schema, data);
  std::vector<sthir::OrderByItem> items = {{"val", true}};  // DESC

  auto sort_exec = std::make_unique<sthir::SortExecutor>(std::move(mock), items, schema);
  sort_exec->init();

  auto t1 = sort_exec->next();
  CHECK(t1.has_value());
  CHECK_EQ(t1->GetValue(1).as_int(), 30);

  auto t2 = sort_exec->next();
  CHECK(t2.has_value());
  CHECK_EQ(t2->GetValue(1).as_int(), 20);

  auto t3 = sort_exec->next();
  CHECK(t3.has_value());
  CHECK_EQ(t3->GetValue(1).as_int(), 10);

  CHECK(!sort_exec->next().has_value());
  sort_exec->close();

  test_passed("sort_executor");
}

void test_nested_loop_join_executor() {
  sthir::TableSchema s_left;
  s_left.table_name = "L";
  s_left.columns = {
      {"l_id", sthir::DataType::INT, 4, false, false, false},
      {"l_val", sthir::DataType::VARCHAR, 20, false, false, false}};

  sthir::TableSchema s_right;
  s_right.table_name = "R";
  s_right.columns = {
      {"r_id", sthir::DataType::INT, 4, false, false, false},
      {"r_val", sthir::DataType::VARCHAR, 20, false, false, false}};

  std::vector<sthir::Tuple> left_data = {
      sthir::Tuple({sthir::Value::MakeInt(1), sthir::Value::MakeVarchar("One")}),
      sthir::Tuple({sthir::Value::MakeInt(2), sthir::Value::MakeVarchar("Two")}),
      sthir::Tuple({sthir::Value::MakeInt(3), sthir::Value::MakeVarchar("Three")})
  };

  std::vector<sthir::Tuple> right_data = {
      sthir::Tuple({sthir::Value::MakeInt(1), sthir::Value::MakeVarchar("Match1")}),
      sthir::Tuple({sthir::Value::MakeInt(1), sthir::Value::MakeVarchar("Match1_dup")}),
      sthir::Tuple({sthir::Value::MakeInt(3), sthir::Value::MakeVarchar("Match3")})
  };

  sthir::TableSchema joined_schema;
  joined_schema.table_name = "Joined";
  joined_schema.columns = {
      {"l_id", sthir::DataType::INT, 4, false, false, false},
      {"l_val", sthir::DataType::VARCHAR, 20, false, false, false},
      {"r_id", sthir::DataType::INT, 4, false, false, false},
      {"r_val", sthir::DataType::VARCHAR, 20, false, false, false}
  };

  auto left_mock = std::make_unique<MockExecutor>(s_left, left_data);
  auto right_mock = std::make_unique<MockExecutor>(s_right, right_data);

  // ON l_id = r_id
  auto join_pred = std::make_unique<sthir::BinaryExpr>(
      sthir::BinaryOp::EQ,
      std::make_unique<sthir::ColumnRefExpr>("", "l_id"),
      std::make_unique<sthir::ColumnRefExpr>("", "r_id"));

  auto nlj = std::make_unique<sthir::NestedLoopJoinExecutor>(
      std::move(left_mock), std::move(right_mock), std::move(join_pred), joined_schema);

  nlj->init();
  std::vector<sthir::Tuple> joined_rows;
  while (auto opt = nlj->next()) {
    joined_rows.push_back(std::move(*opt));
  }
  nlj->close();

  // l_id=1 matches two rows in R; l_id=2 matches 0; l_id=3 matches 1. Total: 3 rows!
  CHECK_EQ(joined_rows.size(), 3u);
  CHECK_EQ(joined_rows[0].GetValue(0).as_int(), 1);
  CHECK_EQ(joined_rows[0].GetValue(3).as_string(), "Match1");
  CHECK_EQ(joined_rows[1].GetValue(0).as_int(), 1);
  CHECK_EQ(joined_rows[1].GetValue(3).as_string(), "Match1_dup");
  CHECK_EQ(joined_rows[2].GetValue(0).as_int(), 3);
  CHECK_EQ(joined_rows[2].GetValue(3).as_string(), "Match3");

  test_passed("nested_loop_join_executor");
}

}  // namespace

int main() {
  test_3vl_logic();
  test_tuple_serialization_roundtrip();
  test_filter_and_project_executors();
  test_aggregate_executor();
  test_sort_executor();
  test_nested_loop_join_executor();
  return 0;
}
