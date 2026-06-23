/*
 * MiniDB Benchmark — Track A: Vectorized/Batch vs Row-at-a-Time
 *
 * Compares performance of batch (vectorized) operators against
 * traditional row-at-a-time (Volcano) operators.
 *
 * Methodology:
 *  We pre-load rows into memory, then measure ONLY the filter + iteration
 *  cost, excluding scan materialization. This isolates the execution model
 *  difference: batch (128 rows per Next()) vs row (1 row per Next()).
 */

#include "common/types.h"
#include "storage/disk_manager.h"
#include "storage/buffer_pool.h"
#include "storage/heap_file.h"
#include "catalog/catalog.h"
#include "execution/operators.h"
#include "execution/ast.h"
#include "execution/batch.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using namespace minidb;

static void PrintBenchSeparator() {
    std::cout << std::string(78, '-') << std::endl;
}

// ── In-memory filter benchmarks (no disk I/O) ──────────────

/*
 * Batch filter: process pre-loaded rows in batches of BATCH_SIZE.
 * Returns the number of matching rows.
 */
static int BatchFilter(const std::vector<Row>& data, const Schema& schema,
                       const WhereClause& clause) {
    int result_count = 0;
    size_t cursor = 0;

    while (cursor < data.size()) {
        // Build a batch
        TupleBatch batch;
        while (cursor < data.size() && !batch.IsFull()) {
            batch.AddRow(data[cursor]);
            cursor++;
        }

        // Filter the batch
        for (auto& row : batch.rows) {
            if (FilterOperator::EvalWhereClause(clause, row, schema)) {
                result_count++;
            }
        }
    }

    return result_count;
}

/*
 * Row-at-a-time filter: process pre-loaded rows one by one.
 * Returns the number of matching rows.
 */
static int RowFilter(const std::vector<Row>& data, const Schema& schema,
                     const WhereClause& clause) {
    int result_count = 0;

    for (size_t i = 0; i < data.size(); i++) {
        const Row& row = data[i];
        if (FilterOperator::EvalWhereClause(clause, row, schema)) {
            result_count++;
        }
    }

    return result_count;
}

/*
 * Full pipeline benchmark: includes scan + filter through operator tree.
 */
static std::pair<long long, long long> RunFullPipelineBenchmark(int num_rows) {
    std::string db_file = "bench_" + std::to_string(num_rows) + ".db";
    std::filesystem::remove(db_file);

    long long batch_us = 0, row_us = 0;

    {
        DiskManager disk(db_file);
        BufferPoolManager bpm(256, &disk);

        Schema schema = {
            {"id",    ColumnType::INT},
            {"value", ColumnType::INT},
            {"name",  ColumnType::STRING}
        };

        Catalog catalog;
        TableInfo* table = catalog.CreateTable("bench", schema, &bpm);

        for (int i = 0; i < num_rows; i++) {
            Row row = {
                static_cast<int64_t>(i),
                static_cast<int64_t>(std::rand() % 1000),
                std::string("name_") + std::to_string(i)
            };
            RecordId rid = table->heap_file->InsertTuple(row);
            table->primary_index->Insert(static_cast<int64_t>(i), rid);
        }

        WhereClause clause;
        clause.preds = {{ "value", CmpOp::GT, static_cast<int64_t>(500) }};

        const int ITERS = 20;

        // Batch pipeline
        {
            auto start = std::chrono::high_resolution_clock::now();
            for (int iter = 0; iter < ITERS; iter++) {
                auto scan = std::make_unique<SeqScanOperator>(table);
                auto filter = std::make_unique<FilterOperator>(
                    std::move(scan), clause, schema);
                filter->Open();
                while (true) {
                    TupleBatch b = filter->Next();
                    if (b.IsEmpty()) break;
                }
                filter->Close();
            }
            auto end = std::chrono::high_resolution_clock::now();
            batch_us = std::chrono::duration_cast<std::chrono::microseconds>(
                end - start).count() / ITERS;
        }

        // Row pipeline
        {
            auto start = std::chrono::high_resolution_clock::now();
            for (int iter = 0; iter < ITERS; iter++) {
                auto scan = std::make_unique<SeqScanRowOperator>(table);
                auto filter = std::make_unique<FilterRowOperator>(
                    std::move(scan), clause, schema);
                filter->Open();
                Row r;
                while (filter->Next(r)) {}
                filter->Close();
            }
            auto end = std::chrono::high_resolution_clock::now();
            row_us = std::chrono::duration_cast<std::chrono::microseconds>(
                end - start).count() / ITERS;
        }

        bpm.FlushAll();
    }

    std::filesystem::remove(db_file);
    return {batch_us, row_us};
}

