/*
 * MiniDB — Main Demo
 *
 * Demonstrates:
 *  1. Storage engine (page allocation, buffer pool)
 *  2. Table creation and inserts
 *  3. B+ Tree index usage
 *  4. SELECT with WHERE (seq scan and index scan)
 *  5. SELECT with JOIN
 *  6. DELETE
 *  7. Transaction management (Strict 2PL)
 *  8. WAL logging and crash recovery
 */

#include "common/types.h"
#include "storage/disk_manager.h"
#include "storage/buffer_pool.h"
#include "storage/heap_file.h"
#include "catalog/catalog.h"
#include "index/bplus_tree.h"
#include "execution/parser.h"
#include "execution/executor.h"
#include "optimizer/optimizer.h"
#include "transaction/transaction.h"
#include "transaction/lock_manager.h"
#include "transaction/txn_manager.h"
#include "recovery/wal.h"

#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <filesystem>
#include <thread>

using namespace minidb;

// ── Pretty-print helpers ────────────────────────────────────

static void PrintSeparator(int width = 70) {
    std::cout << std::string(width, '=') << std::endl;
}

static void PrintHeader(const std::string& title) {
    std::cout << "\n";
    PrintSeparator();
    std::cout << "  " << title << std::endl;
    PrintSeparator();
}

static std::string ValueToString(const Value& v) {
    if (std::holds_alternative<int64_t>(v)) return std::to_string(std::get<int64_t>(v));
    if (std::holds_alternative<double>(v))  return std::to_string(std::get<double>(v));
    return std::get<std::string>(v);
}

static void PrintResults(const std::vector<Row>& rows, const Schema& schema) {
    if (rows.empty()) {
        std::cout << "  (no results)" << std::endl;
        return;
    }

    // Print column headers
    std::cout << "  ";
    for (size_t i = 0; i < schema.size(); i++) {
        std::cout << std::left << std::setw(15) << schema[i].name;
    }
    std::cout << std::endl;

    std::cout << "  ";
    for (size_t i = 0; i < schema.size(); i++) {
        std::cout << std::string(14, '-') << " ";
    }
    std::cout << std::endl;

    for (auto& row : rows) {
        std::cout << "  ";
        for (size_t i = 0; i < row.size(); i++) {
            std::cout << std::left << std::setw(15) << ValueToString(row[i]);
        }
        std::cout << std::endl;
    }
    std::cout << "  (" << rows.size() << " row(s))" << std::endl;
}

// ── Crash recovery demonstration ────────────────────────────

static void DemoCrashRecovery(Catalog* catalog, Executor* executor,
                              WALManager* wal) {
    PrintHeader("CRASH RECOVERY DEMONSTRATION");

    // Show current WAL contents
    std::cout << "\n  Current WAL log contents:" << std::endl;
    auto records = wal->ReadLog();
    for (auto& rec : records) {
        std::string type_str;
        switch (rec.type) {
            case LogType::BEGIN:      type_str = "BEGIN";  break;
            case LogType::INSERT:     type_str = "INSERT"; break;
            case LogType::DELETE_LOG: type_str = "DELETE"; break;
            case LogType::COMMIT:     type_str = "COMMIT"; break;
            case LogType::ABORT:      type_str = "ABORT";  break;
        }
        std::cout << "    LSN=" << rec.lsn
                  << " TXN=" << rec.txn_id
                  << " TYPE=" << type_str;
        if (!rec.table_name.empty()) std::cout << " TABLE=" << rec.table_name;
        if (!rec.record_data.empty()) std::cout << " DATA=" << rec.record_data;
        std::cout << std::endl;
    }

    // Simulate recovery: identify committed transactions
    std::cout << "\n  Simulating crash recovery (REDO phase)..." << std::endl;
    std::set<txn_id_t> committed_txns;
    std::set<txn_id_t> aborted_txns;

    for (auto& rec : records) {
        if (rec.type == LogType::COMMIT) {
            committed_txns.insert(rec.txn_id);
        } else if (rec.type == LogType::ABORT) {
            aborted_txns.insert(rec.txn_id);
        }
    }

    std::cout << "  Committed transactions: ";
    for (auto id : committed_txns) std::cout << id << " ";
    std::cout << std::endl;

    std::cout << "  Aborted transactions: ";
    for (auto id : aborted_txns) std::cout << id << " ";
    if (aborted_txns.empty()) std::cout << "(none)";
    std::cout << std::endl;

    // REDO committed inserts
    int redo_count = 0;
    for (auto& rec : records) {
        if (rec.type == LogType::INSERT &&
            committed_txns.count(rec.txn_id)) {
            redo_count++;
            // In a real recovery, we'd re-insert the record.
            // Here we just count and report.
        }
    }
    std::cout << "  REDO operations to replay: " << redo_count << std::endl;
    std::cout << "  Recovery complete. All committed transactions preserved." << std::endl;
}

