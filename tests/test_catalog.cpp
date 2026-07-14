#include <iostream>
#include <cassert>
#include <filesystem>

#include "../src/storage/disk_manager.h"
#include "../src/storage/buffer_pool.h"
#include "../src/wal/wal_manager.h"
#include "../src/catalog/schema.h"
#include "../src/catalog/catalog_manager.h"

namespace fs = std::filesystem;

static const std::string DB_FILE  = "test_catalog.db";
static const std::string WAL_FILE = "test_catalog.wal";

void cleanup() {
    if (fs::exists(DB_FILE))  fs::remove(DB_FILE);
    if (fs::exists(WAL_FILE)) fs::remove(WAL_FILE);
}

// helper — builds a simple users table schema
TableSchema make_users_schema(uint32_t root_page = INVALID_PAGE) {
    TableSchema schema;
    schema.name              = "users";
    schema.root_page         = root_page;
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
    name.max_length     = 100;
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

    schema.columns = {id, name, age};
    return schema;
}

// helper — builds a simple orders table schema
TableSchema make_orders_schema(uint32_t root_page = INVALID_PAGE) {
    TableSchema schema;
    schema.name              = "orders";
    schema.root_page         = root_page;
    schema.primary_key_indices = {0};

    Column id;
    id.name           = "id";
    id.type           = ColumnType::INT;
    id.max_length     = 0;
    id.is_nullable    = false;
    id.is_primary_key = true;
    id.auto_increment = true;

    Column total;
    total.name           = "total";
    total.type           = ColumnType::FLOAT;
    total.max_length     = 0;
    total.is_nullable    = false;
    total.is_primary_key = false;
    total.auto_increment = false;

    schema.columns = {id, total};
    return schema;
}

// ─────────────────────────────────────────────
// Schema Tests
// ─────────────────────────────────────────────

void test_schema_column_index_found() {
    TableSchema schema = make_users_schema();

    assert(schema.column_index("id")   == 0);
    assert(schema.column_index("name") == 1);
    assert(schema.column_index("age")  == 2);

    std::cout << "[PASS] column_index returns correct index for existing columns\n";
}

void test_schema_column_index_not_found() {
    TableSchema schema = make_users_schema();
    assert(schema.column_index("nonexistent") == -1);
    std::cout << "[PASS] column_index returns -1 for missing column\n";
}

void test_schema_primary_key_column() {
    TableSchema schema = make_users_schema();
    const Column& pk = schema.primary_key_column();
    assert(pk.name           == "id");
    assert(pk.is_primary_key == true);
    assert(pk.auto_increment == true);
    std::cout << "[PASS] primary_key_column returns correct column\n";
}

void test_schema_column_type_size() {
    assert(column_type_size(ColumnType::INT)     == 4);
    assert(column_type_size(ColumnType::FLOAT)   == 4);
    assert(column_type_size(ColumnType::BOOLEAN) == 1);
    assert(column_type_size(ColumnType::VARCHAR) == 0);
    std::cout << "[PASS] column_type_size returns correct sizes\n";
}

void test_schema_column_type_name() {
    assert(column_type_name(ColumnType::INT)     == "INT");
    assert(column_type_name(ColumnType::FLOAT)   == "FLOAT");
    assert(column_type_name(ColumnType::BOOLEAN) == "BOOLEAN");
    assert(column_type_name(ColumnType::VARCHAR) == "VARCHAR");
    std::cout << "[PASS] column_type_name returns correct names\n";
}

// ─────────────────────────────────────────────
// CatalogManager Tests
// ─────────────────────────────────────────────

void test_catalog_create_and_get_table() {
    cleanup();
    DiskManager    dm(DB_FILE);
    dm.allocate_page();  // page 0 — simulates HeaderManager
    BufferPool     bp(dm);
    WALManager     wal(WAL_FILE, bp);
    CatalogManager cat(bp, wal);
    cat.load(true);

    cat.create_table(make_users_schema());

    assert(cat.table_exists("users"));
    const TableSchema& schema = cat.get_table("users");
    assert(schema.name             == "users");
    assert(schema.columns.size()   == 3);
    assert(schema.columns[0].name  == "id");
    assert(schema.columns[1].name  == "name");
    assert(schema.columns[2].name  == "age");

    std::cout << "[PASS] create_table and get_table work correctly\n";
    cleanup();
}

