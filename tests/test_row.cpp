//g++ -std=c++17 tests/test_table.cpp src/table/table.cpp src/row/serializer.cpp src/btree/btree.cpp src/btree/btree_node.cpp src/catalog/catalog_manager.cpp src/storage/disk_manager.cpp src/storage/header_manager.cpp src/storage/buffer_pool.cpp src/wal/wal_manager.cpp -o tests/test_table && ./tests/test_table
#include <iostream>
#include <cassert>
#include <cmath>

#include "../src/row/row.h"
#include "../src/row/serializer.h"
#include "../src/catalog/schema.h"

// helper — builds a users schema: id INT PK, name VARCHAR(50), age INT nullable, active BOOLEAN
TableSchema make_users_schema() {
    TableSchema schema;
    schema.name              = "users";
    schema.root_page         = 0;
    schema.primary_key_indices = {0};

    Column id;
    id.name           = "id";
    id.type           = ColumnType::INT;
    id.max_length     = 0;
    id.is_nullable    = false;
    id.is_primary_key = true;
    id.auto_increment = true;

    Column name;
    name.name           = "name";
    name.type           = ColumnType::VARCHAR;
    name.max_length     = 50;
    name.is_nullable    = false;
    name.is_primary_key = false;
    name.auto_increment = false;

    Column age;
    age.name           = "age";
    age.type           = ColumnType::INT;
    age.max_length     = 0;
    age.is_nullable    = true;
    age.is_primary_key = false;
    age.auto_increment = false;

    Column active;
    active.name           = "active";
    active.type           = ColumnType::BOOLEAN;
    active.max_length     = 0;
    active.is_nullable    = false;
    active.is_primary_key = false;
    active.auto_increment = false;

    schema.columns = {id, name, age, active};
    return schema;
}

// helper — builds a products schema: id INT PK, price FLOAT, description VARCHAR(100) nullable
TableSchema make_products_schema() {
    TableSchema schema;
    schema.name              = "products";
    schema.root_page         = 0;
    schema.primary_key_indices = {0};

    Column id;
    id.name           = "id";
    id.type           = ColumnType::INT;
    id.max_length     = 0;
    id.is_nullable    = false;
    id.is_primary_key = true;
    id.auto_increment = true;

    Column price;
    price.name           = "price";
    price.type           = ColumnType::FLOAT;
    price.max_length     = 0;
    price.is_nullable    = false;
    price.is_primary_key = false;
    price.auto_increment = false;

    Column description;
    description.name           = "description";
    description.type           = ColumnType::VARCHAR;
    description.max_length     = 100;
    description.is_nullable    = true;
    description.is_primary_key = false;
    description.auto_increment = false;

    schema.columns = {id, price, description};
    return schema;
}

// ─────────────────────────────────────────────
// Row Tests
// ─────────────────────────────────────────────

void test_row_get_value() {
    Row row;
    row.values = {int32_t(1), std::string("Alice"), int32_t(25), bool(true)};

    assert(get_int(row.get(0))    == 1);
    assert(get_string(row.get(1)) == "Alice");
    assert(get_int(row.get(2))    == 25);
    assert(get_bool(row.get(3))   == true);

    std::cout << "[PASS] row.get() returns correct values at each index\n";
}

