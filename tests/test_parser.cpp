// g++ -std=c++17 tests/test_parser.cpp src/parser/parser.cpp src/parser/tokenizer.cpp src/row/serializer.cpp src/catalog/catalog_manager.cpp src/storage/disk_manager.cpp src/storage/header_manager.cpp src/storage/buffer_pool.cpp src/wal/wal_manager.cpp -o tests/test_parser && ./tests/test_parser
#include <iostream>
#include <cassert>
#include <stdexcept>
#include <cmath>

#include "../src/parser/token.h"
#include "../src/parser/tokenizer.h"
#include "../src/parser/ast.h"
#include "../src/parser/parser.h"

// helper — tokenize and parse in one step
Statement parse(const std::string& sql) {
    Tokenizer t(sql);
    Parser p(t.tokenize());
    return p.parse();
}

// ─────────────────────────────────────────────
// SELECT Tests
// ─────────────────────────────────────────────

void test_select_star() {
    auto stmt = parse("SELECT * FROM users;");
    auto& s = std::get<SelectStmt>(stmt);

    assert(s.columns.size() == 1);
    assert(s.columns[0].is_star == true);
    assert(s.table_name == "users");
    assert(s.where == nullptr);

    std::cout << "[PASS] SELECT * FROM table\n";
}

void test_select_columns() {
    auto stmt = parse("SELECT id, name, age FROM users;");
    auto& s = std::get<SelectStmt>(stmt);

    assert(s.columns.size() == 3);
    assert(s.columns[0].column.column_name == "id");
    assert(s.columns[1].column.column_name == "name");
    assert(s.columns[2].column.column_name == "age");
    assert(s.table_name == "users");

    std::cout << "[PASS] SELECT col1, col2 FROM table\n";
}

void test_select_with_where_eq() {
    auto stmt = parse("SELECT * FROM users WHERE id = 1;");
    auto& s = std::get<SelectStmt>(stmt);

    assert(s.where != nullptr);
    assert(s.where->kind == WhereExpr::Kind::COMPARE);
    assert(s.where->compare.column.column_name == "id");
    assert(s.where->compare.op == CompareOp::EQ);
    assert(std::get<int32_t>(s.where->compare.operand) == 1);

    std::cout << "[PASS] SELECT * FROM table WHERE col = val\n";
}

void test_select_with_where_gt() {
    auto stmt = parse("SELECT * FROM users WHERE age > 25;");
    auto& s = std::get<SelectStmt>(stmt);

    assert(s.where->compare.op == CompareOp::GT);
    assert(std::get<int32_t>(s.where->compare.operand) == 25);

    std::cout << "[PASS] SELECT * FROM table WHERE col > val\n";
}

void test_select_with_where_string() {
    auto stmt = parse("SELECT * FROM users WHERE name = 'Alice';");
    auto& s = std::get<SelectStmt>(stmt);

    assert(s.where->compare.op == CompareOp::EQ);
    assert(std::get<std::string>(s.where->compare.operand) == "Alice");

    std::cout << "[PASS] SELECT * FROM table WHERE col = 'string'\n";
}

void test_select_where_and() {
    auto stmt = parse("SELECT * FROM users WHERE age > 18 AND active = TRUE;");
    auto& s = std::get<SelectStmt>(stmt);

    assert(s.where->kind       == WhereExpr::Kind::LOGICAL);
    assert(s.where->logical_op == LogicalOp::AND);
    assert(s.where->left  != nullptr);
    assert(s.where->right != nullptr);

    std::cout << "[PASS] SELECT * FROM table WHERE expr AND expr\n";
}

void test_select_where_or() {
    auto stmt = parse("SELECT * FROM users WHERE age < 18 OR age > 65;");
    auto& s = std::get<SelectStmt>(stmt);

    assert(s.where->kind       == WhereExpr::Kind::LOGICAL);
    assert(s.where->logical_op == LogicalOp::OR);

    std::cout << "[PASS] SELECT * FROM table WHERE expr OR expr\n";
}