void test_catalog_table_exists() {
    cleanup();
    DiskManager    dm(DB_FILE);
    dm.allocate_page();  // page 0 — simulates HeaderManager
    BufferPool     bp(dm);
    WALManager     wal(WAL_FILE, bp);
    CatalogManager cat(bp, wal);
    cat.load(true);

    assert(!cat.table_exists("users"));
    cat.create_table(make_users_schema());
    assert(cat.table_exists("users"));

    std::cout << "[PASS] table_exists returns correct result\n";
    cleanup();
}

void test_catalog_duplicate_table_throws() {
    cleanup();
    DiskManager    dm(DB_FILE);
    dm.allocate_page();  // page 0 — simulates HeaderManager
    BufferPool     bp(dm);
    WALManager     wal(WAL_FILE, bp);
    CatalogManager cat(bp, wal);
    cat.load(true);

    cat.create_table(make_users_schema());

    bool threw = false;
    try {
        cat.create_table(make_users_schema());
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);

    std::cout << "[PASS] create_table throws on duplicate table name\n";
    cleanup();
}

void test_catalog_drop_table() {
    cleanup();
    DiskManager    dm(DB_FILE);
    dm.allocate_page();  // page 0 — simulates HeaderManager
    BufferPool     bp(dm);
    WALManager     wal(WAL_FILE, bp);
    CatalogManager cat(bp, wal);
    cat.load(true);

    cat.create_table(make_users_schema());
    assert(cat.table_exists("users"));

    cat.drop_table("users");
    assert(!cat.table_exists("users"));

    std::cout << "[PASS] drop_table removes table correctly\n";
    cleanup();
}

void test_catalog_drop_nonexistent_table_throws() {
    cleanup();
    DiskManager    dm(DB_FILE);
    dm.allocate_page();  // page 0 — simulates HeaderManager
    BufferPool     bp(dm);
    WALManager     wal(WAL_FILE, bp);
    CatalogManager cat(bp, wal);
    cat.load(true);

    bool threw = false;
    try {
        cat.drop_table("nonexistent");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);

    std::cout << "[PASS] drop_table throws on nonexistent table\n";
    cleanup();
}

void test_catalog_list_tables() {
    cleanup();
    DiskManager    dm(DB_FILE);
    dm.allocate_page();  // page 0 — simulates HeaderManager
    BufferPool     bp(dm);
    WALManager     wal(WAL_FILE, bp);
    CatalogManager cat(bp, wal);
    cat.load(true);

    assert(cat.list_tables().empty());

    cat.create_table(make_users_schema());
    cat.create_table(make_orders_schema());

    auto tables = cat.list_tables();
    assert(tables.size() == 2);

    std::cout << "[PASS] list_tables returns all table names\n";
    cleanup();
}

void test_catalog_persists_across_restart() {
    cleanup();
    {
        DiskManager    dm(DB_FILE);
        dm.allocate_page();  // page 0 — simulates HeaderManager
        BufferPool     bp(dm);
        WALManager     wal(WAL_FILE, bp);
        CatalogManager cat(bp, wal);
        cat.load(true);

        cat.create_table(make_users_schema());
        cat.create_table(make_orders_schema());
    }

    // reopen
    DiskManager    dm2(DB_FILE);
    BufferPool     bp2(dm2);
    WALManager     wal2(WAL_FILE, bp2);
    wal2.recover();
    CatalogManager cat2(bp2, wal2);
    cat2.load(false);

    assert(cat2.table_exists("users"));
    assert(cat2.table_exists("orders"));

    const TableSchema& users = cat2.get_table("users");
    assert(users.columns.size()  == 3);
    assert(users.columns[0].name == "id");
    assert(users.columns[1].name == "name");
    assert(users.columns[2].name == "age");

    std::cout << "[PASS] catalog persists correctly across restart\n";
    cleanup();
}

