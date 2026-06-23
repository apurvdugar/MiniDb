#include "execution/parser.h"
#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>

namespace minidb {

// ═══════════════════════════════════════════════════════════
// Utility helpers
// ═══════════════════════════════════════════════════════════

std::string Parser::Trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string Parser::ToUpper(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::toupper);
    return r;
}

std::vector<std::string> Parser::Split(const std::string& s, char delim) {
    std::vector<std::string> parts;
    std::istringstream iss(s);
    std::string token;
    while (std::getline(iss, token, delim)) {
        std::string t = Trim(token);
        if (!t.empty()) parts.push_back(t);
    }
    return parts;
}

Value Parser::ParseValue(const std::string& s) {
    std::string trimmed = Trim(s);

    // String literal (single or double quotes)
    if (trimmed.size() >= 2 &&
        ((trimmed.front() == '\'' && trimmed.back() == '\'') ||
         (trimmed.front() == '"' && trimmed.back() == '"'))) {
        return trimmed.substr(1, trimmed.size() - 2);
    }

    // Try integer
    try {
        size_t pos;
        int64_t iv = std::stoll(trimmed, &pos);
        if (pos == trimmed.size()) return iv;
    } catch (...) {}

    // Try double
    try {
        size_t pos;
        double dv = std::stod(trimmed, &pos);
        if (pos == trimmed.size()) return dv;
    } catch (...) {}

    // Default to string
    return trimmed;
}

Predicate Parser::ParsePredicate(const std::string& s) {
    Predicate pred;
    // Try operators in order (longest first to avoid partial matches)
    static const std::vector<std::pair<std::string, CmpOp>> ops = {
        {"<=", CmpOp::LTE}, {">=", CmpOp::GTE}, {"!=", CmpOp::NEQ},
        {"<>", CmpOp::NEQ}, {"<",  CmpOp::LT},  {">",  CmpOp::GT},
        {"=",  CmpOp::EQ}
    };

    for (auto& [opstr, op] : ops) {
        auto pos = s.find(opstr);
        if (pos != std::string::npos) {
            pred.column = Trim(s.substr(0, pos));
            pred.op     = op;
            pred.value  = ParseValue(s.substr(pos + opstr.size()));
            return pred;
        }
    }

    throw std::runtime_error("Cannot parse predicate: " + s);
}

ColumnType Parser::ParseColumnType(const std::string& type_str) {
    std::string upper = ToUpper(Trim(type_str));
    // Strip anything in parentheses, e.g. VARCHAR(255) → VARCHAR
    auto paren = upper.find('(');
    if (paren != std::string::npos) upper = upper.substr(0, paren);
    upper = Trim(upper);

    if (upper == "INT" || upper == "INTEGER" || upper == "BIGINT")
        return ColumnType::INT;
    if (upper == "DOUBLE" || upper == "FLOAT" || upper == "REAL" || upper == "DECIMAL")
        return ColumnType::DOUBLE;
    if (upper == "STRING" || upper == "TEXT" || upper == "VARCHAR" || upper == "CHAR")
        return ColumnType::STRING;

    throw std::runtime_error("Unknown column type: " + type_str);
}

// ═══════════════════════════════════════════════════════════
// WHERE clause parser (handles AND / OR)
// ═══════════════════════════════════════════════════════════

WhereClause Parser::ParseWhereClause(const std::string& where_str) {
    WhereClause clause;
    std::string upper = ToUpper(where_str);

    // Detect logic operator: check if OR appears (outside quotes)
    // Simple heuristic: if " OR " found, split on OR; else split on AND
    bool has_or  = (upper.find(" OR ") != std::string::npos);
    bool has_and = (upper.find(" AND ") != std::string::npos);

    if (has_or && !has_and) {
        clause.logic = LogicOp::OR;
        // Split by OR
        std::vector<std::string> parts;
        size_t pos = 0;
        while (true) {
            size_t or_pos = upper.find(" OR ", pos);
            if (or_pos == std::string::npos) {
                parts.push_back(Trim(where_str.substr(pos)));
                break;
            }
            parts.push_back(Trim(where_str.substr(pos, or_pos - pos)));
            pos = or_pos + 4; // len(" OR ")
        }
        for (auto& p : parts) {
            if (!p.empty()) clause.preds.push_back(ParsePredicate(p));
        }
    } else {
        // Default: AND (also handles mixed AND/OR as AND for simplicity)
        clause.logic = LogicOp::AND;
        std::vector<std::string> parts;
        size_t pos = 0;
        while (true) {
            size_t and_pos = upper.find(" AND ", pos);
            if (and_pos == std::string::npos) {
                parts.push_back(Trim(where_str.substr(pos)));
                break;
            }
            parts.push_back(Trim(where_str.substr(pos, and_pos - pos)));
            pos = and_pos + 5; // len(" AND ")
        }
        for (auto& p : parts) {
            if (!p.empty()) clause.preds.push_back(ParsePredicate(p));
        }
    }

    return clause;
}

