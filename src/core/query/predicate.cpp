// SPDX-License-Identifier: MIT

#include "zetta/query/predicate.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace zetta::query {

bool compare_cell(const nlohmann::json& cell, CompareOp op,
                  const nlohmann::json& target) {
    switch (op) {
        case CompareOp::Equal:
            return cell == target;
        case CompareOp::NotEqual:
            return cell != target;
        default:
            break;
    }
    if (cell.is_number() && target.is_number()) {
        const double left = cell.get<double>();
        const double right = target.get<double>();
        switch (op) {
            case CompareOp::Less:         return left < right;
            case CompareOp::LessEqual:    return left <= right;
            case CompareOp::Greater:      return left > right;
            case CompareOp::GreaterEqual: return left >= right;
            default:                      return false;
        }
    }
    if (cell.is_string() && target.is_string()) {
        const std::string& left = cell.get_ref<const std::string&>();
        const std::string& right = target.get_ref<const std::string&>();
        switch (op) {
            case CompareOp::Less:         return left < right;
            case CompareOp::LessEqual:    return left <= right;
            case CompareOp::Greater:      return left > right;
            case CompareOp::GreaterEqual: return left >= right;
            default:                      return false;
        }
    }
    return false;
}

namespace {

// SQL's three-valued truth: TRUE, FALSE, or UNKNOWN (nullopt).
using Truth = std::optional<bool>;

Truth evaluate(const nlohmann::json& row, const WhereClause& where) {
    switch (where.kind) {
        case WhereClause::Kind::Compare: {
            const auto cell = row.find(where.column);
            if (cell == row.end()) return std::nullopt;  // like a NULL comparison
            return compare_cell(*cell, where.op, where.value);
        }
        case WhereClause::Kind::And: {
            Truth result = true;
            for (const WhereClause& operand : where.operands) {
                const Truth side = evaluate(row, operand);
                if (side == false) return false;  // FALSE wins over UNKNOWN
                if (!side) result = std::nullopt;
            }
            return result;
        }
        case WhereClause::Kind::Or: {
            Truth result = false;
            for (const WhereClause& operand : where.operands) {
                const Truth side = evaluate(row, operand);
                if (side == true) return true;  // TRUE wins over UNKNOWN
                if (!side) result = std::nullopt;
            }
            return result;
        }
        case WhereClause::Kind::Not: {
            if (where.operands.size() != 1) return false;  // malformed: no match
            const Truth side = evaluate(row, where.operands.front());
            if (!side) return std::nullopt;
            return !*side;
        }
    }
    return false;
}

void collect_columns(const WhereClause& where, std::vector<std::string>& out) {
    if (where.kind == WhereClause::Kind::Compare) {
        if (std::ranges::find(out, where.column) == out.end()) {
            out.push_back(where.column);
        }
        return;
    }
    for (const WhereClause& operand : where.operands) collect_columns(operand, out);
}

}  // namespace

bool row_matches(const nlohmann::json& row,
                 const std::optional<WhereClause>& where) {
    if (!where) return true;
    if (!row.is_object()) return false;
    return evaluate(row, *where) == true;
}

std::vector<std::string> where_columns(const WhereClause& where) {
    std::vector<std::string> columns;
    collect_columns(where, columns);
    return columns;
}

}  // namespace zetta::query
