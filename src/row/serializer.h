#pragma once

#include <vector>
#include <cstdint>
#include "row.h"
#include "../catalog/schema.h"

// RowSerializer converts between Row objects and raw bytes.
//
// These bytes are what gets stored as the "value blob" inside a B+ tree
// leaf cell — the BTree layer treats them as opaque, only RowSerializer
// knows their internal structure.
//
// Byte format:
//   [null_bitmap: ceil(num_columns / 8) bytes]
//      one bit per column, bit=1 means that column is NULL
//      columns are ordered LSB first within each byte
//   per non-null column in schema order:
//      INT:     4 bytes, little-endian signed
//      FLOAT:   4 bytes, IEEE 754 little-endian
//      BOOLEAN: 1 byte, 0x00 = false, 0x01 = true
//      VARCHAR: 2 bytes length (uint16_t) + length bytes of UTF-8 data
//
// The schema is required for both directions:
//   serialize   — to know how many bytes each column takes
//   deserialize — to know how to interpret each byte sequence

class RowSerializer {
public:
    // converts a Row into a byte blob for storage in a B+ tree leaf cell.
    // the Row must have exactly schema.columns.size() values.
    // throws if value count does not match schema column count.
    // throws if a non-nullable column has a NULL value.
    // throws if a Value's type does not match its column's ColumnType.
    static std::vector<uint8_t> serialize(const Row& row, const TableSchema& schema);

    // converts a byte blob back into a Row.
    // the bytes must have been produced by serialize() with the same schema.
    // throws if the byte blob is malformed or too short.
    static Row deserialize(const std::vector<uint8_t>& bytes, const TableSchema& schema);

private:
    // null bitmap helpers
    static size_t bitmap_size(size_t num_columns);
    static void   set_null_bit(std::vector<uint8_t>& bitmap, size_t col_idx);
    static bool   get_null_bit(const std::vector<uint8_t>& bitmap, size_t col_idx);

    // per-type encode helpers — append bytes to buf
    static void encode_int    (std::vector<uint8_t>& buf, int32_t val);
    static void encode_float  (std::vector<uint8_t>& buf, float val);
    static void encode_bool   (std::vector<uint8_t>& buf, bool val);
    static void encode_varchar(std::vector<uint8_t>& buf, const std::string& val);

    // per-type decode helpers — read from bytes at pos, advance pos
    static int32_t     decode_int    (const std::vector<uint8_t>& buf, size_t& pos);
    static float       decode_float  (const std::vector<uint8_t>& buf, size_t& pos);
    static bool        decode_bool   (const std::vector<uint8_t>& buf, size_t& pos);
    static std::string decode_varchar(const std::vector<uint8_t>& buf, size_t& pos);
};