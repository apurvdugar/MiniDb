#include "execution/executor.h"
#include "execution/parser.h"
#include "optimizer/optimizer.h"
#include <iostream>
#include <stdexcept>

namespace minidb {

Executor::Executor(Catalog* catalog, Optimizer* optimizer)
    : catalog_(catalog), optimizer_(optimizer) {}

std::vector<Row> Executor::Execute(const std::string& sql) {
    auto stmt = Parser::Parse(sql);

    switch (stmt->type) {
        case StmtType::SELECT:
            return ExecuteSelect(static_cast<SelectStmt*>(stmt.get()));
        case StmtType::INSERT:
            return ExecuteInsert(static_cast<InsertStmt*>(stmt.get()));
        case StmtType::DELETE_STMT:
            return ExecuteDelete(static_cast<DeleteStmt*>(stmt.get()));
        case StmtType::CREATE_TABLE:
            return ExecuteCreate(static_cast<CreateTableStmt*>(stmt.get()));
    }
    return {};
}

std::vector<Row> Executor::ExecuteSelect(SelectStmt* stmt) {
    std::vector<Row> results;

    if (stmt->has_join) {
        // ── JOIN query ──────────────────────────────────
        auto [left_name, right_name] =
            optimizer_->DecideJoinOrder(stmt->table_name, stmt->join_table, catalog_);

        TableInfo* left_table  = catalog_->GetTable(left_name);
        TableInfo* right_table = catalog_->GetTable(right_name);
        if (!left_table || !right_table) {
            throw std::runtime_error("Table not found for JOIN");
        }

        // Remap join condition if optimizer swapped table order.
        // The parsed condition has left/right referring to the SQL's original
        // table order. After the optimizer swap we must ensure that
        // join_cond.left_column belongs to left_name's schema and
        // join_cond.right_column belongs to right_name's schema.
        JoinCondition join_cond = stmt->join_cond;
        if (join_cond.left_table != left_name) {
            // The optimizer swapped: parsed-left is now physical-right
            std::swap(join_cond.left_table,  join_cond.right_table);
            std::swap(join_cond.left_column, join_cond.right_column);
        }

        auto left_scan  = std::make_unique<SeqScanOperator>(left_table);
        auto right_scan = std::make_unique<SeqScanOperator>(right_table);

        auto join = std::make_unique<NestedLoopJoinOperator>(
            std::move(left_scan), std::move(right_scan),
            join_cond, left_table->schema, right_table->schema
        );

        std::unique_ptr<Operator> top = std::move(join);

        // Apply WHERE filter if present
        if (!stmt->where_clause.IsEmpty()) {
            auto filter_schema = top->GetOutputSchema();
            top = std::make_unique<FilterOperator>(
                std::move(top), stmt->where_clause, filter_schema
            );
        }

        // Apply projection
        auto proj_schema = top->GetOutputSchema();
        auto proj = std::make_unique<ProjectionOperator>(
            std::move(top), stmt->columns, proj_schema
        );

        last_schema_ = proj->GetOutputSchema();
        proj->Open();
        while (true) {
            TupleBatch batch = proj->Next();
            if (batch.IsEmpty()) break;
            for (auto& row : batch.rows) {
                results.push_back(std::move(row));
            }
        }
        proj->Close();

    } else {
        // ── Single table query ──────────────────────────
        TableInfo* table = catalog_->GetTable(stmt->table_name);
        if (!table) {
            throw std::runtime_error("Table not found: " + stmt->table_name);
        }

        // Ask optimizer for scan strategy
        ScanDecision scan = optimizer_->DecideScan(stmt, catalog_);

        std::unique_ptr<Operator> source;
        if (scan.scan_type == ScanType::INDEX_SCAN) {
            if (scan.is_range) {
                source = std::make_unique<IndexScanOperator>(
                    table, scan.index_key_lo, scan.index_key_hi);
            } else {
                source = std::make_unique<IndexScanOperator>(
                    table, scan.index_key_lo);
            }
            std::cout << "  [Optimizer] Using INDEX SCAN on '"
                      << stmt->table_name << "'" << std::endl;
        } else {
            source = std::make_unique<SeqScanOperator>(table);
            std::cout << "  [Optimizer] Using SEQ SCAN on '"
                      << stmt->table_name << "'" << std::endl;
        }

        std::unique_ptr<Operator> top = std::move(source);

        // Apply filters (for non-index predicates or additional predicates)
        if (!stmt->where_clause.IsEmpty()) {
            // For index scan with EQ on PK, skip the PK predicate in filter
            WhereClause remaining_clause;
            remaining_clause.logic = stmt->where_clause.logic;
            if (scan.scan_type == ScanType::INDEX_SCAN && stmt->where_clause.logic == LogicOp::AND) {
                std::string pk_name = table->schema[table->pk_col].name;
                for (auto& pred : stmt->where_clause.preds) {
                    if (pred.column != pk_name) {
                        remaining_clause.preds.push_back(pred);
                    }
                }
            } else {
                remaining_clause.preds = stmt->where_clause.preds;
            }

            if (!remaining_clause.IsEmpty()) {
                top = std::make_unique<FilterOperator>(
                    std::move(top), remaining_clause, table->schema);
            }
        }

        // Apply projection
        auto proj = std::make_unique<ProjectionOperator>(
            std::move(top), stmt->columns, table->schema);

        last_schema_ = proj->GetOutputSchema();
        proj->Open();
        while (true) {
            TupleBatch batch = proj->Next();
            if (batch.IsEmpty()) break;
            for (auto& row : batch.rows) {
                results.push_back(std::move(row));
            }
        }
        proj->Close();
    }

    return results;
}

std::vector<Row> Executor::ExecuteInsert(InsertStmt* stmt) {
    TableInfo* table = catalog_->GetTable(stmt->table_name);
    if (!table) {
        throw std::runtime_error("Table not found: " + stmt->table_name);
    }

    RecordId rid = table->heap_file->InsertTuple(stmt->values);
    if (rid.page_id == INVALID_PAGE_ID) {
        throw std::runtime_error("Insert failed — no space");
    }

    // Update primary index
    if (table->primary_index && !stmt->values.empty()) {
        if (std::holds_alternative<int64_t>(stmt->values[table->pk_col])) {
            int64_t key = std::get<int64_t>(stmt->values[table->pk_col]);
            table->primary_index->Insert(key, rid);
        }
    }

    return {};
}

std::vector<Row> Executor::ExecuteDelete(DeleteStmt* stmt) {
    TableInfo* table = catalog_->GetTable(stmt->table_name);
    if (!table) {
        throw std::runtime_error("Table not found: " + stmt->table_name);
    }

    // Collect records to delete
    std::vector<std::pair<RecordId, Row>> to_delete;
    table->heap_file->ScanAll([&](const RecordId& rid, const Row& row) {
        if (FilterOperator::EvalWhereClause(stmt->where_clause, row, table->schema)) {
            to_delete.push_back({rid, row});
        }
    });

    // Delete them
    for (auto& [rid, row] : to_delete) {
        table->heap_file->DeleteTuple(rid);

        // Remove from index
        if (table->primary_index && !row.empty()) {
            if (std::holds_alternative<int64_t>(row[table->pk_col])) {
                int64_t key = std::get<int64_t>(row[table->pk_col]);
                table->primary_index->Delete(key);
            }
        }
    }

    return {};
}

std::vector<Row> Executor::ExecuteCreate(CreateTableStmt* stmt) {
    if (catalog_->GetTable(stmt->table_name)) {
        throw std::runtime_error("Table already exists: " + stmt->table_name);
    }

    auto ts = std::make_unique<TableStorage>();
    ts->disk = std::make_unique<DiskManager>(stmt->table_name + ".db");
    ts->bpm = std::make_unique<BufferPoolManager>(BUFFER_POOL_SIZE, ts->disk.get());

    TableInfo* table = catalog_->CreateTable(stmt->table_name, stmt->columns, ts->bpm.get());
    if (!table) {
        throw std::runtime_error("Failed to create table in catalog");
    }

    storage_[stmt->table_name] = std::move(ts);
    return {};
}

} // namespace minidb
