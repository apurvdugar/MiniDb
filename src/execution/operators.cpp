#include "execution/operators.h"
#include <algorithm>

namespace minidb {

// ═══════════════════════════════════════════════════════════
// Helper: compare two Values
// ═══════════════════════════════════════════════════════════

static bool CompareValues(const Value& lhs, const Value& rhs, CmpOp op) {
    // Both must be the same type for meaningful comparison
    if (lhs.index() != rhs.index()) {
        // Try int vs double comparison
        double dl = 0, dr = 0;
        if (std::holds_alternative<int64_t>(lhs)) dl = static_cast<double>(std::get<int64_t>(lhs));
        else if (std::holds_alternative<double>(lhs)) dl = std::get<double>(lhs);
        else return op == CmpOp::NEQ; // string vs number

        if (std::holds_alternative<int64_t>(rhs)) dr = static_cast<double>(std::get<int64_t>(rhs));
        else if (std::holds_alternative<double>(rhs)) dr = std::get<double>(rhs);
        else return op == CmpOp::NEQ;

        switch (op) {
            case CmpOp::EQ:  return dl == dr;
            case CmpOp::NEQ: return dl != dr;
            case CmpOp::LT:  return dl < dr;
            case CmpOp::LTE: return dl <= dr;
            case CmpOp::GT:  return dl > dr;
            case CmpOp::GTE: return dl >= dr;
        }
    }

    if (std::holds_alternative<int64_t>(lhs)) {
        int64_t l = std::get<int64_t>(lhs), r = std::get<int64_t>(rhs);
        switch (op) {
            case CmpOp::EQ:  return l == r;
            case CmpOp::NEQ: return l != r;
            case CmpOp::LT:  return l < r;
            case CmpOp::LTE: return l <= r;
            case CmpOp::GT:  return l > r;
            case CmpOp::GTE: return l >= r;
        }
    } else if (std::holds_alternative<double>(lhs)) {
        double l = std::get<double>(lhs), r = std::get<double>(rhs);
        switch (op) {
            case CmpOp::EQ:  return l == r;
            case CmpOp::NEQ: return l != r;
            case CmpOp::LT:  return l < r;
            case CmpOp::LTE: return l <= r;
            case CmpOp::GT:  return l > r;
            case CmpOp::GTE: return l >= r;
        }
    } else {
        const auto& l = std::get<std::string>(lhs);
        const auto& r = std::get<std::string>(rhs);
        switch (op) {
            case CmpOp::EQ:  return l == r;
            case CmpOp::NEQ: return l != r;
            case CmpOp::LT:  return l < r;
            case CmpOp::LTE: return l <= r;
            case CmpOp::GT:  return l > r;
            case CmpOp::GTE: return l >= r;
        }
    }
    return false;
}

// ═══════════════════════════════════════════════════════════
// SeqScanOperator (Batch)
// ═══════════════════════════════════════════════════════════

SeqScanOperator::SeqScanOperator(TableInfo* table) : table_(table) {}

void SeqScanOperator::Open() {
    cursor_ = 0;
    all_rows_.clear();
    table_->heap_file->ScanAll([&](const RecordId& rid, const Row& row) {
        all_rows_.push_back({rid, row});
    });
}

TupleBatch SeqScanOperator::Next() {
    TupleBatch batch;
    while (cursor_ < all_rows_.size() && !batch.IsFull()) {
        batch.AddRow(all_rows_[cursor_].second);
        cursor_++;
    }
    return batch;
}

void SeqScanOperator::Close() {
    all_rows_.clear();
}

Schema SeqScanOperator::GetOutputSchema() const {
    return table_->schema;
}

// ═══════════════════════════════════════════════════════════
// IndexScanOperator (Batch)
// ═══════════════════════════════════════════════════════════

IndexScanOperator::IndexScanOperator(TableInfo* table, int64_t key)
    : table_(table), is_range_(false), key_lo_(key), key_hi_(key) {}

IndexScanOperator::IndexScanOperator(TableInfo* table, int64_t lo, int64_t hi)
    : table_(table), is_range_(true), key_lo_(lo), key_hi_(hi) {}

void IndexScanOperator::Open() {
    done_ = false;
    result_rows_.clear();

    if (!table_->primary_index) return;

    if (is_range_) {
        auto rids = table_->primary_index->RangeSearch(key_lo_, key_hi_);
        for (auto& rid : rids) {
            Row row;
            if (table_->heap_file->GetTuple(rid, row)) {
                result_rows_.push_back(std::move(row));
            }
        }
    } else {
        auto rid = table_->primary_index->Search(key_lo_);
        if (rid.has_value()) {
            Row row;
            if (table_->heap_file->GetTuple(rid.value(), row)) {
                result_rows_.push_back(std::move(row));
            }
        }
    }
}

TupleBatch IndexScanOperator::Next() {
    TupleBatch batch;
    if (done_) return batch;

    for (auto& row : result_rows_) {
        batch.AddRow(std::move(row));
    }
    done_ = true;
    return batch;
}

void IndexScanOperator::Close() {
    result_rows_.clear();
}

Schema IndexScanOperator::GetOutputSchema() const {
    return table_->schema;
}

// ═══════════════════════════════════════════════════════════
// FilterOperator (Batch)
// ═══════════════════════════════════════════════════════════

FilterOperator::FilterOperator(std::unique_ptr<Operator> child,
                               const WhereClause& where_clause,
                               const Schema& schema)
    : child_(std::move(child)), where_clause_(where_clause), schema_(schema) {}

bool FilterOperator::EvalPredicate(const Predicate& pred, const Row& row,
                                   const Schema& schema) {
    // Find column index
    int col_idx = -1;
    // Handle dotted names (table.column)
    std::string col_name = pred.column;
    auto dot_pos = col_name.find('.');
    if (dot_pos != std::string::npos) {
        col_name = col_name.substr(dot_pos + 1);
    }

    for (size_t i = 0; i < schema.size(); i++) {
        std::string schema_col = schema[i].name;
        auto schema_dot = schema_col.find('.');
        if (schema_dot != std::string::npos) {
            schema_col = schema_col.substr(schema_dot + 1);
        }
        if (schema_col == col_name) {
            col_idx = static_cast<int>(i);
            break;
        }
    }
    if (col_idx < 0 || col_idx >= static_cast<int>(row.size())) return false;

    return CompareValues(row[col_idx], pred.value, pred.op);
}

bool FilterOperator::EvalWhereClause(const WhereClause& clause, const Row& row, const Schema& schema) {
    if (clause.IsEmpty()) return true;

    if (clause.logic == LogicOp::AND) {
        for (auto& pred : clause.preds) {
            if (!EvalPredicate(pred, row, schema)) {
                return false;
            }
        }
        return true;
    } else {
        // LogicOp::OR
        for (auto& pred : clause.preds) {
            if (EvalPredicate(pred, row, schema)) {
                return true;
            }
        }
        return false;
    }
}

void FilterOperator::Open() {
    child_->Open();
}

TupleBatch FilterOperator::Next() {
    while (true) {
        TupleBatch input = child_->Next();
        if (input.IsEmpty()) return input;

        TupleBatch output;
        for (auto& row : input.rows) {
            if (EvalWhereClause(where_clause_, row, schema_)) {
                output.AddRow(std::move(row));
            }
        }
        if (!output.IsEmpty()) return output;
        // If all rows filtered out, get next batch
    }
}

void FilterOperator::Close() {
    child_->Close();
}

Schema FilterOperator::GetOutputSchema() const {
    return schema_;
}

// ═══════════════════════════════════════════════════════════
// NestedLoopJoinOperator (Batch)
// ═══════════════════════════════════════════════════════════

NestedLoopJoinOperator::NestedLoopJoinOperator(
    std::unique_ptr<Operator> left, std::unique_ptr<Operator> right,
    const JoinCondition& cond, const Schema& left_schema,
    const Schema& right_schema)
    : left_(std::move(left)), right_(std::move(right)),
      cond_(cond), left_schema_(left_schema), right_schema_(right_schema) {
    // Build output schema: all left columns + all right columns
    // Prefix with table name to avoid ambiguity
    for (auto& col : left_schema_) {
        output_schema_.push_back(col);
    }
    for (auto& col : right_schema_) {
        output_schema_.push_back(col);
    }
}

int NestedLoopJoinOperator::FindCol(const Schema& schema,
                                     const std::string& col_name) const {
    for (size_t i = 0; i < schema.size(); i++) {
        std::string s = schema[i].name;
        auto dot = s.find('.');
        if (dot != std::string::npos) s = s.substr(dot + 1);

        std::string c = col_name;
        auto dot2 = c.find('.');
        if (dot2 != std::string::npos) c = c.substr(dot2 + 1);

        if (s == c) return static_cast<int>(i);
    }
    return -1;
}

void NestedLoopJoinOperator::Open() {
    left_->Open();
    right_->Open();

    // Materialize right side
    right_rows_.clear();
    while (true) {
        TupleBatch batch = right_->Next();
        if (batch.IsEmpty()) break;
        for (auto& row : batch.rows) {
            right_rows_.push_back(std::move(row));
        }
    }

    // Get first left batch
    left_batch_ = left_->Next();
    left_idx_ = 0;
    right_idx_ = 0;
    left_exhausted_ = left_batch_.IsEmpty();
}

TupleBatch NestedLoopJoinOperator::Next() {
    TupleBatch output;

    int left_col = FindCol(left_schema_, cond_.left_column);
    int right_col = FindCol(right_schema_, cond_.right_column);

    while (!output.IsFull()) {
        if (left_exhausted_) return output;

        if (left_idx_ >= left_batch_.Size()) {
            left_batch_ = left_->Next();
            if (left_batch_.IsEmpty()) {
                left_exhausted_ = true;
                return output;
            }
            left_idx_ = 0;
            right_idx_ = 0;
        }

        const Row& left_row = left_batch_.rows[left_idx_];

        while (right_idx_ < right_rows_.size() && !output.IsFull()) {
            const Row& right_row = right_rows_[right_idx_];
            right_idx_++;

            // Check join condition
            bool match = false;
            if (left_col >= 0 && right_col >= 0 &&
                left_col < static_cast<int>(left_row.size()) &&
                right_col < static_cast<int>(right_row.size())) {
                match = CompareValues(left_row[left_col], right_row[right_col],
                                      CmpOp::EQ);
            }

            if (match) {
                // Concatenate rows
                Row joined;
                joined.insert(joined.end(), left_row.begin(), left_row.end());
                joined.insert(joined.end(), right_row.begin(), right_row.end());
                output.AddRow(std::move(joined));
            }
        }

        if (right_idx_ >= right_rows_.size()) {
            left_idx_++;
            right_idx_ = 0;
        }
    }

    return output;
}

void NestedLoopJoinOperator::Close() {
    left_->Close();
    right_->Close();
    right_rows_.clear();
}

Schema NestedLoopJoinOperator::GetOutputSchema() const {
    return output_schema_;
}

// ═══════════════════════════════════════════════════════════
// ProjectionOperator (Batch)
// ═══════════════════════════════════════════════════════════

ProjectionOperator::ProjectionOperator(std::unique_ptr<Operator> child,
                                       const std::vector<std::string>& columns,
                                       const Schema& input_schema)
    : child_(std::move(child)), columns_(columns), input_schema_(input_schema) {
    // If "*", select all columns
    if (columns.size() == 1 && columns[0] == "*") {
        output_schema_ = input_schema_;
        for (int i = 0; i < static_cast<int>(input_schema_.size()); i++) {
            col_indices_.push_back(i);
        }
    } else {
        for (auto& col : columns) {
            for (int i = 0; i < static_cast<int>(input_schema_.size()); i++) {
                std::string s = input_schema_[i].name;
                auto dot = s.find('.');
                if (dot != std::string::npos) s = s.substr(dot + 1);

                std::string c = col;
                auto dot2 = c.find('.');
                if (dot2 != std::string::npos) c = c.substr(dot2 + 1);

                if (s == c) {
                    col_indices_.push_back(i);
                    output_schema_.push_back(input_schema_[i]);
                    break;
                }
            }
        }
    }
}

void ProjectionOperator::Open() {
    child_->Open();
}

TupleBatch ProjectionOperator::Next() {
    TupleBatch input = child_->Next();
    if (input.IsEmpty()) return input;

    TupleBatch output;
    for (auto& row : input.rows) {
        Row projected;
        for (int idx : col_indices_) {
            if (idx < static_cast<int>(row.size())) {
                projected.push_back(row[idx]);
            }
        }
        output.AddRow(std::move(projected));
    }
    return output;
}

void ProjectionOperator::Close() {
    child_->Close();
}

Schema ProjectionOperator::GetOutputSchema() const {
    return output_schema_;
}

// ═══════════════════════════════════════════════════════════
// Row-at-a-time operators (for benchmark comparison)
// ═══════════════════════════════════════════════════════════

SeqScanRowOperator::SeqScanRowOperator(TableInfo* table) : table_(table) {}

void SeqScanRowOperator::Open() {
    cursor_ = 0;
    all_rows_.clear();
    table_->heap_file->ScanAll([&](const RecordId&, const Row& row) {
        all_rows_.push_back(row);
    });
}

bool SeqScanRowOperator::Next(Row& out) {
    if (cursor_ >= all_rows_.size()) return false;
    out = all_rows_[cursor_++];
    return true;
}

void SeqScanRowOperator::Close() { all_rows_.clear(); }

Schema SeqScanRowOperator::GetOutputSchema() const {
    return table_->schema;
}

FilterRowOperator::FilterRowOperator(std::unique_ptr<RowOperator> child,
                                     const WhereClause& where_clause,
                                     const Schema& schema)
    : child_(std::move(child)), where_clause_(where_clause), schema_(schema) {}

void FilterRowOperator::Open() { child_->Open(); }

bool FilterRowOperator::Next(Row& out) {
    while (child_->Next(out)) {
        if (FilterOperator::EvalWhereClause(where_clause_, out, schema_)) {
            return true;
        }
    }
    return false;
}

void FilterRowOperator::Close() { child_->Close(); }

Schema FilterRowOperator::GetOutputSchema() const { return schema_; }

} // namespace minidb