int main() {
    std::cout << R"(
    +----------------------------------------------------------+
    |     MiniDB Benchmark -- Track A Performance Comparison   |
    |         Batch/Vectorized vs Row-at-a-Time Execution      |
    +----------------------------------------------------------+
    )" << std::endl;

    std::srand(42);

    // ═════════════════════════════════════════════════════════
    // Benchmark 1: Pure filter (in-memory, no disk I/O)
    // ═════════════════════════════════════════════════════════
    std::cout << " +------------------------------------------------------+" << std::endl;
    std::cout << " |  Benchmark 1: Pure Filter (in-memory, no I/O)        |" << std::endl;
    std::cout << " |  Measures only predicate evaluation overhead         |" << std::endl;
    std::cout << " +------------------------------------------------------+\n" << std::endl;

    Schema schema = {
        {"id",    ColumnType::INT},
        {"value", ColumnType::INT},
        {"name",  ColumnType::STRING}
    };

    WhereClause clause;
    clause.preds = {{ "value", CmpOp::GT, static_cast<int64_t>(500) }};

    std::vector<int> workloads = {1000, 5000, 10000, 50000, 100000};

    struct FilterResult {
        int num_rows;
        long long batch_us;
        long long row_us;
        double speedup;
    };
    std::vector<FilterResult> filter_results;

    for (int n : workloads) {
        // Generate in-memory data
        std::vector<Row> data;
        data.reserve(n);
        for (int i = 0; i < n; i++) {
            data.push_back({
                static_cast<int64_t>(i),
                static_cast<int64_t>(std::rand() % 1000),
                std::string("name_") + std::to_string(i)
            });
        }

        const int ITERS = 200;

        // Batch filter
        long long batch_us;
        {
            auto start = std::chrono::high_resolution_clock::now();
            int count = 0;
            for (int iter = 0; iter < ITERS; iter++) {
                count = BatchFilter(data, schema, clause);
            }
            auto end = std::chrono::high_resolution_clock::now();
            batch_us = std::chrono::duration_cast<std::chrono::microseconds>(
                end - start).count() / ITERS;
            (void)count;
        }

        // Row filter
        long long row_us;
        {
            auto start = std::chrono::high_resolution_clock::now();
            int count = 0;
            for (int iter = 0; iter < ITERS; iter++) {
                count = RowFilter(data, schema, clause);
            }
            auto end = std::chrono::high_resolution_clock::now();
            row_us = std::chrono::duration_cast<std::chrono::microseconds>(
                end - start).count() / ITERS;
            (void)count;
        }

        double speedup = (batch_us > 0) ? static_cast<double>(row_us) / batch_us : 0;
        filter_results.push_back({n, batch_us, row_us, speedup});
    }

    PrintBenchSeparator();
    std::cout << std::left
              << std::setw(12) << "  Rows"
              << std::setw(18) << "Batch (us)"
              << std::setw(18) << "Row (us)"
              << std::setw(12) << "Speedup"
              << std::endl;
    PrintBenchSeparator();

    for (auto& r : filter_results) {
        std::cout << "  "
                  << std::left << std::setw(11) << r.num_rows
                  << std::setw(18) << r.batch_us
                  << std::setw(18) << r.row_us
                  << std::fixed << std::setprecision(2)
                  << std::setw(12) << r.speedup
                  << std::endl;
    }
    PrintBenchSeparator();

    // ═════════════════════════════════════════════════════════
    // Benchmark 2: Full pipeline (scan + filter through operators)
    // ═════════════════════════════════════════════════════════
    std::cout << "\n  +------------------------------------------------------+" << std::endl;
    std::cout << "  |  Benchmark 2: Full Pipeline (scan + filter via ops)   |" << std::endl;
    std::cout << "  |  End-to-end including heap scan overhead              |" << std::endl;
    std::cout << "  +------------------------------------------------------+\n" << std::endl;

    std::vector<int> pipeline_workloads = {1000, 5000, 10000, 50000};

    struct PipelineResult {
        int num_rows;
        long long batch_us;
        long long row_us;
        double speedup;
        double batch_tp;
        double row_tp;
    };
    std::vector<PipelineResult> pipeline_results;

    for (int n : pipeline_workloads) {
        std::cout << "  Running pipeline benchmark with " << n << " rows..." << std::endl;

        // Warmup
        RunFullPipelineBenchmark(n);

        // Measure
        long long total_b = 0, total_r = 0;
        for (int i = 0; i < 3; i++) {
            auto [b, r] = RunFullPipelineBenchmark(n);
            total_b += b;
            total_r += r;
        }
        long long avg_b = total_b / 3;
        long long avg_r = total_r / 3;

        double speedup = (avg_b > 0) ? static_cast<double>(avg_r) / avg_b : 0;
        double b_tp = (avg_b > 0) ? static_cast<double>(n) / avg_b * 1e6 : 0;
        double r_tp = (avg_r > 0) ? static_cast<double>(n) / avg_r * 1e6 : 0;

        pipeline_results.push_back({n, avg_b, avg_r, speedup, b_tp, r_tp});
    }

    std::cout << "\n";
    PrintBenchSeparator();
    std::cout << std::left
              << std::setw(10) << "  Rows"
              << std::setw(15) << "Batch (us)"
              << std::setw(15) << "Row (us)"
              << std::setw(12) << "Speedup"
              << std::setw(16) << "Batch (rows/s)"
              << std::setw(16) << "Row (rows/s)"
              << std::endl;
    PrintBenchSeparator();

    for (auto& r : pipeline_results) {
        std::cout << "  "
                  << std::left << std::setw(9) << r.num_rows
                  << std::setw(15) << r.batch_us
                  << std::setw(15) << r.row_us
                  << std::fixed << std::setprecision(2)
                  << std::setw(12) << r.speedup
                  << std::setprecision(0)
                  << std::setw(16) << r.batch_tp
                  << std::setw(16) << r.row_tp
                  << std::endl;
    }
    PrintBenchSeparator();

    // Analysis
    std::cout << "\n  Analysis:" << std::endl;
    std::cout << "  ---------" << std::endl;
    std::cout << "  Benchmark 1 isolates the filter evaluation cost. Batch mode" << std::endl;
    std::cout << "  processes " << BATCH_SIZE << " rows per iteration, reducing loop overhead." << std::endl;
    std::cout << "  " << std::endl;
    std::cout << "  Benchmark 2 includes the full scan+filter pipeline where heap" << std::endl;
    std::cout << "  scan materialization dominates the cost for both execution models." << std::endl;
    std::cout << "  " << std::endl;
    std::cout << "  Key observations:" << std::endl;
    std::cout << "    - Batch processing amortizes overhead across " << BATCH_SIZE << " rows" << std::endl;
    std::cout << "    - The benefit is most visible in filter-heavy workloads" << std::endl;
    std::cout << "    - Production vectorized engines gain more with columnar storage" << std::endl;
    std::cout << "      and SIMD predicate evaluation on tight column arrays" << std::endl;
    std::cout << std::endl;

    return 0;
}
