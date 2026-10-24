#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "catalog/catalog.h"
#include "concurrency/lock_manager.h"
#include "exec/execution_engine.h"

namespace fs = std::filesystem;
using namespace sthir;

static void AssertTrue(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << std::endl;
    std::exit(1);
  }
}

// -------------------------------------------------------------
// Test 1: S/X Lock Compatibility & FIFO Fairness
// -------------------------------------------------------------
void test_lock_modes_and_fifo_fairness() {
  LockManager lm(2000);  // 2s timeout
  std::string table = "items";
  RID rid{1, 5, true};

  // Multiple Shared locks can be granted concurrently
  AssertTrue(lm.AcquireShared(1, table, rid), "Txn 1 should acquire Shared lock");
  AssertTrue(lm.AcquireShared(2, table, rid), "Txn 2 should acquire Shared lock concurrently");
  AssertTrue(lm.TotalLocksHeld() == 2, "2 shared locks held");

  // Exclusive lock request from Txn 3 must wait while Shared locks exist
  std::atomic<bool> txn3_acquired{false};
  std::atomic<bool> txn3_started{false};
  std::thread t3([&]() {
    txn3_started = true;
    bool ok = lm.AcquireExclusive(3, table, rid);
    if (ok) txn3_acquired = true;
  });

  while (!txn3_started) std::this_thread::yield();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  AssertTrue(!txn3_acquired, "Txn 3 Exclusive request must block while Shared locks are held");

  // FIFO Fairness: Txn 4 requesting Shared should queue behind Txn 3 (no writer starvation)
  std::atomic<bool> txn4_acquired{false};
  std::atomic<bool> txn4_started{false};
  std::thread t4([&]() {
    txn4_started = true;
    bool ok = lm.AcquireShared(4, table, rid);
    if (ok) txn4_acquired = true;
  });

  while (!txn4_started) std::this_thread::yield();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  AssertTrue(!txn4_acquired, "Txn 4 Shared request must queue behind Txn 3 for FIFO fairness");

  // Release Txn 1 and Txn 2 shared locks
  lm.Release(1, table, rid);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  AssertTrue(!txn3_acquired, "Txn 3 still waiting for Txn 2 to release");

  lm.Release(2, table, rid);
  t3.join();
  AssertTrue(txn3_acquired, "Txn 3 must acquire Exclusive lock once all Shared locks released");
  AssertTrue(!txn4_acquired, "Txn 4 must still wait while Txn 3 holds Exclusive lock");

  // Release Txn 3 Exclusive lock -> Txn 4 should now be granted Shared lock
  lm.Release(3, table, rid);
  t4.join();
  AssertTrue(txn4_acquired, "Txn 4 must acquire Shared lock once Txn 3 Exclusive lock released");

  lm.Release(4, table, rid);
  AssertTrue(lm.TotalLocksHeld() == 0, "All locks released");
  std::cout << "PASS test_lock_modes_and_fifo_fairness" << std::endl;
}

// -------------------------------------------------------------
// Test 2: Global Lock Ordering for Deadlock Avoidance
// -------------------------------------------------------------
void test_global_lock_ordering_deadlock_avoidance() {
  LockManager lm(3000);
  std::string table = "items";
  RID r1{1, 1, true};
  RID r2{1, 2, true};
  RID r3{2, 1, true};

  std::atomic<bool> t1_ok{false};
  std::atomic<bool> t2_ok{false};

  // Thread 1 requests in order {r1, r2, r3}
  // Thread 2 requests in reverse order {r3, r2, r1}
  // Because AcquireExclusiveBatch sorts keys globally, both acquire in ascending order.
  std::thread t1([&]() {
    bool ok = lm.AcquireExclusiveBatch(101, table, {r1, r2, r3});
    t1_ok = ok;
    if (ok) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      lm.ReleaseAll(101);
    }
  });

  std::thread t2([&]() {
    bool ok = lm.AcquireExclusiveBatch(102, table, {r3, r2, r1});
    t2_ok = ok;
    if (ok) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      lm.ReleaseAll(102);
    }
  });

  t1.join();
  t2.join();

  AssertTrue(t1_ok && t2_ok, "Both transactions should succeed without deadlocking");
  AssertTrue(lm.TotalLocksHeld() == 0, "No locks leaked after batch release");
  std::cout << "PASS test_global_lock_ordering_deadlock_avoidance" << std::endl;
}

