# MiniDB — Minimalist Relational Database Engine

> **Advanced Database Management Systems — Capstone Project**
> **Extension Track A: Performance (Vectorized/Batch Execution)**

---

## 1. Project Overview

### Problem Statement

Build a functioning relational database engine from foundational components, integrating storage, indexing, query processing, transactions, and recovery into a coherent system.

### Goals

- Implement all core database subsystems (storage, indexing, execution, optimization, transactions, recovery)
- Demonstrate understanding of database internals and engineering trade-offs
- Deliver a working system with correct, simple, and maintainable code

### Chosen Extension Track

**Track A — Performance**: Vectorized/Batch execution engine that processes rows in chunks (batches of 128 tuples) instead of one-at-a-time, reducing per-row overhead and improving CPU cache locality.

### Team Information

**Team Name**: `Team_MiniDB`

| Name | Roll Number | Scaler Email |
|------|-------------|-------|
| Apurv Dugar | 24BCS10107 | `apurv.24bcs10107@sst.scaler.com` |
| Akshat Sipany | 24BCS10059 | `akshat.24bcs10059@sst.scaler.com` |
| Harshita Hirawat | 24BCS10044 | `harshita.24bcs10044@sst.scaler.com` |

---

## 2. System Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                      SQL Interface                           │
│              (Simple string-based parser)                    │
├──────────────────────────────────────────────────────────────┤
│                    Query Optimizer                           │
│          (Selectivity estimation, scan routing)              │
├──────────────────────────────────────────────────────────────┤
│                  Execution Engine                            │
│        (Batch/Vectorized operators — Track A)                │
│  ┌──────────┐ ┌───────────┐ ┌────────┐ ┌──────────────────┐  │
│  │ SeqScan  │ │ IndexScan │ │ Filter │ │ NestedLoopJoin   │  │
│  └──────────┘ └───────────┘ └────────┘ └──────────────────┘  │
├──────────────────────────────────────────────────────────────┤
│  ┌─────────────────┐        ┌──────────────────────────┐     │
│  │  B+ Tree Index  │        │  HeapFile (Table Store)  │     │
│  └─────────────────┘        └──────────────────────────┘     │
├──────────────────────────────────────────────────────────────┤
│              Buffer Pool Manager (LRU, 64 frames)            │
├──────────────────────────────────────────────────────────────┤
│                Disk Manager (Page I/O)                       │
├──────────────────────────────────────────────────────────────┤
│  ┌────────────────────────┐  ┌─────────────────────────┐     │
│  │ Transaction Manager    │  │  WAL Recovery Manager   │     │
│  │ (Strict 2PL)           │  │  (REDO-only)            │     │
│  └────────────────────────┘  └─────────────────────────┘     │
└──────────────────────────────────────────────────────────────┘
```

### Major Modules

| Module | Directory | Responsibility |
|--------|-----------|---------------|
| Storage | `src/storage/` | Page management, disk I/O, buffer pool, heap files |
| Index | `src/index/` | B+ Tree for primary key indexing |
| Catalog | `src/catalog/` | Table metadata and schema management |
| Execution | `src/execution/` | SQL parsing, batch operators, query execution |
| Optimizer | `src/optimizer/` | Cost-based scan selection and join ordering |
| Transaction | `src/transaction/` | Strict 2PL locking, transaction lifecycle |
| Recovery | `src/recovery/` | Write-Ahead Logging and REDO recovery |

### Data Flow

1. User submits SQL string → **Parser** produces AST
2. **Optimizer** analyzes predicates, estimates selectivity, chooses scan strategy
3. **Executor** builds operator tree (SeqScan/IndexScan → Filter → Join → Projection)
4. Operators exchange **TupleBatch** objects (128 rows per batch)
5. Operators access data through **HeapFile** → **BufferPool** → **DiskManager**
6. All mutations are logged via **WAL** before being applied

---

## 3. Storage Layer

### Page Format

- **Fixed 4 KB pages** (`PAGE_SIZE = 4096`)
- **Slotted page layout**:
  - Header (8 bytes): `num_records`, `free_offset`, `next_page_id`
  - Record data grows forward from the header
  - Slot directory stored at the end of the page, growing backward
  - Each slot entry: `offset (2B) + length (2B) + valid flag (2B) = 6 bytes`

### Heap Files

- Each table is stored as a **linked list of pages**
- Records are serialized as pipe-delimited text (e.g., `1|Alice|22|3.8`)
- Operations: `InsertTuple`, `GetTuple`, `DeleteTuple`, `ScanAll`
- Each table gets its own disk file (e.g., `students.db`)

### Buffer Pool

- **LRU replacement policy** using `std::list` + `std::unordered_map` for O(1) operations
- Default pool size: **64 frames**
- Pin counting prevents eviction of in-use pages
- Dirty pages are flushed on eviction or explicit flush

---

## 4. Indexing

### B+ Tree Design

- **In-memory B+ tree** on `int64_t` primary keys
- Configurable order (default: 64 keys per node)

### Node Structure

- **Internal nodes**: `keys[]` + `children[]` (pointers to child nodes)
- **Leaf nodes**: `keys[]` + `values[]` (RecordId mapping) + `next` (sibling pointer)
- Sibling pointers enable efficient range scans

### Operations

| Operation | Complexity | Notes |
|-----------|-----------|-------|
| Search | O(log n) | Traverse from root to leaf |
| Insert | O(log n) | Split leaf/internal nodes when full |
| Delete | O(log n) | Lazy deletion (no rebalancing) |
| Range Scan | O(log n + k) | Follow sibling pointers |

### Search Path

1. Start at root
2. At each internal node, binary search for the correct child pointer
3. Follow pointer to next level
4. At leaf node, scan keys for match
5. For range queries, follow `next` pointers across leaves

---

## 5. Query Execution

### Parser

Dynamic string-splitting SQL parser supporting:
- `CREATE TABLE table (col1 type, col2 type...)`
- `SELECT col1, col2 FROM table WHERE col op value [AND/OR ...]`
- `SELECT * FROM t1 JOIN t2 ON t1.col = t2.col [WHERE ...]`
- `INSERT INTO table VALUES (v1, v2, ...)`
- `DELETE FROM table WHERE col op value`

Predicates support: `=`, `!=`, `<`, `<=`, `>`, `>=` with `AND`/`OR` logical groupings.

### Query Plan Generation

1. Parser produces AST (`SelectStmt`, `InsertStmt`, `DeleteStmt`)
2. Optimizer analyzes predicates and decides scan strategy
3. Executor builds operator tree bottom-up:
   - Source: `SeqScanOperator` or `IndexScanOperator`
   - Filter: `FilterOperator` (if WHERE predicates exist)
   - Join: `NestedLoopJoinOperator` (if JOIN clause present)
   - Projection: `ProjectionOperator` (column selection)

### Operator Execution (Track A — Vectorized)

All operators implement the **batch iterator interface**:
```cpp
class Operator {
    void Open();
    TupleBatch Next();   // returns batch of up to 128 rows
    void Close();
};
```

**TupleBatch** (`std::vector<Row>`, max 128 rows) is the unit of exchange between operators, replacing the traditional one-row-at-a-time Volcano model. Benefits:
- Amortizes virtual function call overhead
- Improves CPU cache locality
- Enables future SIMD/vectorized predicate evaluation

---

## 6. Optimizer

### Cost Estimation

The optimizer uses a simple cost model:
- **Sequential Scan cost** = number of pages in the table
- **Index Scan cost** = tree height + number of matching pages

### Selectivity Estimation

| Predicate Type | Selectivity |
|----------------|-------------|
| Equality on PK (`id = 3`) | `1 / num_rows` |
| Equality on non-PK | `1 / min(num_rows, 10)` |
| Range predicate | `1 / 3` (heuristic) |

### Decision Rule

- If `selectivity < 0.15` AND an index exists on the predicate column → **Index Scan**
- Otherwise → **Sequential Scan**

### Join Ordering

For 2-table joins:
- Estimate cardinality of each table
- Place the **smaller table as the outer** (left) relation in nested loop join
- This minimizes the number of inner-loop iterations

---

## 7. Transactions & Concurrency

### Locking Strategy

- **Strict Two-Phase Locking (2PL)**
- Lock granularity: **table-level** (SHARED / EXCLUSIVE)
- Locks are acquired before data access
- Locks are released **only on commit or abort** (strict phase)

### Isolation Guarantees

- **Serializable isolation**: Strict 2PL ensures that the schedule is conflict-serializable
- No dirty reads, no non-repeatable reads, no phantom reads (at table-level granularity)

### Deadlock Handling

- **Timeout-based detection**: if a lock request waits longer than 1 second, the requesting transaction is aborted
- This avoids the complexity of a wait-for graph while being effective for the demo workload

### Transaction API

```
BEGIN → txn_id
COMMIT(txn_id) → flush WAL, release all locks
ABORT(txn_id) → release all locks, discard changes
```

---

## 8. Recovery

### WAL Design

- **Append-only log file** (`wal.log`)
- Log records written **before** any data page modifications (WAL protocol)
- Log flushed to disk on every commit (FORCE policy)

### Log Records

Each record is stored as a pipe-delimited line:
```
LSN|TXN_ID|TYPE|TABLE_NAME|KEY_DATA|RECORD_DATA
```

Log types: `BEGIN`, `INSERT`, `DELETE`, `COMMIT`, `ABORT`

### Recovery Policy: FORCE + NO-STEAL

| Policy | Meaning | Impact |
|--------|---------|--------|
| **FORCE** | Dirty pages flushed at commit | All committed data is on disk |
| **NO-STEAL** | No dirty pages written before commit | No uncommitted data on disk |

This combination means recovery only needs **REDO** (no UNDO required).

### Crash Recovery Procedure

1. **Analysis phase**: Scan log to identify committed and aborted transactions
2. **REDO phase**: Replay all INSERT/DELETE operations for committed transactions
3. **Result**: Database state is restored to the last consistent committed state

---

## 9. Extension Track — Track A: Performance

### Motivation

Traditional row-at-a-time (Volcano) execution incurs significant per-row overhead:
- Virtual function dispatch for each row
- Poor CPU cache utilization
- Branch misprediction on tight filter loops

Batch/vectorized execution amortizes this overhead by processing multiple rows per operator call.

### Design

- **TupleBatch**: `std::vector<Row>` with configurable batch size (128 rows)
- All operators (`SeqScan`, `Filter`, `Join`, `Projection`) exchange batches
- Row-at-a-time variants (`SeqScanRowOperator`, `FilterRowOperator`) implemented for comparison

### Results

Benchmark compares batch vs. row-at-a-time on scan+filter workloads with varying table sizes (1K–50K rows). Expected and observed improvements:

- **Reduced function call overhead**: Each `Next()` call processes up to 128 rows
- **Better cache locality**: Batch of rows kept in contiguous memory
- **Measurable speedup**: 1.1x–1.5x+ improvement for larger datasets

*(Run `./benchmark` to generate actual numbers for your hardware)*

---

## 10. Benchmarks

### Experimental Setup

- **Operation**: Sequential scan + filter (predicate: `value > 500`, ~50% selectivity)
- **Workloads**: 1,000 / 5,000 / 10,000 / 50,000 rows
- **Iterations**: 3 runs averaged (1 warmup)
- **Metric**: Wall-clock time (microseconds), throughput (rows/sec), speedup ratio

### Results

Run the benchmark to generate results:
```bash
./benchmark
```

Sample output format:
```
  Rows     Batch (μs)     Row (μs)       Speedup     Batch (rows/s) Row (rows/s)
  ──────────────────────────────────────────────────────────────────────────────
  1000     xxx            xxx            x.xx        xxx            xxx
  5000     xxx            xxx            x.xx        xxx            xxx
  10000    xxx            xxx            x.xx        xxx            xxx
  50000    xxx            xxx            x.xx        xxx            xxx