void test_select_where_not() {
    auto stmt = parse("SELECT * FROM users WHERE NOT active = TRUE;");
    auto& s = std::get<SelectStmt>(stmt);

    assert(s.where->kind       == WhereExpr::Kind::LOGICAL);
    assert(s.where->logical_op == LogicalOp::NOT);
    assert(s.where->left  != nullptr);
    assert(s.where->right == nullptr);

    std::cout << "[PASS] SELECT * FROM table WHERE NOT expr\n";
}

void test_select_where_is_null() {
    auto stmt = parse("SELECT * FROM users WHERE age IS NULL;");
    auto& s = std::get<SelectStmt>(stmt);

    assert(s.where->compare.op == CompareOp::IS_NULL);
    assert(s.where->compare.column.column_name == "age");

    std::cout << "[PASS] SELECT * FROM table WHERE col IS NULL\n";
}

void test_select_where_is_not_null() {
    auto stmt = parse("SELECT * FROM users WHERE age IS NOT NULL;");
    auto& s = std::get<SelectStmt>(stmt);

    assert(s.where->compare.op == CompareOp::IS_NOT_NULL);

    std::cout << "[PASS] SELECT * FROM table WHERE col IS NOT NULL\n";
}

void test_select_order_by_asc() {
    auto stmt = parse("SELECT * FROM users ORDER BY age ASC;");
    auto& s = std::get<SelectStmt>(stmt);

    assert(s.order_by.size() == 1);
    assert(s.order_by[0].column.column_name == "age");
    assert(s.order_by[0].ascending == true);

    std::cout << "[PASS] SELECT * FROM table ORDER BY col ASC\n";
}

void test_select_order_by_desc() {
    auto stmt = parse("SELECT * FROM users ORDER BY age DESC;");
    auto& s = std::get<SelectStmt>(stmt);

    assert(s.order_by[0].ascending == false);

    std::cout << "[PASS] SELECT * FROM table ORDER BY col DESC\n";
}

void test_select_limit() {
    auto stmt = parse("SELECT * FROM users LIMIT 10;");
    auto& s = std::get<SelectStmt>(stmt);

    assert(s.limit.has_value());
    assert(s.limit.value() == 10);

    std::cout << "[PASS] SELECT * FROM table LIMIT n\n";
}

void test_select_inner_join() {
    auto stmt = parse("SELECT * FROM orders INNER JOIN users ON orders.user_id = users.id;");
    auto& s = std::get<SelectStmt>(stmt);

    assert(s.joins.size() == 1);
    assert(s.joins[0].type == JoinType::INNER);
    assert(s.joins[0].table_name == "users");
    assert(s.joins[0].left_col.column_name  == "user_id");
    assert(s.joins[0].right_col.column_name == "id");

    std::cout << "[PASS] SELECT * FROM t1 INNER JOIN t2 ON t1.col = t2.col\n";
}

void test_select_left_join() {
    auto stmt = parse("SELECT * FROM orders LEFT JOIN users ON orders.user_id = users.id;");
    auto& s = std::get<SelectStmt>(stmt);

    assert(s.joins[0].type == JoinType::LEFT);

    std::cout << "[PASS] SELECT * FROM t1 LEFT JOIN t2 ON ...\n";
}

void test_select_right_join() {
    auto stmt = parse("SELECT * FROM orders RIGHT JOIN users ON orders.user_id = users.id;");
    auto& s = std::get<SelectStmt>(stmt);

    assert(s.joins[0].type == JoinType::RIGHT);

    std::cout << "[PASS] SELECT * FROM t1 RIGHT JOIN t2 ON ...\n";
}

void test_select_qualified_column() {
    auto stmt = parse("SELECT users.name FROM users;");
    auto& s = std::get<SelectStmt>(stmt);

    assert(s.columns[0].column.table_name   == "users");
    assert(s.columns[0].column.column_name  == "name");

    std::cout << "[PASS] SELECT table.column FROM table\n";
}

