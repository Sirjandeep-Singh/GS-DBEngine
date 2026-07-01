#include "serializer.h"

#include <stdexcept>
#include <cstring>

// ─────────────────────────────────────────────
// Null bitmap helpers
// ─────────────────────────────────────────────

size_t RowSerializer::bitmap_size(size_t num_columns) {
    return (num_columns + 7) / 8;  // ceil(num_columns / 8)
}

void RowSerializer::set_null_bit(std::vector<uint8_t>& bitmap, size_t col_idx) {
    bitmap[col_idx / 8] |= (1 << (col_idx % 8));
}

bool RowSerializer::get_null_bit(const std::vector<uint8_t>& bitmap, size_t col_idx) {
    return (bitmap[col_idx / 8] >> (col_idx % 8)) & 1;
}

// ─────────────────────────────────────────────
// Encode helpers
// ─────────────────────────────────────────────

void RowSerializer::encode_int(std::vector<uint8_t>& buf, int32_t val) {
    uint8_t bytes[4];
    std::memcpy(bytes, &val, 4);
    buf.push_back(bytes[0]);
    buf.push_back(bytes[1]);
    buf.push_back(bytes[2]);
    buf.push_back(bytes[3]);
}

void RowSerializer::encode_float(std::vector<uint8_t>& buf, float val) {
    uint8_t bytes[4];
    std::memcpy(bytes, &val, 4);
    buf.push_back(bytes[0]);
    buf.push_back(bytes[1]);
    buf.push_back(bytes[2]);
    buf.push_back(bytes[3]);
}

void RowSerializer::encode_bool(std::vector<uint8_t>& buf, bool val) {
    buf.push_back(val ? 0x01 : 0x00);
}

void RowSerializer::encode_varchar(std::vector<uint8_t>& buf, const std::string& val) {
    uint16_t len = static_cast<uint16_t>(val.size());
    uint8_t len_bytes[2];
    std::memcpy(len_bytes, &len, 2);
    buf.push_back(len_bytes[0]);
    buf.push_back(len_bytes[1]);
    for (char c : val) {
        buf.push_back(static_cast<uint8_t>(c));
    }
}

// ─────────────────────────────────────────────
// Decode helpers
// ─────────────────────────────────────────────

int32_t RowSerializer::decode_int(const std::vector<uint8_t>& buf, size_t& pos) {
    if (pos + 4 > buf.size()) {
        throw std::runtime_error("RowSerializer::decode_int: buffer too short");
    }
    int32_t val;
    std::memcpy(&val, &buf[pos], 4);
    pos += 4;
    return val;
}

float RowSerializer::decode_float(const std::vector<uint8_t>& buf, size_t& pos) {
    if (pos + 4 > buf.size()) {
        throw std::runtime_error("RowSerializer::decode_float: buffer too short");
    }
    float val;
    std::memcpy(&val, &buf[pos], 4);
    pos += 4;
    return val;
}

bool RowSerializer::decode_bool(const std::vector<uint8_t>& buf, size_t& pos) {
    if (pos + 1 > buf.size()) {
        throw std::runtime_error("RowSerializer::decode_bool: buffer too short");
    }
    return buf[pos++] != 0x00;
}

std::string RowSerializer::decode_varchar(const std::vector<uint8_t>& buf, size_t& pos) {
    if (pos + 2 > buf.size()) {
        throw std::runtime_error("RowSerializer::decode_varchar: buffer too short for length");
    }
    uint16_t len;
    std::memcpy(&len, &buf[pos], 2);
    pos += 2;

    if (pos + len > buf.size()) {
        throw std::runtime_error("RowSerializer::decode_varchar: buffer too short for data");
    }
    std::string val(buf.begin() + pos, buf.begin() + pos + len);
    pos += len;
    return val;
}

// ─────────────────────────────────────────────
// Serialize
// ─────────────────────────────────────────────

