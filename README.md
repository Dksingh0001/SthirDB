# SthirDB

**SthirDB** is a disk-resident relational database engine with ARIES-style crash recovery, evaluated on a constraint-driven retail workload.

## Requirements

- C++17 compatible compiler (`g++` or `clang++`)
- GNU Make (`make` or `mingw32-make`)
- POSIX environment or Windows (MinGW / MSYS2 / UCRT64)

## Building

To build the database executable:

```bash
make
```

The compiled binary will be placed at `./build/sthirdb`.

To build and run the complete test suite:

```bash
make test
```

## CLI Usage

SthirDB provides an interactive command-line interface:

```bash
# Print version
./build/sthirdb --version

# Run SQL statements (DDL, DML, DQL, Control)
./build/sthirdb run "<sql>"

# Legacy Phase 1 demo commands
./build/sthirdb init_schema
./build/sthirdb load_data --rows 5000
./build/sthirdb count
```

### Supported SQL Subset (Phase 2)

- **DDL:** `CREATE TABLE t (col type [constraints...], ...)` supporting `INT`, `VARCHAR(n)`, `DECIMAL`, `PRIMARY KEY`, `UNIQUE`, `CHECK (<expr>)`, `FOREIGN KEY ... REFERENCES ...`, `NOT NULL`, `ON DELETE RESTRICT | CASCADE`.
- **DML:**
  - `INSERT INTO t VALUES (...)` and `INSERT INTO t (col, ...) VALUES (...)`
  - `UPDATE t SET col = val [WHERE pred]`
  - `DELETE FROM t [WHERE pred]`
- **Queries:** `SELECT col, ... | * FROM t [JOIN t2 ON pred] [WHERE pred] [GROUP BY col] [ORDER BY col [ASC|DESC]]`
- **Aggregates:** `COUNT(*)`, `COUNT(col)`, `SUM(col)`, `MIN(col)`, `MAX(col)`, `AVG(col)`
- **Predicates:** `=`, `<>`, `<`, `<=`, `>`, `>=`, `AND`, `OR`, `NOT`, `IS NULL`, `IS NOT NULL`
- **Control & Utilities:** `BEGIN`, `COMMIT`, `ABORT` / `ROLLBACK`, `ANALYZE t`, `SET <flag> = <value>`, `EXPLAIN <sql>`

### Example

```bash
./build/sthirdb run "CREATE TABLE Items (ItemID INT, ItemName VARCHAR(100), RetailPrice DECIMAL);"
./build/sthirdb run "INSERT INTO Items VALUES (1, 'Widget', 9.99);"
./build/sthirdb run "SELECT ItemName FROM Items WHERE RetailPrice > 5;"
./build/sthirdb run "SELECT COUNT(*) FROM Items;"
./build/sthirdb run "UPDATE Items SET RetailPrice = 12.50 WHERE ItemID = 1;"
./build/sthirdb run "DELETE FROM Items WHERE ItemID = 1;"
```
