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
    bool        has_default = false;   // was a DEFAULT (...) clause given?
    Value       default_value;         // used when a column is omitted (or given
                                        // as DEFAULT) on INSERT, or SET col = DEFAULT
                                        // on UPDATE. Only meaningful when has_default.
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

// ON DELETE behavior for a FOREIGN KEY constraint — see the parser's
// FkOnDelete (ast.h) for the full explanation; this is the catalog's flat,
// serializable copy of the same two values, kept as its own enum (not a
// #include of ast.h) for the same reason CheckConstraint doesn't reuse the
// parser's CompareOp: this layer must not depend on the parser at all.
enum class FkOnDelete : uint8_t {
    RESTRICT = 1,
    CASCADE  = 2,
};

// A single FOREIGN KEY constraint: this table's column(s) at column_indices
// must, for every non-NULL row, match some row in ref_table's
// ref_column_indices. Like CheckConstraint, this is a flat, POD-like,
// serializable representation — NOT a parser AST node — for the same
// round-trip-to-disk reason.
//
// ref_column_indices must name either ref_table's primary key or an
// existing UNIQUE constraint on ref_table (validated once, at CREATE TABLE
// time, in Executor::execute_create_table) — this is what makes the
// parent-side existence check a single indexed lookup instead of a scan.
//
// Enforcement needs an efficient way to go the other direction too — given
// a parent key, find every child row referencing it (for a parent-side
// DELETE/UPDATE) — so Executor::execute_create_table also auto-creates a
// plain (non-unique) secondary Index over column_indices on THIS table,
// the same way a UNIQUE constraint auto-creates a unique one. Its name is
// stored here as child_index_name so enforcement code never has to
// reconstruct or guess it.
struct ForeignKeyConstraint {
    std::vector<uint32_t> column_indices;      // this table's FK columns
    std::string           ref_table;           // parent table name
    std::vector<uint32_t> ref_column_indices;  // parent's referenced columns, same order/arity
    FkOnDelete             on_delete = FkOnDelete::RESTRICT;
    std::string            child_index_name;   // auto-created non-unique index on column_indices

    // Which parent-side index a value lookup uses, resolved once at CREATE
    // TABLE time instead of re-searched on every INSERT/UPDATE:
    //   ""    — ref_column_indices IS ref_table's primary key; look up via
    //           Table::select_by_key() on ref_table (no secondary Index —
    //           the primary B+ tree itself is the "index").
    //   other — the name of the existing UNIQUE IndexSchema on ref_table
    //           whose column_names (in that exact declared order) equal
    //           ref_column_indices; look up via Index::find().
    std::string            ref_index_name;
};

// describes a complete table — its name, columns, and where its data lives
struct TableSchema {
    std::string              name;           // table name
    std::vector<Column>      columns;        // column definitions, in order
    uint32_t                 root_page;      // page_id of the B+ tree root for this table
    // indices into columns[] that make up the primary key, in declared order.
    // size() == 1 for the common single-column case (INT or otherwise);
    // size() > 1 for a table-level composite PRIMARY KEY (a, b, ...).
    // The BTree Key built from a row is the tuple of values at these
    // indices, in this order — see Table::extract_primary_key.
    std::vector<uint32_t>    primary_key_indices;
    std::vector<CheckConstraint> checks;     // CHECK constraints, all implicitly ANDed together
    std::vector<ForeignKeyConstraint> foreign_keys;  // FOREIGN KEY constraints on this table

    // returns the column index for a given column name, or -1 if not found
    int column_index(const std::string& col_name) const {
        for (size_t i = 0; i < columns.size(); i++) {
            if (columns[i].name == col_name) return static_cast<int>(i);
        }
        return -1;
    }

    bool is_composite_primary_key() const {
        return primary_key_indices.size() > 1;
    }

    // returns the sole primary key column. Only meaningful when the
    // primary key is a single column — callers must not call this for a
    // composite key (is_composite_primary_key() == true).
    const Column& primary_key_column() const {
        return columns[primary_key_indices.at(0)];
    }
};

// describes a secondary index on a table
struct IndexSchema {
    std::string name;                      // index name
    std::string table_name;                // which table this index belongs to
    // names of the indexed column(s), in declared order. size() == 1 for
    // the common single-column case; size() > 1 for a composite index
    // (e.g. CREATE INDEX ... ON t (a, b)) — the same "arity via vector"
    // pattern TableSchema::primary_key_indices already uses for composite
    // primary keys. The Index class builds its raw BTree key by looking
    // up each of these columns' values, in this order, then appending the
    // table's full primary key after them.
    std::vector<std::string> column_names;
    uint32_t    root_page;      // page_id of the B+ tree root for this index
    bool        is_unique;      // does this index enforce uniqueness?

    bool is_composite() const {
        return column_names.size() > 1;
    }
};