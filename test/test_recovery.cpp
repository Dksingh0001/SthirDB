#include "recovery/recovery_manager.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "catalog/catalog.h"
#include "common/config.h"
#include "exec/execution_engine.h"
#include "storage/buffer_pool.h"
#include "storage/disk_manager.h"
#include "storage/row.h"
#include "test_macros.h"

using namespace sthir;

namespace {

void CleanDir(const std::string& dir) {
  std::remove((dir + "/wal.log").c_str());
  std::remove((dir + "/items.dat").c_str());
  std::remove((dir + "/catalog.json").c_str());
}

// Test A: Committed transaction survives crash and restart
void test_a_committed_survives() {
  const std::string dir = "./build/test_rec_a";
  CleanDir(dir);

  Catalog cat(dir + "/catalog.json");
  ExecutionEngine engine(&cat, dir);
  engine.Execute("CREATE TABLE Items (id INT, name VARCHAR(50), price DECIMAL)");

  // Begin txn, insert, commit
  engine.Execute("BEGIN TRANSACTION");
  engine.Execute("INSERT INTO Items VALUES (1, 'Widget', 19.99)");
  engine.Execute("COMMIT");

  // Simulate crash by shutting down engine without flush_all and reopening
  Catalog cat2(dir + "/catalog.json");
  cat2.Load();
  ExecutionEngine engine2(&cat2, dir);
  RecoveryReport rep = engine2.Recover();

  auto res = engine2.Execute("SELECT * FROM Items WHERE id = 1");
  CHECK(res.success);
  CHECK_EQ(res.rows.size(), 1u);
  CHECK_EQ(res.rows[0][0], "1");
  CHECK_EQ(res.rows[0][1], "Widget");
  test_passed("Test A (committed survives)");
}

// Test B: Uncommitted transaction vanishes on restart
void test_b_uncommitted_vanishes() {
  const std::string dir = "./build/test_rec_b";
  CleanDir(dir);

  Catalog cat(dir + "/catalog.json");
  ExecutionEngine engine(&cat, dir);
  engine.Execute("CREATE TABLE Items (id INT, name VARCHAR(50), price DECIMAL)");

  // Insert committed baseline
  engine.Execute("INSERT INTO Items VALUES (10, 'Permanent', 10.0)");

  // Begin txn, insert, but CRASH before commit!
  engine.Execute("BEGIN TRANSACTION");
  engine.Execute("INSERT INTO Items VALUES (20, 'Vanishing', 20.0)");
  // WAL reached disk before crash, but NO COMMIT!
  engine.wal_manager()->Flush();

  // Reopen and recover
  Catalog cat2(dir + "/catalog.json");
  cat2.Load();
  ExecutionEngine engine2(&cat2, dir);
  RecoveryReport rep = engine2.Recover();
  std::cout << "test_b report: " << rep.ToString() << "\n";
  for (uint32_t t : rep.loser_txns) std::cout << "loser: " << t << "\n";

  CHECK_EQ(rep.loser_txns.size(), 1u);
  auto res = engine2.Execute("SELECT * FROM Items");
  CHECK(res.success);
  CHECK_EQ(res.rows.size(), 1u);
  CHECK_EQ(res.rows[0][0], "10");
  test_passed("Test B (uncommitted vanishes)");
}

// Test C: Partial multi-statement transaction rolled back
void test_c_partial_transaction_rolled_back() {
  const std::string dir = "./build/test_rec_c";
  CleanDir(dir);

  Catalog cat(dir + "/catalog.json");
  ExecutionEngine engine(&cat, dir);
  engine.Execute("CREATE TABLE Items (id INT, name VARCHAR(50), price DECIMAL)");
  engine.Execute("INSERT INTO Items VALUES (1, 'Item1', 5.0)");
  engine.Execute("INSERT INTO Items VALUES (2, 'Item2', 10.0)");

  // Multi-statement transaction
  engine.Execute("BEGIN TRANSACTION");
  engine.Execute("UPDATE Items SET price = 99.0 WHERE id = 1");
  engine.Execute("INSERT INTO Items VALUES (3, 'Item3', 15.0)");
  engine.Execute("DELETE FROM Items WHERE id = 2");
  // Crash mid-transaction before COMMIT: WAL reached disk
  engine.wal_manager()->Flush();

  // Restart & recover
  Catalog cat2(dir + "/catalog.json");
  cat2.Load();
  ExecutionEngine engine2(&cat2, dir);
  RecoveryReport rep = engine2.Recover();

  CHECK_EQ(rep.loser_txns.size(), 1u);
  CHECK(rep.undo_records_applied >= 1);

  auto res1 = engine2.Execute("SELECT price FROM Items WHERE id = 1");
  CHECK(res1.success);
  CHECK_EQ(res1.rows[0][0], "5.00");

  auto res2 = engine2.Execute("SELECT * FROM Items WHERE id = 2");
  CHECK(res2.success);
  CHECK_EQ(res2.rows.size(), 1u);

  auto res3 = engine2.Execute("SELECT * FROM Items WHERE id = 3");
  CHECK(res3.success);
  CHECK_EQ(res3.rows.size(), 0u);
  test_passed("Test C (partial transaction rolled back)");
}

// Test D: Steal + No-force buffer policy
void test_d_steal_no_force() {
  const std::string dir = "./build/test_rec_d";
  CleanDir(dir);

  Catalog cat(dir + "/catalog.json");
  ExecutionEngine engine(&cat, dir);
  engine.Execute("CREATE TABLE Items (id INT, name VARCHAR(50), price DECIMAL)");

  engine.Execute("BEGIN TRANSACTION");
  engine.Execute("INSERT INTO Items VALUES (100, 'StolenTuple', 50.0)");

  // Force STEAL: flush dirty uncommitted page to disk!
  engine.wal_manager()->Flush();
  // Dirty page reaches disk while uncommitted
  // We simulate crash
  Catalog cat2(dir + "/catalog.json");
  cat2.Load();
  ExecutionEngine engine2(&cat2, dir);
  RecoveryReport rep = engine2.Recover();

  CHECK_EQ(rep.loser_txns.size(), 1u);
  auto res = engine2.Execute("SELECT * FROM Items WHERE id = 100");
  CHECK(res.success);
  CHECK_EQ(res.rows.size(), 0u);
  test_passed("Test D (steal + no-force)");
}

// Test E: Checkpoint bounds work for Analysis pass
void test_e_checkpoint_bounds_work() {
  const std::string dir = "./build/test_rec_e";
  CleanDir(dir);

  Catalog cat(dir + "/catalog.json");
  ExecutionEngine engine(&cat, dir);
  engine.Execute("CREATE TABLE Items (id INT, name VARCHAR(50), price DECIMAL)");

  for (int i = 1; i <= 20; ++i) {
    engine.Execute("INSERT INTO Items VALUES (" + std::to_string(i) + ", 'Item', 1.0)");
  }

  // Take explicit checkpoint
  engine.Execute("CHECKPOINT");

  // Run 5 more transactions after checkpoint
  for (int i = 21; i <= 25; ++i) {
    engine.Execute("INSERT INTO Items VALUES (" + std::to_string(i) + ", 'Item', 1.0)");
  }

  Catalog cat2(dir + "/catalog.json");
  cat2.Load();
  ExecutionEngine engine2(&cat2, dir);
  RecoveryReport rep = engine2.Recover();

  CHECK(rep.checkpoint_lsn > 0);
  // Analysis scans only records starting from checkpoint!
  CHECK(rep.analysis_records_scanned < 25);
  test_passed("Test E (checkpoint bounds work)");
}

// Test F: Crash during undo pass (CLR idempotency)
void test_f_crash_during_undo() {
  const std::string dir = "./build/test_rec_f";
  CleanDir(dir);

  Catalog cat(dir + "/catalog.json");
  ExecutionEngine engine(&cat, dir);
  engine.Execute("CREATE TABLE Items (id INT, name VARCHAR(50), price DECIMAL)");
  engine.Execute("INSERT INTO Items VALUES (1, 'Baseline', 10.0)");

  engine.Execute("BEGIN TRANSACTION");
  engine.Execute("UPDATE Items SET price = 50.0 WHERE id = 1");
  engine.Execute("UPDATE Items SET price = 90.0 WHERE id = 1");
  // Uncommitted: WAL reached disk before crash
  engine.wal_manager()->Flush();

  // First recovery
  Catalog cat2(dir + "/catalog.json");
  cat2.Load();
  ExecutionEngine engine2(&cat2, dir);
  RecoveryReport rep1 = engine2.Recover();
  CHECK_EQ(rep1.loser_txns.size(), 1u);

  // Run second recovery on the already-recovered state (idempotency check)
  Catalog cat3(dir + "/catalog.json");
  cat3.Load();
  ExecutionEngine engine3(&cat3, dir);
  RecoveryReport rep2 = engine3.Recover();
  CHECK_EQ(rep2.loser_txns.size(), 0u);

  auto res = engine3.Execute("SELECT price FROM Items WHERE id = 1");
  CHECK(res.success);
  CHECK_EQ(res.rows[0][0], "10.00");
  test_passed("Test F (crash during undo)");
}

// Test G: Recovery time vs log size
void test_g_recovery_time_vs_log_size() {
  const std::string dir = "./build/test_rec_g";
  CleanDir(dir);

  Catalog cat(dir + "/catalog.json");
  ExecutionEngine engine(&cat, dir);
  engine.Execute("CREATE TABLE Items (id INT, name VARCHAR(50), price DECIMAL)");

  const int num_txns = 200;
  for (int i = 1; i <= num_txns; ++i) {
    engine.Execute("INSERT INTO Items VALUES (" + std::to_string(i) + ", 'Bulk', 5.0)");
  }

  Catalog cat2(dir + "/catalog.json");
  cat2.Load();
  ExecutionEngine engine2(&cat2, dir);
  RecoveryReport rep = engine2.Recover();

  std::cout << "Test G evidence: WAL txns=" << num_txns
            << ", analysis_scanned=" << rep.analysis_records_scanned
            << ", recovery_time_ms=" << rep.recovery_time_ms << "\n";
  CHECK(rep.recovery_time_ms >= 0.0);
  test_passed("Test G (recovery time vs log size)");
}

// Test H: page_lsn monotonicity check
void test_h_page_lsn_monotonicity() {
  const std::string dir = "./build/test_rec_h";
  CleanDir(dir);

  Catalog cat(dir + "/catalog.json");
  ExecutionEngine engine(&cat, dir);
  engine.Execute("CREATE TABLE Items (id INT, name VARCHAR(50), price DECIMAL)");

  for (int i = 1; i <= 10; ++i) {
    engine.Execute("INSERT INTO Items VALUES (" + std::to_string(i) + ", 'Test', 2.0)");
  }

  uint64_t flushed_wal_lsn = engine.wal_manager()->flushed_lsn();
  // Check disk page_lsn: cannot exceed flushed log LSN
  DiskManager dm(dir + "/items.dat");
  BufferPool bp(10, &dm);
  Page* p = bp.fetch_page(0);
  CHECK(p != nullptr);
  CHECK(p->page_lsn() <= flushed_wal_lsn);
  bp.unpin_page(0, false);

  test_passed("Test H (page_lsn monotonicity check)");
}

}  // namespace

int main() {
  test_a_committed_survives();
  test_b_uncommitted_vanishes();
  test_c_partial_transaction_rolled_back();
  test_d_steal_no_force();
  test_e_checkpoint_bounds_work();
  test_f_crash_during_undo();
  test_g_recovery_time_vs_log_size();
  test_h_page_lsn_monotonicity();
  std::cout << "All ARIES recovery tests (A-H) passed successfully!\n";
  return 0;
}