std::vector<uint8_t> RowSerializer::serialize(const Row& row, const TableSchema& schema) {
    size_t num_cols = schema.columns.size();

    if (row.size() != num_cols) {
        throw std::runtime_error(
            "RowSerializer::serialize: row has " + std::to_string(row.size()) +
            " values but schema has " + std::to_string(num_cols) + " columns");
    }

    // build null bitmap first — all zeros (no nulls) to start
    std::vector<uint8_t> bitmap(bitmap_size(num_cols), 0x00);

    for (size_t i = 0; i < num_cols; i++) {
        const Value& val = row.get(i);
        const Column& col = schema.columns[i];

        if (is_null(val)) {
            if (!col.is_nullable) {
                throw std::runtime_error(
                    "RowSerializer::serialize: NULL value in non-nullable column: " + col.name);
            }
            set_null_bit(bitmap, i);
        }
    }

    std::vector<uint8_t> buf;

    // write null bitmap
    buf.insert(buf.end(), bitmap.begin(), bitmap.end());

    // write each non-null column value in schema order
    for (size_t i = 0; i < num_cols; i++) {
        if (get_null_bit(bitmap, i)) continue;  // skip NULLs

        const Value& val = row.get(i);
        const Column& col = schema.columns[i];

        switch (col.type) {
            case ColumnType::INT: {
                if (!std::holds_alternative<int32_t>(val)) {
                    throw std::runtime_error(
                        "RowSerializer::serialize: expected INT for column: " + col.name);
                }
                encode_int(buf, get_int(val));
                break;
            }
            case ColumnType::FLOAT: {
                if (!std::holds_alternative<float>(val)) {
                    throw std::runtime_error(
                        "RowSerializer::serialize: expected FLOAT for column: " + col.name);
                }
                encode_float(buf, get_float(val));
                break;
            }
            case ColumnType::BOOLEAN: {
                if (!std::holds_alternative<bool>(val)) {
                    throw std::runtime_error(
                        "RowSerializer::serialize: expected BOOLEAN for column: " + col.name);
                }
                encode_bool(buf, get_bool(val));
                break;
            }
            case ColumnType::VARCHAR: {
                if (!std::holds_alternative<std::string>(val)) {
                    throw std::runtime_error(
                        "RowSerializer::serialize: expected VARCHAR for column: " + col.name);
                }
                const std::string& str = get_string(val);
                if (str.size() > col.max_length) {
                    throw std::runtime_error(
                        "RowSerializer::serialize: VARCHAR value exceeds max_length for column: "
                        + col.name);
                }
                encode_varchar(buf, str);
                break;
            }
        }
    }

    return buf;
}

// ─────────────────────────────────────────────
// Deserialize
// ─────────────────────────────────────────────

Row RowSerializer::deserialize(const std::vector<uint8_t>& bytes, const TableSchema& schema) {
    size_t num_cols = schema.columns.size();
    size_t bm_size  = bitmap_size(num_cols);

    if (bytes.size() < bm_size) {
        throw std::runtime_error("RowSerializer::deserialize: buffer too short for null bitmap");
    }

    // extract null bitmap
    std::vector<uint8_t> bitmap(bytes.begin(), bytes.begin() + bm_size);

    Row row;
    row.values.resize(num_cols);

    size_t pos = bm_size;

    for (size_t i = 0; i < num_cols; i++) {
        if (get_null_bit(bitmap, i)) {
            row.values[i] = std::monostate{};  // NULL
            continue;
        }

        const Column& col = schema.columns[i];

        switch (col.type) {
            case ColumnType::INT:
                row.values[i] = decode_int(bytes, pos);
                break;
            case ColumnType::FLOAT:
                row.values[i] = decode_float(bytes, pos);
                break;
            case ColumnType::BOOLEAN:
                row.values[i] = decode_bool(bytes, pos);
                break;
            case ColumnType::VARCHAR:
                row.values[i] = decode_varchar(bytes, pos);
                break;
        }
    }

    return row;
}