// ─────────────────────────────────────────────
// INSERT Tests
// ─────────────────────────────────────────────

void test_insert_with_columns() {
    auto stmt = parse("INSERT INTO users (name, age) VALUES ('Alice', 25);");
    auto& s = std::get<InsertStmt>(stmt);

    assert(s.table_name == "users");
    assert(s.columns.size() == 2);
    assert(s.columns[0] == "name");
    assert(s.columns[1] == "age");
    assert(s.values.size() == 2);
    assert(std::get<std::string>(s.values[0]) == "Alice");
    assert(std::get<int32_t>(s.values[1]) == 25);

    std::cout << "[PASS] INSERT INTO table (cols) VALUES (vals)\n";
}

void test_insert_without_columns() {
    auto stmt = parse("INSERT INTO users VALUES (1, 'Bob', 30, TRUE);");
    auto& s = std::get<InsertStmt>(stmt);

    assert(s.columns.empty());
    assert(s.values.size() == 4);
    assert(std::get<int32_t>(s.values[0]) == 1);
    assert(std::get<std::string>(s.values[1]) == "Bob");
    assert(std::get<int32_t>(s.values[2]) == 30);
    assert(std::get<bool>(s.values[3]) == true);

    std::cout << "[PASS] INSERT INTO table VALUES (vals)\n";
}

void test_insert_with_null() {
    auto stmt = parse("INSERT INTO users VALUES (1, 'Alice', NULL, TRUE);");
    auto& s = std::get<InsertStmt>(stmt);

    assert(is_null(s.values[2]));

    std::cout << "[PASS] INSERT with NULL value\n";
}

void test_insert_with_float() {
    auto stmt = parse("INSERT INTO products VALUES (1, 9.99, 'Widget');");
    auto& s = std::get<InsertStmt>(stmt);

    assert(std::fabs(std::get<float>(s.values[1]) - 9.99f) < 0.001f);

    std::cout << "[PASS] INSERT with FLOAT value\n";
}

// ─────────────────────────────────────────────
// UPDATE Tests
// ─────────────────────────────────────────────

void test_update_single_column() {
    auto stmt = parse("UPDATE users SET age = 30 WHERE id = 1;");
    auto& s = std::get<UpdateStmt>(stmt);

    assert(s.table_name == "users");
    assert(s.assignments.size() == 1);
    assert(s.assignments[0].column_name == "age");
    assert(std::get<int32_t>(s.assignments[0].value) == 30);
    assert(s.where != nullptr);

    std::cout << "[PASS] UPDATE table SET col = val WHERE ...\n";
}

void test_update_multiple_columns() {
    auto stmt = parse("UPDATE users SET name = 'Bob', age = 25 WHERE id = 1;");
    auto& s = std::get<UpdateStmt>(stmt);

    assert(s.assignments.size() == 2);
    assert(s.assignments[0].column_name == "name");
    assert(s.assignments[1].column_name == "age");

    std::cout << "[PASS] UPDATE table SET col1 = val1, col2 = val2 WHERE ...\n";
}

void test_update_without_where() {
    auto stmt = parse("UPDATE users SET active = FALSE;");
    auto& s = std::get<UpdateStmt>(stmt);

    assert(s.where == nullptr);
    assert(std::get<bool>(s.assignments[0].value) == false);

    std::cout << "[PASS] UPDATE table SET col = val (no WHERE)\n";
}

// ─────────────────────────────────────────────
// DELETE Tests
// ─────────────────────────────────────────────

void test_delete_with_where() {
    auto stmt = parse("DELETE FROM users WHERE id = 1;");
    auto& s = std::get<DeleteStmt>(stmt);

    assert(s.table_name == "users");
    assert(s.where != nullptr);
    assert(s.where->compare.column.column_name == "id");

    std::cout << "[PASS] DELETE FROM table WHERE ...\n";
}