void test_catalog_persists_column_default() {
    cleanup();
    {
        DiskManager    dm(DB_FILE);
        dm.allocate_page();  // page 0 — simulates HeaderManager
        BufferPool     bp(dm);
        WALManager     wal(WAL_FILE, bp);
        CatalogManager cat(bp, wal);
        cat.load(true);

        TableSchema schema;
        schema.name                = "t";
        schema.root_page           = INVALID_PAGE;
        schema.primary_key_indices = {0};

        Column id;
        id.name           = "id";
        id.type           = ColumnType::INT;
        id.max_length     = 0;
        id.is_nullable    = false;
        id.is_primary_key = true;
        id.auto_increment = false;

        Column status;
        status.name           = "status";
        status.type           = ColumnType::VARCHAR;
        status.max_length     = 10;
        status.is_nullable    = true;
        status.is_primary_key = false;
        status.auto_increment = false;
        status.has_default    = true;
        status.default_value  = std::string("active");

        schema.columns = {id, status};
        cat.create_table(schema);
    }

    // reopen
    DiskManager    dm2(DB_FILE);
    BufferPool     bp2(dm2);
    WALManager     wal2(WAL_FILE, bp2);
    wal2.recover();
    CatalogManager cat2(bp2, wal2);
    cat2.load(false);

    const TableSchema& t = cat2.get_table("t");
    assert(t.columns[0].has_default == false);
    assert(t.columns[1].has_default == true);
    assert(std::get<std::string>(t.columns[1].default_value) == "active");

    std::cout << "[PASS] column DEFAULT clause persists correctly across restart\n";
    cleanup();
}

void test_catalog_update_table_root() {
    cleanup();
    DiskManager    dm(DB_FILE);
    dm.allocate_page();  // page 0 — simulates HeaderManager
    BufferPool     bp(dm);
    WALManager     wal(WAL_FILE, bp);
    CatalogManager cat(bp, wal);
    cat.load(true);

    cat.create_table(make_users_schema());
    cat.update_table_root("users", 42);

    assert(cat.get_table("users").root_page == 42);

    std::cout << "[PASS] update_table_root updates root page correctly\n";
    cleanup();
}

void test_catalog_create_and_get_index() {
    cleanup();
    DiskManager    dm(DB_FILE);
    dm.allocate_page();  // page 0 — simulates HeaderManager
    BufferPool     bp(dm);
    WALManager     wal(WAL_FILE, bp);
    CatalogManager cat(bp, wal);
    cat.load(true);

    cat.create_table(make_users_schema());

    IndexSchema idx;
    idx.name         = "idx_name";
    idx.table_name   = "users";
    idx.column_names = {"name"};
    idx.root_page    = INVALID_PAGE;
    idx.is_unique    = false;

    cat.create_index(idx);

    const IndexSchema& retrieved = cat.get_index("idx_name");
    assert(retrieved.name         == "idx_name");
    assert(retrieved.table_name   == "users");
    assert(retrieved.column_names == std::vector<std::string>{"name"});
    assert(retrieved.is_composite() == false);
    assert(retrieved.is_unique    == false);

    std::cout << "[PASS] create_index and get_index work correctly\n";
    cleanup();
}

void test_catalog_get_indexes_for_table() {
    cleanup();
    DiskManager    dm(DB_FILE);
    dm.allocate_page();  // page 0 — simulates HeaderManager
    BufferPool     bp(dm);
    WALManager     wal(WAL_FILE, bp);
    CatalogManager cat(bp, wal);
    cat.load(true);

    cat.create_table(make_users_schema());

    IndexSchema idx1;
    idx1.name         = "idx_name";
    idx1.table_name   = "users";
    idx1.column_names = {"name"};
    idx1.root_page    = INVALID_PAGE;
    idx1.is_unique    = false;

    IndexSchema idx2;
    idx2.name         = "idx_age";
    idx2.table_name   = "users";
    idx2.column_names = {"age"};
    idx2.root_page    = INVALID_PAGE;
    idx2.is_unique     = false;

    cat.create_index(idx1);
    cat.create_index(idx2);

    auto indexes = cat.get_indexes_for_table("users");
    assert(indexes.size() == 2);

    std::cout << "[PASS] get_indexes_for_table returns all indexes for a table\n";
    cleanup();
}

int main() {
    std::cout << "\n=== Schema Tests ===\n";
    test_schema_column_index_found();
    test_schema_column_index_not_found();
    test_schema_primary_key_column();
    test_schema_column_type_size();
    test_schema_column_type_name();

    std::cout << "\n=== CatalogManager Tests ===\n";
    test_catalog_create_and_get_table();
    test_catalog_table_exists();
    test_catalog_duplicate_table_throws();
    test_catalog_drop_table();
    test_catalog_drop_nonexistent_table_throws();
    test_catalog_list_tables();
    test_catalog_persists_across_restart();
    test_catalog_persists_column_default();
    test_catalog_update_table_root();
    test_catalog_create_and_get_index();
    test_catalog_get_indexes_for_table();

    std::cout << "\nAll tests passed.\n";
    return 0;
}