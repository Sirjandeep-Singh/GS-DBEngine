#pragma once

#include <string>
#include <vector>
#include <variant>
#include <cstdint>
#include <stdexcept>

// A Value holds the data for one column in a row.
// std::monostate represents SQL NULL — no value present.
// The active type must match the column's ColumnType from the schema.
using Value = std::variant<
    std::monostate,  // NULL
    int32_t,         // INT
    float,           // FLOAT
    bool,            // BOOLEAN
    std::string      // VARCHAR
>;

// helper to check if a Value is NULL
inline bool is_null(const Value& v) {
    return std::holds_alternative<std::monostate>(v);
}

// helper accessors — throw if the wrong type is accessed
inline int32_t     get_int(const Value& v)     { return std::get<int32_t>(v); }
inline float       get_float(const Value& v)   { return std::get<float>(v); }
inline bool        get_bool(const Value& v)    { return std::get<bool>(v); }
inline std::string get_string(const Value& v)  { return std::get<std::string>(v); }

// converts a Value to a human-readable string for display
inline std::string value_to_string(const Value& v) {
    if (is_null(v))                                  return "NULL";
    if (std::holds_alternative<int32_t>(v))          return std::to_string(get_int(v));
    if (std::holds_alternative<float>(v))            return std::to_string(get_float(v));
    if (std::holds_alternative<bool>(v))             return get_bool(v) ? "true" : "false";
    if (std::holds_alternative<std::string>(v))      return get_string(v);
    return "UNKNOWN";
}

// A Row is an ordered list of Values, one per column.
// The i-th Value corresponds to the i-th column in the TableSchema.
// Row does not carry schema information — it is always interpreted
// alongside a TableSchema by the layers that need type information.
struct Row {
    std::vector<Value> values;

    // returns the value at a given column index
    const Value& get(size_t col_idx) const {
        if (col_idx >= values.size()) {
            throw std::out_of_range("Row::get: column index out of range");
        }
        return values[col_idx];
    }

    Value& get(size_t col_idx) {
        if (col_idx >= values.size()) {
            throw std::out_of_range("Row::get: column index out of range");
        }
        return values[col_idx];
    }

    size_t size() const { return values.size(); }
};