void test_delete_without_where() {
    auto stmt = parse("DELETE FROM users;");
    auto& s = std::get<DeleteStmt>(stmt);

    assert(s.table_name == "users");
    assert(s.where == nullptr);

    std::cout << "[PASS] DELETE FROM table (no WHERE)\n";
}

// ─────────────────────────────────────────────
// CREATE TABLE Tests
// ─────────────────────────────────────────────

void test_create_table_basic() {
    auto stmt = parse(
        "CREATE TABLE users ("
        "  id INT PRIMARY KEY AUTO_INCREMENT,"
        "  name VARCHAR(50),"
        "  age INT,"
        "  active BOOLEAN"
        ");");
    auto& s = std::get<CreateTableStmt>(stmt);

    assert(s.table_name == "users");
    assert(s.columns.size() == 4);

    assert(s.columns[0].name          == "id");
    assert(s.columns[0].type_name     == "INT");
    assert(s.columns[0].is_primary_key == true);
    assert(s.columns[0].auto_increment == true);

    assert(s.columns[1].name        == "name");
    assert(s.columns[1].type_name   == "VARCHAR");
    assert(s.columns[1].varchar_len == 50);

    assert(s.columns[2].name      == "age");
    assert(s.columns[2].type_name == "INT");

    assert(s.columns[3].name      == "active");
    assert(s.columns[3].type_name == "BOOLEAN");

    std::cout << "[PASS] CREATE TABLE with multiple column types\n";
}

void test_create_table_float_column() {
    auto stmt = parse("CREATE TABLE products (id INT PRIMARY KEY, price FLOAT);");
    auto& s = std::get<CreateTableStmt>(stmt);

    assert(s.columns[1].type_name == "FLOAT");

    std::cout << "[PASS] CREATE TABLE with FLOAT column\n";
}

// ─────────────────────────────────────────────
// DROP TABLE Tests
// ─────────────────────────────────────────────

void test_drop_table() {
    auto stmt = parse("DROP TABLE users;");
    auto& s = std::get<DropTableStmt>(stmt);

    assert(s.table_name == "users");

    std::cout << "[PASS] DROP TABLE name\n";
}

// ─────────────────────────────────────────────
// Database management Tests
// ─────────────────────────────────────────────

void test_create_database() {
    auto stmt = parse("CREATE DATABASE school;");
    auto& s = std::get<CreateDatabaseStmt>(stmt);

    assert(s.name == "school");

    std::cout << "[PASS] CREATE DATABASE name\n";
}

void test_drop_database() {
    auto stmt = parse("DROP DATABASE school;");
    auto& s = std::get<DropDatabaseStmt>(stmt);

    assert(s.name == "school");

    std::cout << "[PASS] DROP DATABASE name\n";
}

void test_use_database() {
    auto stmt = parse("USE school;");
    auto& s = std::get<UseStmt>(stmt);

    assert(s.name == "school");

    std::cout << "[PASS] USE database_name\n";
}

void test_show_tables() {
    auto stmt = parse("SHOW TABLES;");
    auto& s = std::get<ShowStmt>(stmt);

    assert(s.target == ShowTarget::TABLES);

    std::cout << "[PASS] SHOW TABLES\n";
}

void test_show_databases() {
    auto stmt = parse("SHOW DATABASES;");
    auto& s = std::get<ShowStmt>(stmt);

    assert(s.target == ShowTarget::DATABASES);

    std::cout << "[PASS] SHOW DATABASES\n";
}

// ─────────────────────────────────────────────
// Error Tests
// ─────────────────────────────────────────────

void test_unknown_statement_throws() {
    bool threw = false;
    try {
        parse("BLAH users;");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);

    std::cout << "[PASS] unknown statement throws\n";
}