// -------------------------------------------------------------
// Test 3: Strict 2PL Release-All on Abort
// -------------------------------------------------------------
void test_strict_2pl_abort_releases_locks() {
  LockManager lm(2000);
  std::string table = "items";
  RID r1{1, 10, true};
  RID r2{1, 11, true};

  AssertTrue(lm.AcquireExclusive(201, table, r1), "Txn 201 lock r1");
  AssertTrue(lm.AcquireExclusive(201, table, r2), "Txn 201 lock r2");
  AssertTrue(lm.TotalLocksHeld() == 2, "2 locks held by 201");

  std::atomic<bool> t2_granted{false};
  std::thread t2([&]() {
    bool ok = lm.AcquireExclusive(202, table, r1);
    if (ok) {
      t2_granted = true;
      lm.ReleaseAll(202);
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  AssertTrue(!t2_granted, "Txn 202 must be blocked by Txn 201");

  // Strict 2PL: Txn 201 aborts and releases ALL locks
  lm.ReleaseAll(201);
  t2.join();

  AssertTrue(t2_granted, "Txn 202 acquired lock after Txn 201 aborted and released all");
  AssertTrue(lm.TotalLocksHeld() == 0, "All locks cleared");
  std::cout << "PASS test_strict_2pl_abort_releases_locks" << std::endl;
}

// -------------------------------------------------------------
// Test 4: Two Concurrent Threads Updating the Same Row (No Lost Update)
// -------------------------------------------------------------
void test_two_threads_serialized_no_lost_update() {
  std::string test_dir = "./data/test_cc_lost_update";
  fs::remove_all(test_dir);
  fs::create_directories(test_dir);

  auto catalog = std::make_unique<Catalog>(test_dir + "/catalog.dat");
  auto engine = std::make_unique<ExecutionEngine>(catalog.get(), test_dir);

  auto r = engine->Execute("CREATE TABLE Counter (ID INT, Val INT)");
  AssertTrue(r.success, "CREATE TABLE Counter");
  r = engine->Execute("INSERT INTO Counter VALUES (1, 100)");
  AssertTrue(r.success, "INSERT INTO Counter");

  std::atomic<bool> t1_done{false};
  std::atomic<bool> t2_done{false};

  // Thread 1 increments by 50
  std::thread t1([&]() {
    auto res = engine->Execute("UPDATE Counter SET Val = Val + 50 WHERE ID = 1");
    if (res.success) t1_done = true;
    else std::cerr << "t1 error: " << res.error_message << "\n";
  });

  // Thread 2 increments by 30
  std::thread t2([&]() {
    auto res = engine->Execute("UPDATE Counter SET Val = Val + 30 WHERE ID = 1");
    if (res.success) t2_done = true;
    else std::cerr << "t2 error: " << res.error_message << "\n";
  });

  t1.join();
  t2.join();

  AssertTrue(t1_done && t2_done, "Both update threads succeeded");

  r = engine->Execute("SELECT Val FROM Counter WHERE ID = 1");
  AssertTrue(r.success && r.rows.size() == 1, "SELECT Val");
  int final_val = std::stoi(r.rows[0][0]);
  AssertTrue(final_val == 180, "Expected Val = 180 (100 + 50 + 30), got: " + std::to_string(final_val));

  engine.reset();
  catalog.reset();
  fs::remove_all(test_dir);
  std::cout << "PASS test_two_threads_serialized_no_lost_update" << std::endl;
}

// -------------------------------------------------------------
// Test 5: 10 Threads x 100 Increments (Stress Concurrency)
// -------------------------------------------------------------
void test_ten_threads_100_increments() {
  std::string test_dir = "./data/test_cc_100_increments";
  fs::remove_all(test_dir);
  fs::create_directories(test_dir);

  auto catalog = std::make_unique<Catalog>(test_dir + "/catalog.dat");
  auto engine = std::make_unique<ExecutionEngine>(catalog.get(), test_dir);

  auto r = engine->Execute("CREATE TABLE Accumulator (ID INT, Val INT)");
  AssertTrue(r.success, "CREATE TABLE Accumulator");
  r = engine->Execute("INSERT INTO Accumulator VALUES (1, 0)");
  AssertTrue(r.success, "INSERT initial 0");

  const int kThreads = 10;
  const int kIncrementsPerThread = 100;
  std::vector<std::thread> workers;
  std::atomic<int> success_count{0};

  for (int i = 0; i < kThreads; ++i) {
    workers.emplace_back([&]() {
      for (int inc = 0; inc < kIncrementsPerThread; ++inc) {
        auto res = engine->Execute("UPDATE Accumulator SET Val = Val + 1 WHERE ID = 1");
        if (res.success) {
          success_count++;
        } else {
          std::cerr << "inc failed: " << res.error_message << "\n";
        }
      }
    });
  }

  for (auto& w : workers) {
    w.join();
  }

  AssertTrue(success_count == kThreads * kIncrementsPerThread,
             "All 1000 increment queries succeeded, got: " + std::to_string(success_count));

  r = engine->Execute("SELECT Val FROM Accumulator WHERE ID = 1");
  AssertTrue(r.success && r.rows.size() == 1, "SELECT Val");
  int final_val = std::stoi(r.rows[0][0]);
  AssertTrue(final_val == 1000, "Expected 1000, got: " + std::to_string(final_val));

  engine.reset();
  catalog.reset();
  fs::remove_all(test_dir);
  std::cout << "PASS test_ten_threads_100_increments (1000/1000 atomic increments verified)" << std::endl;
}

// -------------------------------------------------------------
// Test 6: Concurrency + ARIES Crash Recovery Interaction
// -------------------------------------------------------------
void test_concurrent_crash_and_recovery() {
  std::string test_dir = "./data/test_cc_aries_crash";
  fs::remove_all(test_dir);
  fs::create_directories(test_dir);

  {
    auto catalog = std::make_unique<Catalog>(test_dir + "/catalog.dat");
    auto engine = std::make_unique<ExecutionEngine>(catalog.get(), test_dir);

    auto r = engine->Execute("CREATE TABLE Bank (AccID INT, Balance INT)");
    AssertTrue(r.success, "CREATE TABLE Bank");

    // Seed 5 accounts
    for (int i = 1; i <= 5; ++i) {
      r = engine->Execute("INSERT INTO Bank VALUES (" + std::to_string(i) + ", 1000)");
      AssertTrue(r.success, "Insert account");
    }

    // Run concurrent transactions: some commit, some abort, some left in flight at simulated crash
    std::thread t1([&]() {
      // Committed transaction
      engine->Execute("BEGIN");
      engine->Execute("UPDATE Bank SET Balance = Balance + 200 WHERE AccID = 1");
      engine->Execute("COMMIT");
    });

    std::thread t2([&]() {
      // Aborted transaction
      engine->Execute("BEGIN");
      engine->Execute("UPDATE Bank SET Balance = Balance - 500 WHERE AccID = 2");
      engine->Execute("ROLLBACK");
    });

    t1.join();
    t2.join();

    // Now start an uncommitted transaction and simulate crash
    engine->Execute("BEGIN");
    engine->Execute("UPDATE Bank SET Balance = Balance + 999 WHERE AccID = 3");
    // Flush WAL so the uncommitted update log record reaches disk
    engine->wal_manager()->Flush();

    // Force dirty page to disk (Steal policy)
    // Simulated crash occurs here without COMMIT
  }

  // Restart engine and recover
  {
    auto catalog = std::make_unique<Catalog>(test_dir + "/catalog.dat");
    auto engine = std::make_unique<ExecutionEngine>(catalog.get(), test_dir);

    RecoveryReport rep = engine->Recover();
    AssertTrue(rep.loser_txns.size() == 1, "Uncommitted transaction identified as loser");
    AssertTrue(rep.undo_records_applied >= 1, "Undo applied to uncommitted update");

    // Verify balances:
    // AccID 1: 1000 + 200 = 1200 (committed)
    auto r1 = engine->Execute("SELECT Balance FROM Bank WHERE AccID = 1");
    AssertTrue(r1.success && r1.rows.size() == 1, "SELECT AccID 1");
    AssertTrue(std::stoi(r1.rows[0][0]) == 1200, "AccID 1 balance committed at 1200");

    // AccID 2: 1000 (aborted)
    auto r2 = engine->Execute("SELECT Balance FROM Bank WHERE AccID = 2");
    AssertTrue(r2.success && r2.rows.size() == 1, "SELECT AccID 2");
    AssertTrue(std::stoi(r2.rows[0][0]) == 1000, "AccID 2 balance restored to 1000");

    // AccID 3: 1000 (uncommitted at crash, undone by ARIES)
    auto r3 = engine->Execute("SELECT Balance FROM Bank WHERE AccID = 3");
    AssertTrue(r3.success && r3.rows.size() == 1, "SELECT AccID 3");
    AssertTrue(std::stoi(r3.rows[0][0]) == 1000, "AccID 3 uncommitted 999 undone to 1000");

    // Verify all locks in lock manager are clear after recovery
    AssertTrue(engine->lock_manager()->TotalLocksHeld() == 0,
               "Lock table completely clear following restart recovery");
    engine.reset();
    catalog.reset();
  }

  fs::remove_all(test_dir);
  std::cout << "PASS test_concurrent_crash_and_recovery" << std::endl;
}

int main() {
  std::cout << "Starting Phase 6 Concurrency Control Tests..." << std::endl;
  test_lock_modes_and_fifo_fairness();
  test_global_lock_ordering_deadlock_avoidance();
  test_strict_2pl_abort_releases_locks();
  test_two_threads_serialized_no_lost_update();
  test_ten_threads_100_increments();
  test_concurrent_crash_and_recovery();
  std::cout << "All Phase 6 Concurrency Control tests passed successfully!" << std::endl;
  return 0;
}