void test_row_get_out_of_range_throws() {
    Row row;
    row.values = {int32_t(1)};

    bool threw = false;
    try {
        row.get(5);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    assert(threw);

    std::cout << "[PASS] row.get() throws on out of range index\n";
}

void test_row_size() {
    Row row;
    row.values = {int32_t(1), std::string("Alice"), std::monostate{}};

    assert(row.size() == 3);

    std::cout << "[PASS] row.size() returns correct column count\n";
}

void test_is_null() {
    Value null_val  = std::monostate{};
    Value int_val   = int32_t(42);

    assert(is_null(null_val)  == true);
    assert(is_null(int_val)   == false);

    std::cout << "[PASS] is_null() correctly identifies NULL and non-NULL values\n";
}

void test_value_to_string() {
    assert(value_to_string(std::monostate{})     == "NULL");
    assert(value_to_string(int32_t(42))          == "42");
    assert(value_to_string(bool(true))           == "true");
    assert(value_to_string(bool(false))          == "false");
    assert(value_to_string(std::string("hello")) == "hello");

    std::cout << "[PASS] value_to_string() returns correct string for each type\n";
}

// ─────────────────────────────────────────────
// Serializer Tests
// ─────────────────────────────────────────────

void test_serialize_deserialize_full_row() {
    TableSchema schema = make_users_schema();

    Row row;
    row.values = {int32_t(1), std::string("Alice"), int32_t(25), bool(true)};

    auto bytes = RowSerializer::serialize(row, schema);
    Row result = RowSerializer::deserialize(bytes, schema);

    assert(result.size()              == 4);
    assert(get_int(result.get(0))     == 1);
    assert(get_string(result.get(1))  == "Alice");
    assert(get_int(result.get(2))     == 25);
    assert(get_bool(result.get(3))    == true);

    std::cout << "[PASS] serialize/deserialize round trips a full row correctly\n";
}

void test_serialize_deserialize_with_null() {
    TableSchema schema = make_users_schema();

    Row row;
    row.values = {int32_t(2), std::string("Bob"), std::monostate{}, bool(false)};

    auto bytes = RowSerializer::serialize(row, schema);
    Row result = RowSerializer::deserialize(bytes, schema);

    assert(result.size()              == 4);
    assert(get_int(result.get(0))     == 2);
    assert(get_string(result.get(1))  == "Bob");
    assert(is_null(result.get(2))     == true);
    assert(get_bool(result.get(3))    == false);

    std::cout << "[PASS] serialize/deserialize handles NULL column correctly\n";
}

void test_serialize_deserialize_float() {
    TableSchema schema = make_products_schema();

    Row row;
    row.values = {int32_t(1), float(9.99f), std::string("A great product")};

    auto bytes = RowSerializer::serialize(row, schema);
    Row result = RowSerializer::deserialize(bytes, schema);

    assert(get_int(result.get(0))    == 1);
    assert(std::fabs(get_float(result.get(1)) - 9.99f) < 0.0001f);
    assert(get_string(result.get(2)) == "A great product");

    std::cout << "[PASS] serialize/deserialize handles FLOAT correctly\n";
}

void test_serialize_deserialize_nullable_varchar_null() {
    TableSchema schema = make_products_schema();

    Row row;
    row.values = {int32_t(2), float(4.99f), std::monostate{}};

    auto bytes = RowSerializer::serialize(row, schema);
    Row result = RowSerializer::deserialize(bytes, schema);

    assert(get_int(result.get(0))            == 2);
    assert(std::fabs(get_float(result.get(1)) - 4.99f) < 0.0001f);
    assert(is_null(result.get(2))            == true);

    std::cout << "[PASS] serialize/deserialize handles nullable VARCHAR as NULL\n";
}

void test_serialize_deserialize_empty_varchar() {
    TableSchema schema = make_users_schema();

    Row row;
    row.values = {int32_t(3), std::string(""), int32_t(18), bool(true)};

    auto bytes = RowSerializer::serialize(row, schema);
    Row result = RowSerializer::deserialize(bytes, schema);

    assert(get_string(result.get(1)) == "");

    std::cout << "[PASS] serialize/deserialize handles empty VARCHAR string\n";
}

void test_serialize_deserialize_all_nulls() {
    TableSchema schema = make_users_schema();

    // only id and active are non-nullable — set those, null the rest
    Row row;
    row.values = {int32_t(4), std::string("null test"), std::monostate{}, bool(false)};

    auto bytes = RowSerializer::serialize(row, schema);
    Row result = RowSerializer::deserialize(bytes, schema);

    assert(get_int(result.get(0))    == 4);
    assert(get_string(result.get(1)) == "null test");
    assert(is_null(result.get(2))    == true);
    assert(get_bool(result.get(3))   == false);

    std::cout << "[PASS] serialize/deserialize handles maximum NULL columns correctly\n";
}

void test_serialize_null_in_non_nullable_column_throws() {
    TableSchema schema = make_users_schema();

    Row row;
    // id is non-nullable — setting it to NULL should throw
    row.values = {std::monostate{}, std::string("Alice"), int32_t(25), bool(true)};

    bool threw = false;
    try {
        RowSerializer::serialize(row, schema);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);

    std::cout << "[PASS] serialize throws when NULL given for non-nullable column\n";
}

void test_serialize_wrong_column_count_throws() {
    TableSchema schema = make_users_schema();

    Row row;
    row.values = {int32_t(1), std::string("Alice")};  // only 2 values, schema has 4

    bool threw = false;
    try {
        RowSerializer::serialize(row, schema);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);

    std::cout << "[PASS] serialize throws when row column count does not match schema\n";
}

void test_serialize_varchar_exceeds_max_length_throws() {
    TableSchema schema = make_users_schema();  // name max_length = 50

    Row row;
    row.values = {int32_t(1), std::string(51, 'A'), int32_t(25), bool(true)};

    bool threw = false;
    try {
        RowSerializer::serialize(row, schema);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);

    std::cout << "[PASS] serialize throws when VARCHAR exceeds max_length\n";
}

void test_serialize_wrong_type_throws() {
    TableSchema schema = make_users_schema();

    Row row;
    // id expects INT but gets a string
    row.values = {std::string("wrong"), std::string("Alice"), int32_t(25), bool(true)};

    bool threw = false;
    try {
        RowSerializer::serialize(row, schema);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);

    std::cout << "[PASS] serialize throws when value type does not match schema column type\n";
}

void test_serialize_produces_smaller_blob_with_nulls() {
    TableSchema schema = make_users_schema();

    Row full_row;
    full_row.values = {int32_t(1), std::string("Alice"), int32_t(25), bool(true)};

    Row null_row;
    null_row.values = {int32_t(1), std::string("Alice"), std::monostate{}, bool(true)};

    auto full_bytes = RowSerializer::serialize(full_row, schema);
    auto null_bytes = RowSerializer::serialize(null_row, schema);

    // null row should be smaller since age (4 bytes INT) is omitted
    assert(null_bytes.size() < full_bytes.size());

    std::cout << "[PASS] NULL columns produce smaller serialized blob than non-NULL\n";
}

void test_serialize_negative_int() {
    TableSchema schema = make_users_schema();

    Row row;
    row.values = {int32_t(-42), std::string("negative"), int32_t(-1), bool(false)};

    auto bytes = RowSerializer::serialize(row, schema);
    Row result = RowSerializer::deserialize(bytes, schema);

    assert(get_int(result.get(0)) == -42);
    assert(get_int(result.get(2)) == -1);

    std::cout << "[PASS] serialize/deserialize handles negative INT values correctly\n";
}

void test_serialize_boolean_values() {
    TableSchema schema = make_users_schema();

    Row row_true;
    row_true.values = {int32_t(1), std::string("A"), int32_t(1), bool(true)};
    auto bytes_true = RowSerializer::serialize(row_true, schema);
    Row result_true = RowSerializer::deserialize(bytes_true, schema);
    assert(get_bool(result_true.get(3)) == true);

    Row row_false;
    row_false.values = {int32_t(2), std::string("B"), int32_t(2), bool(false)};
    auto bytes_false = RowSerializer::serialize(row_false, schema);
    Row result_false = RowSerializer::deserialize(bytes_false, schema);
    assert(get_bool(result_false.get(3)) == false);

    std::cout << "[PASS] serialize/deserialize handles true and false BOOLEAN correctly\n";
}

int main() {
    std::cout << "\n=== Row Tests ===\n";
    test_row_get_value();
    test_row_get_out_of_range_throws();
    test_row_size();
    test_is_null();
    test_value_to_string();

    std::cout << "\n=== Serializer Tests ===\n";
    test_serialize_deserialize_full_row();
    test_serialize_deserialize_with_null();
    test_serialize_deserialize_float();
    test_serialize_deserialize_nullable_varchar_null();
    test_serialize_deserialize_empty_varchar();
    test_serialize_deserialize_all_nulls();
    test_serialize_null_in_non_nullable_column_throws();
    test_serialize_wrong_column_count_throws();
    test_serialize_varchar_exceeds_max_length_throws();
    test_serialize_wrong_type_throws();
    test_serialize_produces_smaller_blob_with_nulls();
    test_serialize_negative_int();
    test_serialize_boolean_values();

    std::cout << "\nAll tests passed.\n";
    return 0;
}