#include "zetta/query/predicate.hpp"

#include <string>

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

bool row_matches(const nlohmann::json& row,
                 const std::optional<WhereClause>& where) {
    if (!where) return true;
    if (!row.contains(where->column)) return false;
    return compare_cell(row.at(where->column), where->op, where->value);
}

}  // namespace zetta::query