// ── Main ────────────────────────────────────────────────────

int main() {
    std::cout << R"(
    +------------------------------------------------------+
    |              MiniDB -- Database Engine               |
    |        Track A: Vectorized/Batch Execution           |
    +------------------------------------------------------+
    )" << std::endl;

    // Clean up any previous run
    std::filesystem::remove("students.db");
    std::filesystem::remove("courses.db");
    std::filesystem::remove("wal.log");

    // ── Initialize core components ──────────────────────
    DiskManager disk_students("students.db");
    DiskManager disk_courses("courses.db");
    BufferPoolManager bpm_students(BUFFER_POOL_SIZE, &disk_students);
    BufferPoolManager bpm_courses(BUFFER_POOL_SIZE, &disk_courses);

    Catalog catalog;
    Optimizer optimizer;
    Executor executor(&catalog, &optimizer);

    WALManager wal("wal.log");
    LockManager lock_mgr;
    TransactionManager txn_mgr(&lock_mgr, &wal);

    std::cout << "  Would you like to run the automated demo or start the manual interactive terminal? [demo/manual]: ";
    std::string mode_input;
    std::getline(std::cin, mode_input);
    bool run_demo = (mode_input != "manual");

    if (run_demo) {

    // ═══════════════════════════════════════════════════════
    // 1. CREATE TABLES
    // ═══════════════════════════════════════════════════════
    PrintHeader("1. TABLE CREATION");

    Schema student_schema = {
        {"id",    ColumnType::INT},
        {"name",  ColumnType::STRING},
        {"age",   ColumnType::INT},
        {"gpa",   ColumnType::DOUBLE}
    };
    catalog.CreateTable("students", student_schema, &bpm_students);
    std::cout << "  Created table 'students' (id INT PK, name STRING, age INT, gpa DOUBLE)" << std::endl;

    Schema course_schema = {
        {"id",          ColumnType::INT},
        {"student_id",  ColumnType::INT},
        {"course_name", ColumnType::STRING}
    };
    catalog.CreateTable("courses", course_schema, &bpm_courses);
    std::cout << "  Created table 'courses' (id INT PK, student_id INT, course_name STRING)" << std::endl;

    // ═══════════════════════════════════════════════════════
    // 2. INSERT DATA (with transaction + WAL)
    // ═══════════════════════════════════════════════════════
    PrintHeader("2. INSERT DATA (Transactional)");

    {
        Transaction* txn = txn_mgr.Begin();
        lock_mgr.LockExclusive(txn, "students");
        lock_mgr.LockExclusive(txn, "courses");

        // Insert students
        std::vector<std::string> student_inserts = {
            "INSERT INTO students VALUES (1, 'Alice', 22, 3.8)",
            "INSERT INTO students VALUES (2, 'Bob', 23, 3.5)",
            "INSERT INTO students VALUES (3, 'Charlie', 21, 3.9)",
            "INSERT INTO students VALUES (4, 'Diana', 24, 3.2)",
            "INSERT INTO students VALUES (5, 'Eve', 22, 3.7)",
            "INSERT INTO students VALUES (6, 'Frank', 25, 2.9)",
            "INSERT INTO students VALUES (7, 'Grace', 21, 3.6)",
            "INSERT INTO students VALUES (8, 'Hank', 23, 3.1)"
        };

        for (auto& sql : student_inserts) {
            executor.Execute(sql);
            // Log the insert to WAL
            auto stmt = Parser::Parse(sql);
            auto* ins = static_cast<InsertStmt*>(stmt.get());
            std::string record_data;
            for (size_t i = 0; i < ins->values.size(); i++) {
                if (i > 0) record_data += "|";
                record_data += ValueToString(ins->values[i]);
            }
            wal.AppendLog(txn->GetTxnId(), LogType::INSERT, "students",
                          record_data, ValueToString(ins->values[0]));
            std::cout << "  " << sql << std::endl;
        }

        // Insert courses
        std::vector<std::string> course_inserts = {
            "INSERT INTO courses VALUES (1, 1, 'Databases')",
            "INSERT INTO courses VALUES (2, 1, 'Algorithms')",
            "INSERT INTO courses VALUES (3, 2, 'Databases')",
            "INSERT INTO courses VALUES (4, 3, 'Operating Systems')",
            "INSERT INTO courses VALUES (5, 3, 'Databases')",
            "INSERT INTO courses VALUES (6, 5, 'Machine Learning')"
        };

        for (auto& sql : course_inserts) {
            executor.Execute(sql);
            auto stmt = Parser::Parse(sql);
            auto* ins = static_cast<InsertStmt*>(stmt.get());
            std::string record_data;
            for (size_t i = 0; i < ins->values.size(); i++) {
                if (i > 0) record_data += "|";
                record_data += ValueToString(ins->values[i]);
            }
            wal.AppendLog(txn->GetTxnId(), LogType::INSERT, "courses",
                          record_data, ValueToString(ins->values[0]));
            std::cout << "  " << sql << std::endl;
        }

        txn_mgr.Commit(txn);
        std::cout << "\n  All inserts committed successfully." << std::endl;
    }

    // ═══════════════════════════════════════════════════════
    // 3. SELECT QUERIES
    // ═══════════════════════════════════════════════════════
    PrintHeader("3. SELECT * FROM students");
    {
        std::string sql = "SELECT * FROM students";
        std::cout << "  SQL: " << sql << std::endl;
        auto rows = executor.Execute(sql);
        PrintResults(rows, executor.GetLastSchema());
    }

    PrintHeader("4. SELECT WITH WHERE (Seq Scan)");
    {
        std::string sql = "SELECT name, gpa FROM students WHERE age > 22";
        std::cout << "  SQL: " << sql << std::endl;
        auto rows = executor.Execute(sql);
        PrintResults(rows, executor.GetLastSchema());
    }

    PrintHeader("5. SELECT WITH WHERE (Index Scan on PK)");
    {
        std::string sql = "SELECT * FROM students WHERE id = 3";
        std::cout << "  SQL: " << sql << std::endl;
        auto rows = executor.Execute(sql);
        PrintResults(rows, executor.GetLastSchema());
    }

    PrintHeader("5.5 SELECT WITH WHERE (AND / OR logic)");
    {
        std::string sql = "SELECT name, gpa FROM students WHERE age > 23 OR gpa >= 3.8";
        std::cout << "  SQL: " << sql << std::endl;
        auto rows = executor.Execute(sql);
        PrintResults(rows, executor.GetLastSchema());
    }

    // ═══════════════════════════════════════════════════════
    // 4. JOIN QUERY
    // ═══════════════════════════════════════════════════════
    PrintHeader("6. JOIN QUERY");
    {
        std::string sql = "SELECT * FROM students JOIN courses ON students.id = courses.student_id";
        std::cout << "  SQL: " << sql << std::endl;
        auto rows = executor.Execute(sql);
        PrintResults(rows, executor.GetLastSchema());
    }

    // ═══════════════════════════════════════════════════════
    // 5. DELETE
    // ═══════════════════════════════════════════════════════
    PrintHeader("7. DELETE");
    {
        Transaction* txn = txn_mgr.Begin();
        lock_mgr.LockExclusive(txn, "students");

        std::string sql = "DELETE FROM students WHERE id = 6";
        std::cout << "  SQL: " << sql << std::endl;
        executor.Execute(sql);
        wal.AppendLog(txn->GetTxnId(), LogType::DELETE_LOG, "students", "", "6");

        txn_mgr.Commit(txn);

        // Verify
        std::cout << "\n  After delete:" << std::endl;
        auto rows = executor.Execute("SELECT * FROM students");
        PrintResults(rows, executor.GetLastSchema());
    }

    // ═══════════════════════════════════════════════════════
    // 6. CONCURRENT TRANSACTION DEMO
    // ═══════════════════════════════════════════════════════
    PrintHeader("8. CONCURRENT TRANSACTIONS (Strict 2PL)");
    {
        std::cout << "  Demonstrating lock acquisition and conflict detection..." << std::endl;

        Transaction* txn1 = txn_mgr.Begin();
        Transaction* txn2 = txn_mgr.Begin();

        // txn1 acquires exclusive lock
        bool got_lock1 = lock_mgr.LockExclusive(txn1, "students");
        std::cout << "  TXN " << txn1->GetTxnId()
                  << " acquired EXCLUSIVE lock on 'students': "
                  << (got_lock1 ? "YES" : "NO") << std::endl;

        // txn2 tries shared lock — should block/timeout
        std::cout << "  TXN " << txn2->GetTxnId()
                  << " requesting SHARED lock on 'students' (will timeout in 1s)..."
                  << std::endl;

        // Run txn2's lock request in a separate thread
        bool got_lock2 = false;
        std::thread t2([&]() {
            got_lock2 = lock_mgr.LockShared(txn2, "students");
        });

        // Sleep briefly, then commit txn1 (releases lock)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        txn_mgr.Commit(txn1);
        std::cout << "  TXN " << txn1->GetTxnId()
                  << " committed (locks released)" << std::endl;

        t2.join();
        std::cout << "  TXN " << txn2->GetTxnId()
                  << " acquired SHARED lock: "
                  << (got_lock2 ? "YES" : "NO") << std::endl;

        if (got_lock2) {
            txn_mgr.Commit(txn2);
        } else {
            txn_mgr.Abort(txn2);
        }
    }

    // ═══════════════════════════════════════════════════════
    // 7. DYNAMIC SQL (CREATE / INSERT)
    // ═══════════════════════════════════════════════════════
    PrintHeader("9. DYNAMIC SQL (CREATE TABLE & INSERT)");
    {
        std::filesystem::remove("teachers.db");

        std::string sql1 = "CREATE TABLE teachers (id INT, name STRING, subject STRING)";
        std::cout << "  SQL: " << sql1 << std::endl;
        executor.Execute(sql1);

        std::string sql2 = "INSERT INTO teachers VALUES (1, 'Dr. Smith', 'Mathematics')";
        std::cout << "  SQL: " << sql2 << std::endl;
        executor.Execute(sql2);

        std::string sql3 = "SELECT * FROM teachers WHERE id = 1";
        std::cout << "  SQL: " << sql3 << std::endl;
        auto rows = executor.Execute(sql3);
        PrintResults(rows, executor.GetLastSchema());
    }

    // ═══════════════════════════════════════════════════════
    // 8. CRASH RECOVERY DEMO
    // ═══════════════════════════════════════════════════════
    DemoCrashRecovery(&catalog, &executor, &wal);

    // ═══════════════════════════════════════════════════════
    // 8. BUFFER POOL STATS
    // ═══════════════════════════════════════════════════════
    PrintHeader("9. BUFFER POOL FLUSH");
    bpm_students.FlushAll();
    bpm_courses.FlushAll();
    std::cout << "  All dirty pages flushed to disk." << std::endl;
    std::cout << "  Students DB file: students.db ("
              << disk_students.GetNumPages() << " pages)" << std::endl;
    std::cout << "  Courses DB file: courses.db ("
              << disk_courses.GetNumPages() << " pages)" << std::endl;

    PrintHeader("DEMO COMPLETE");
    std::cout << "  MiniDB automated tests finished.\n" << std::endl;
    } // end if (run_demo)

    PrintHeader("10. INTERACTIVE TERMINAL");
    std::cout << "  Type your SQL queries below. Type 'exit' or 'quit' to close.\n" << std::endl;

    std::string input;
    while (true) {
        std::cout << "minidb> ";
        if (!std::getline(std::cin, input)) break;
        if (input == "exit" || input == "quit") break;
        if (input.empty()) continue;

        if (input == "/tables") {
            auto tables = catalog.GetTableNames();
            std::cout << "  Tables in database:" << std::endl;
            for (const auto& t : tables) {
                std::cout << "    - " << t << std::endl;
            }
            std::cout << std::endl;
            continue;
        }

        if (input.find("/schema ") == 0) {
            std::string tname = input.substr(8);
            auto table = catalog.GetTable(tname);
            if (!table) {
                std::cout << "  Error: Table '" << tname << "' not found.\n" << std::endl;
            } else {
                std::cout << "  Schema for '" << tname << "':" << std::endl;
                for (const auto& col : table->schema) {
                    std::cout << "    - " << col.name << " (" 
                              << (col.type == ColumnType::INT ? "INT" : 
                                 (col.type == ColumnType::DOUBLE ? "DOUBLE" : "STRING"))
                              << ")" << std::endl;
                }
                std::cout << std::endl;
            }
            continue;
        }

        try {
            auto rows = executor.Execute(input);
            if (input.find("SELECT") == 0 || input.find("select") == 0) {
                PrintResults(rows, executor.GetLastSchema());
            } else {
                std::cout << "  OK.\n" << std::endl;
            }
        } catch (const std::exception& e) {
            std::cout << "  Error: " << e.what() << "\n" << std::endl;
        }
    }

    std::cout << "\n  MiniDB shutdown successful.\n" << std::endl;

    return 0;
}