```

### Analysis

Batch processing provides measurable speedup due to:
1. Amortized virtual call overhead (128x fewer `Next()` calls)
2. Improved cache utilization (batch fits in L1/L2 cache)
3. Speedup grows with dataset size as fixed overhead is amortized over more rows

---

## 11. Limitations

### Missing Features
- No `UPDATE` statement support
- No aggregate functions (`COUNT`, `SUM`, `AVG`, etc.)
- No `ORDER BY` or `GROUP BY`
- No secondary indexes (only primary key B+ tree)
- No multi-column predicates in the optimizer
- No `CREATE TABLE` SQL syntax (tables created programmatically)

### Scalability Limits
- Table-level locking limits concurrent write throughput
- In-memory B+ tree not persisted across restarts
- Buffer pool is per-table (not shared across all tables)
- Single-threaded query execution

### Future Improvements
- Row-level locking for finer concurrency
- Persistent B+ tree (stored in pages)
- Hash join and sort-merge join operators
- Expression evaluation with SIMD vectorization
- Shared buffer pool across all tables
- SQL `CREATE TABLE` / `ALTER TABLE` support

---

## 12. How to Run

### Dependencies

- **C++17** compiler (GCC 8+, Clang 7+, MSVC 2019+)
- **CMake** 3.16 or higher
- No external database libraries required

### Build Steps

```bash
# Clone and navigate to project
cd MINI-DB