void test_missing_from_throws() {
    bool threw = false;
    try {
        parse("SELECT * users;");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);

    std::cout << "[PASS] SELECT missing FROM throws\n";
}

void test_missing_table_name_throws() {
    bool threw = false;
    try {
        parse("SELECT * FROM;");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);

    std::cout << "[PASS] SELECT missing table name throws\n";
}

void test_missing_values_keyword_throws() {
    bool threw = false;
    try {
        parse("INSERT INTO users (name) (1);");
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);

    std::cout << "[PASS] INSERT missing VALUES keyword throws\n";
}

// ─────────────────────────────────────────────
// Complex queries
// ─────────────────────────────────────────────

void test_complex_select() {
    auto stmt = parse(
        "SELECT users.name, orders.total "
        "FROM orders "
        "INNER JOIN users ON orders.user_id = users.id "
        "WHERE orders.total > 100 AND users.active = TRUE "
        "ORDER BY orders.total DESC "
        "LIMIT 10;");

    auto& s = std::get<SelectStmt>(stmt);

    assert(s.columns.size()   == 2);
    assert(s.table_name       == "orders");
    assert(s.joins.size()     == 1);
    assert(s.joins[0].type    == JoinType::INNER);
    assert(s.where            != nullptr);
    assert(s.where->logical_op == LogicalOp::AND);
    assert(s.order_by.size()  == 1);
    assert(s.order_by[0].ascending == false);
    assert(s.limit.value()    == 10);

    std::cout << "[PASS] complex SELECT with JOIN, WHERE AND, ORDER BY, LIMIT\n";
}

void test_where_nested_and_or() {
    auto stmt = parse("SELECT * FROM users WHERE age > 18 AND active = TRUE OR name = 'Admin';");
    auto& s = std::get<SelectStmt>(stmt);

    // OR has lower precedence than AND, so tree is: (age>18 AND active=TRUE) OR name='Admin'
    assert(s.where->kind       == WhereExpr::Kind::LOGICAL);
    assert(s.where->logical_op == LogicalOp::OR);
    assert(s.where->left->logical_op == LogicalOp::AND);

    std::cout << "[PASS] WHERE AND/OR precedence: AND binds tighter than OR\n";
}

int main() {
    std::cout << "\n=== SELECT Tests ===\n";
    test_select_star();
    test_select_columns();
    test_select_with_where_eq();
    test_select_with_where_gt();
    test_select_with_where_string();
    test_select_where_and();
    test_select_where_or();
    test_select_where_not();
    test_select_where_is_null();
    test_select_where_is_not_null();
    test_select_order_by_asc();
    test_select_order_by_desc();
    test_select_limit();
    test_select_inner_join();
    test_select_left_join();
    test_select_right_join();
    test_select_qualified_column();

    std::cout << "\n=== INSERT Tests ===\n";
    test_insert_with_columns();
    test_insert_without_columns();
    test_insert_with_null();
    test_insert_with_float();

    std::cout << "\n=== UPDATE Tests ===\n";
    test_update_single_column();
    test_update_multiple_columns();
    test_update_without_where();

    std::cout << "\n=== DELETE Tests ===\n";
    test_delete_with_where();
    test_delete_without_where();

    std::cout << "\n=== CREATE TABLE Tests ===\n";
    test_create_table_basic();
    test_create_table_float_column();

    std::cout << "\n=== DROP TABLE Tests ===\n";
    test_drop_table();

    std::cout << "\n=== Database Management Tests ===\n";
    test_create_database();
    test_drop_database();
    test_use_database();
    test_show_tables();
    test_show_databases();

    std::cout << "\n=== Error Tests ===\n";
    test_unknown_statement_throws();
    test_missing_from_throws();
    test_missing_table_name_throws();
    test_missing_values_keyword_throws();

    std::cout << "\n=== Complex Query Tests ===\n";
    test_complex_select();
    test_where_nested_and_or();

    std::cout << "\nAll parser tests passed.\n";
    return 0;
}