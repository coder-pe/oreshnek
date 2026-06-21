// oreshnek/include/oreshnek/platform/SqlResult.h
//
// Domain-agnostic result of running a SQL statement. The framework's database
// layer is a generic gateway: it knows nothing about users, videos or any other
// model. Applications run parameterized statements and map the generic rows
// below into their own structs.
#ifndef ORESHNEK_PLATFORM_SQL_RESULT_H
#define ORESHNEK_PLATFORM_SQL_RESULT_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Oreshnek {
namespace Platform {

// A bound query parameter. std::nullopt binds SQL NULL; otherwise the value is
// passed as text. Both drivers (SQLite, libpq) coerce the text to the column's
// type, which keeps the generic API free of per-type binding code while still
// using true parameterized queries (never string concatenation) — so callers
// get SQL-injection safety for free.
using SqlParam = std::optional<std::string>;
using SqlParams = std::vector<SqlParam>;

// The outcome of running a statement.
//
// For a SELECT, `columns` and `rows` are filled. For INSERT/UPDATE/DELETE/DDL,
// `affected` carries the row count and `last_insert_id` the new rowid where the
// backend provides one (SQLite always; PostgreSQL only via `RETURNING`). `ok` is
// false on a driver error, with the message in `error`.
//
// Cell values are stored as text exactly as the driver returns them; a missing
// optional means SQL NULL. The typed accessors parse on demand so application
// code never touches driver-specific column types or null handling.
class SqlResult {
public:
    bool ok = false;
    std::string error;
    std::int64_t affected = 0;
    std::int64_t last_insert_id = 0;
    std::vector<std::string> columns;
    std::vector<std::vector<std::optional<std::string>>> rows;

    std::size_t row_count() const { return rows.size(); }
    bool empty() const { return rows.empty(); }

    // Index of a column by name, or -1 if absent.
    int column(std::string_view name) const {
        for (std::size_t i = 0; i < columns.size(); ++i) {
            if (columns[i] == name) return static_cast<int>(i);
        }
        return -1;
    }

    bool is_null(std::size_t row, std::size_t col) const {
        return row >= rows.size() || col >= rows[row].size() || !rows[row][col].has_value();
    }

    // The raw text of a cell, or "" if NULL / out of range.
    std::string_view text(std::size_t row, std::size_t col) const {
        if (is_null(row, col)) return {};
        return rows[row][col].value();
    }

    std::int64_t integer(std::size_t row, std::size_t col, std::int64_t def = 0) const {
        if (is_null(row, col)) return def;
        try { return std::stoll(rows[row][col].value()); } catch (...) { return def; }
    }

    double real(std::size_t row, std::size_t col, double def = 0.0) const {
        if (is_null(row, col)) return def;
        try { return std::stod(rows[row][col].value()); } catch (...) { return def; }
    }

    // SQLite stores booleans as 0/1; PostgreSQL renders them as t/f in text.
    bool boolean(std::size_t row, std::size_t col) const {
        if (is_null(row, col)) return false;
        const std::string& v = rows[row][col].value();
        return v == "1" || v == "t" || v == "true" || v == "TRUE";
    }
};

}  // namespace Platform
}  // namespace Oreshnek

#endif  // ORESHNEK_PLATFORM_SQL_RESULT_H