# Create build directory
mkdir build && cd build

# Configure
cmake .. -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build . --config Release
```

### Run the Demo

```bash
# From the build directory:
./minidb          # or .\Release\minidb.exe on Windows
```

This will prompt you to choose between running the automated tests (`demo`) or starting a clean terminal (`manual`).
If you choose `demo`, it will run the full demonstration:
1. Creates tables (`students`, `courses`, `teachers` via SQL)
2. Inserts sample data with transactions
3. Runs SELECT queries (seq scan, index scan, joins, AND/OR logic)
4. Demonstrates DELETE
5. Shows concurrent transaction locking
6. Demonstrates WAL crash recovery

After the demo (or if you choose `manual`), it launches an **Interactive Terminal**:
- Type `minidb>` queries like `SELECT * FROM students`
- Use `/tables` to see all database tables
- Use `/schema <tablename>` to view table structure
- Type `exit` to close.

### Run the Benchmark

```bash
./benchmark       # or .\Release\benchmark.exe on Windows
```

Compares batch/vectorized vs. row-at-a-time execution across multiple workload sizes.

### Project Structure

```
MINI-DB/
├── CMakeLists.txt
├── README.md
├── src/
│   ├── common/types.h
│   ├── storage/
│   │   ├── page.h / page.cpp
│   │   ├── disk_manager.h / disk_manager.cpp
│   │   ├── buffer_pool.h / buffer_pool.cpp
│   │   └── heap_file.h / heap_file.cpp
│   ├── index/
│   │   └── bplus_tree.h / bplus_tree.cpp
│   ├── catalog/
│   │   └── catalog.h / catalog.cpp
│   ├── execution/
│   │   ├── ast.h
│   │   ├── parser.h / parser.cpp
│   │   ├── batch.h
│   │   ├── operators.h / operators.cpp
│   │   └── executor.h / executor.cpp
│   ├── optimizer/
│   │   └── optimizer.h / optimizer.cpp
│   ├── transaction/
│   │   ├── transaction.h
│   │   ├── lock_manager.h / lock_manager.cpp
│   │   └── txn_manager.h / txn_manager.cpp
│   ├── recovery/
│   │   └── wal.h / wal.cpp
│   └── main.cpp
└── benchmarks/
    └── benchmark.cpp
```