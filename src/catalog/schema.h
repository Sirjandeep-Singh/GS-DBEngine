#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "../row/row.h"  // for Value — a dependency-free leaf header, safe at this layer

// supported column types
enum class ColumnType : uint8_t {
    INT     = 1,  // 4 bytes, signed
    FLOAT   = 2,  // 4 bytes, IEEE 754
    BOOLEAN = 3,  // 1 byte, 0 or 1
    VARCHAR = 4,  // variable length, up to max_length bytes
};

// returns the fixed byte size of a type
// VARCHAR returns 0 — it has no fixed size
inline uint32_t column_type_size(ColumnType type) {
    switch (type) {
        case ColumnType::INT:     return 4;
        case ColumnType::FLOAT:   return 4;
        case ColumnType::BOOLEAN: return 1;
        case ColumnType::VARCHAR: return 0;
    }
    return 0;
}

inline std::string column_type_name(ColumnType type) {
    switch (type) {
        case ColumnType::INT:     return "INT";
        case ColumnType::FLOAT:   return "FLOAT";
        case ColumnType::BOOLEAN: return "BOOLEAN";
        case ColumnType::VARCHAR: return "VARCHAR";
    }
    return "UNKNOWN";
}

// describes a single column in a table
struct Column {
    std::string name;
    ColumnType  type;
    uint32_t    max_length;     // only meaningful for VARCHAR, ignored otherwise
    bool        is_nullable;    // can this column hold NULL?
    bool        is_primary_key; // is this the primary key column?
    bool        auto_increment; // only valid for INT primary keys
};

// comparison operators supported in a CHECK constraint.
// deliberately a small, flat set — SQL has no chained comparisons
// (`30 < x < 50` is not valid SQL), so every CHECK constraint is
// represented as one or more simple `column OP literal` comparisons,
// implicitly ANDed together. `CHECK (x > 30 AND x < 50)` becomes two
// CheckConstraint entries: {x, GT, 30} and {x, LT, 50}.
enum class CheckOp : uint8_t {
    EQ  = 1,
    NEQ = 2,
    LT  = 3,
    GT  = 4,
    LTE = 5,
    GTE = 6,
};

inline std::string check_op_symbol(CheckOp op) {
    switch (op) {
        case CheckOp::EQ:  return "=";
        case CheckOp::NEQ: return "!=";
        case CheckOp::LT:  return "<";
        case CheckOp::GT:  return ">";
        case CheckOp::LTE: return "<=";
        case CheckOp::GTE: return ">=";
    }
    return "?";
}

// A single CHECK constraint: schema.columns[column_index] OP operand.
// This is deliberately a flat, POD-like, serializable representation —
// NOT a parser AST node. It has to survive CatalogManager's binary
// serialize()/deserialize() round-trip to disk and back, and TableSchema
// must stay copyable, so nothing here may own a recursive expression
// tree (no unique_ptr, no std::function).
struct CheckConstraint {
    uint32_t column_index;
    CheckOp  op;
    Value    operand;
};

// describes a complete table — its name, columns, and where its data lives
struct TableSchema {
    std::string              name;           // table name
    std::vector<Column>      columns;        // column definitions, in order
    uint32_t                 root_page;      // page_id of the B+ tree root for this table
    uint32_t                 primary_key_index; // index into columns[] of the primary key column
    std::vector<CheckConstraint> checks;     // CHECK constraints, all implicitly ANDed together

    // returns the column index for a given column name, or -1 if not found
    int column_index(const std::string& col_name) const {
        for (size_t i = 0; i < columns.size(); i++) {
            if (columns[i].name == col_name) return static_cast<int>(i);
        }
        return -1;
    }

    const Column& primary_key_column() const {
        return columns[primary_key_index];
    }
};

// describes a secondary index on a table
struct IndexSchema {
    std::string name;           // index name
    std::string table_name;     // which table this index belongs to
    std::string column_name;    // which column is indexed
    uint32_t    root_page;      // page_id of the B+ tree root for this index
    bool        is_unique;      // does this index enforce uniqueness?
};