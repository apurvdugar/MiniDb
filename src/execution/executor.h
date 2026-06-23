#pragma once

#include "common/types.h"
#include "execution/ast.h"
#include "execution/operators.h"
#include "catalog/catalog.h"

#include <memory>
#include <string>
#include <vector>

namespace minidb {

// Forward declarations
class Optimizer;

/*
 * Executor: takes a SQL string, parses it, optimizes it, builds an operator
 * tree, and executes it.
 */
class Executor {
public:
    Executor(Catalog* catalog, Optimizer* optimizer);

    // Execute a SQL statement. Returns result rows (for SELECT) or empty (for INSERT/DELETE).
    std::vector<Row> Execute(const std::string& sql);

    // Get the schema of the last result
    Schema GetLastSchema() const { return last_schema_; }

private:
    Catalog*    catalog_;
    Optimizer*  optimizer_;
    Schema      last_schema_;

    std::vector<Row> ExecuteSelect(SelectStmt* stmt);
    std::vector<Row> ExecuteInsert(InsertStmt* stmt);
    std::vector<Row> ExecuteDelete(DeleteStmt* stmt);
    std::vector<Row> ExecuteCreate(CreateTableStmt* stmt);

private:
    struct TableStorage {
        std::unique_ptr<minidb::DiskManager> disk;
        std::unique_ptr<minidb::BufferPoolManager> bpm;
    };
    std::unordered_map<std::string, std::unique_ptr<TableStorage>> storage_;
};

} // namespace minidb