// ═══════════════════════════════════════════════════════════
// Top-level Parse dispatcher
// ═══════════════════════════════════════════════════════════

std::unique_ptr<Statement> Parser::Parse(const std::string& sql) {
    std::string trimmed = Trim(sql);
    // Remove trailing semicolon
    if (!trimmed.empty() && trimmed.back() == ';') {
        trimmed.pop_back();
        trimmed = Trim(trimmed);
    }

    std::string upper = ToUpper(trimmed);

    if (upper.substr(0, 6) == "CREATE") {
        return ParseCreate(trimmed);
    } else if (upper.substr(0, 6) == "SELECT") {
        return ParseSelect(trimmed);
    } else if (upper.substr(0, 6) == "INSERT") {
        return ParseInsert(trimmed);
    } else if (upper.substr(0, 6) == "DELETE") {
        return ParseDelete(trimmed);
    }

    throw std::runtime_error("Unsupported SQL: " + sql);
}

// ═══════════════════════════════════════════════════════════
// CREATE TABLE name (col1 TYPE, col2 TYPE, ...)
// ═══════════════════════════════════════════════════════════

std::unique_ptr<CreateTableStmt> Parser::ParseCreate(const std::string& sql) {
    auto stmt = std::make_unique<CreateTableStmt>();
    std::string upper = ToUpper(sql);

    // Expect "CREATE TABLE"
    size_t table_kw = upper.find("TABLE ");
    if (table_kw == std::string::npos) {
        throw std::runtime_error("CREATE missing TABLE keyword: " + sql);
    }

    std::string after_table = Trim(sql.substr(table_kw + 6));

    // Find the opening parenthesis
    size_t paren_open = after_table.find('(');
    if (paren_open == std::string::npos) {
        throw std::runtime_error("CREATE TABLE missing column definitions: " + sql);
    }

    stmt->table_name = Trim(after_table.substr(0, paren_open));

    // Find matching closing paren
    size_t paren_close = after_table.rfind(')');
    if (paren_close == std::string::npos || paren_close <= paren_open) {
        throw std::runtime_error("CREATE TABLE missing closing parenthesis: " + sql);
    }

    std::string cols_str = after_table.substr(paren_open + 1, paren_close - paren_open - 1);
    auto col_defs = Split(cols_str, ',');

    for (auto& col_def : col_defs) {
        // Each column def: "name TYPE" or "name TYPE PRIMARY KEY"
        auto tokens = Split(col_def, ' ');
        if (tokens.size() < 2) {
            throw std::runtime_error("Invalid column definition: " + col_def);
        }

        ColumnDef cd;
        cd.name = tokens[0];
        cd.type = ParseColumnType(tokens[1]);
        stmt->columns.push_back(cd);
    }

    if (stmt->columns.empty()) {
        throw std::runtime_error("CREATE TABLE needs at least one column: " + sql);
    }

    return stmt;
}

// ═══════════════════════════════════════════════════════════
// SELECT [cols] FROM table [JOIN t2 ON ...] [WHERE ...]
// ═══════════════════════════════════════════════════════════

