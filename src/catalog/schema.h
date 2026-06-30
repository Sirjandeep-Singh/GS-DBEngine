#pragma once

#include <string>
#include <vector>
#include <cstdint>

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

// describes a complete table — its name, columns, and where its data lives
struct TableSchema {
    std::string         name;           // table name
    std::vector<Column> columns;        // column definitions, in order
    uint32_t            root_page;      // page_id of the B+ tree root for this table
    uint32_t            primary_key_index; // index into columns[] of the primary key column

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