std::unique_ptr<SelectStmt> Parser::ParseSelect(const std::string& sql) {
    auto stmt = std::make_unique<SelectStmt>();
    std::string upper = ToUpper(sql);

    // Extract columns: between SELECT and FROM
    size_t from_pos = upper.find(" FROM ");
    if (from_pos == std::string::npos) {
        throw std::runtime_error("SELECT missing FROM: " + sql);
    }

    std::string cols_str = Trim(sql.substr(6, from_pos - 6)); // after "SELECT"
    if (cols_str == "*") {
        stmt->columns.push_back("*");
    } else {
        stmt->columns = Split(cols_str, ',');
    }

    // After FROM: get table name(s)
    std::string after_from = Trim(sql.substr(from_pos + 6));
    std::string upper_after = ToUpper(after_from);

    // Check for JOIN
    size_t join_pos = upper_after.find(" JOIN ");
    size_t where_pos = upper_after.find(" WHERE ");

    if (join_pos != std::string::npos) {
        stmt->has_join = true;
        stmt->table_name = Trim(after_from.substr(0, join_pos));

        // After JOIN: join_table ON condition
        std::string after_join = Trim(after_from.substr(join_pos + 6));
        std::string upper_join = ToUpper(after_join);
        size_t on_pos = upper_join.find(" ON ");
        if (on_pos == std::string::npos) {
            throw std::runtime_error("JOIN missing ON: " + sql);
        }

        stmt->join_table = Trim(after_join.substr(0, on_pos));

        // Parse ON condition: t1.col = t2.col
        std::string on_str = after_join.substr(on_pos + 4);

        // Check if there's a WHERE after the ON condition
        std::string upper_on = ToUpper(on_str);
        size_t where_in_on = upper_on.find(" WHERE ");
        std::string on_cond_str;
        if (where_in_on != std::string::npos) {
            on_cond_str = Trim(on_str.substr(0, where_in_on));
            std::string where_str = Trim(on_str.substr(where_in_on + 7));
            stmt->where_clause = ParseWhereClause(where_str);
        } else {
            on_cond_str = Trim(on_str);
        }

        // Parse "t1.col = t2.col"
        auto eq_pos2 = on_cond_str.find('=');
        if (eq_pos2 == std::string::npos) {
            throw std::runtime_error("JOIN ON missing =: " + sql);
        }
        std::string left_part  = Trim(on_cond_str.substr(0, eq_pos2));
        std::string right_part = Trim(on_cond_str.substr(eq_pos2 + 1));

        auto dot1 = left_part.find('.');
        auto dot2 = right_part.find('.');

        if (dot1 != std::string::npos) {
            stmt->join_cond.left_table  = left_part.substr(0, dot1);
            stmt->join_cond.left_column = left_part.substr(dot1 + 1);
        } else {
            stmt->join_cond.left_table  = stmt->table_name;
            stmt->join_cond.left_column = left_part;
        }

        if (dot2 != std::string::npos) {
            stmt->join_cond.right_table  = right_part.substr(0, dot2);
            stmt->join_cond.right_column = right_part.substr(dot2 + 1);
        } else {
            stmt->join_cond.right_table  = stmt->join_table;
            stmt->join_cond.right_column = right_part;
        }

    } else if (where_pos != std::string::npos) {
        stmt->table_name = Trim(after_from.substr(0, where_pos));
        std::string where_str = Trim(after_from.substr(where_pos + 7));
        stmt->where_clause = ParseWhereClause(where_str);
    } else {
        stmt->table_name = Trim(after_from);
    }

    return stmt;
}

// ═══════════════════════════════════════════════════════════
// INSERT INTO table VALUES (v1, v2, ...)
// ═══════════════════════════════════════════════════════════

std::unique_ptr<InsertStmt> Parser::ParseInsert(const std::string& sql) {
    auto stmt = std::make_unique<InsertStmt>();
    std::string upper = ToUpper(sql);

    size_t into_pos = upper.find("INTO ");
    if (into_pos == std::string::npos) {
        throw std::runtime_error("INSERT missing INTO: " + sql);
    }

    std::string after_into = Trim(sql.substr(into_pos + 5));
    size_t values_pos = ToUpper(after_into).find("VALUES");
    if (values_pos == std::string::npos) {
        throw std::runtime_error("INSERT missing VALUES: " + sql);
    }

    stmt->table_name = Trim(after_into.substr(0, values_pos));

    // Get the values between parentheses
    std::string vals_str = after_into.substr(values_pos + 6);
    size_t open  = vals_str.find('(');
    size_t close = vals_str.rfind(')');
    if (open == std::string::npos || close == std::string::npos) {
        throw std::runtime_error("INSERT VALUES missing parentheses: " + sql);
    }

    std::string inside = vals_str.substr(open + 1, close - open - 1);
    auto parts = Split(inside, ',');
    for (auto& p : parts) {
        stmt->values.push_back(ParseValue(p));
    }

    return stmt;
}

// ═══════════════════════════════════════════════════════════
// DELETE FROM table [WHERE ...]
// ═══════════════════════════════════════════════════════════

std::unique_ptr<DeleteStmt> Parser::ParseDelete(const std::string& sql) {
    auto stmt = std::make_unique<DeleteStmt>();
    std::string upper = ToUpper(sql);

    size_t from_pos = upper.find(" FROM ");
    if (from_pos == std::string::npos) {
        throw std::runtime_error("DELETE missing FROM: " + sql);
    }

    std::string after_from = Trim(sql.substr(from_pos + 6));
    std::string upper_after = ToUpper(after_from);
    size_t where_pos = upper_after.find(" WHERE ");

    if (where_pos != std::string::npos) {
        stmt->table_name = Trim(after_from.substr(0, where_pos));
        std::string where_str = Trim(after_from.substr(where_pos + 7));
        stmt->where_clause = ParseWhereClause(where_str);
    } else {
        stmt->table_name = Trim(after_from);
    }

    return stmt;
}

} // namespace minidb
