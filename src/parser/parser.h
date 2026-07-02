#pragma once

#include <vector>
#include <string>
#include <memory>
#include "token.h"
#include "ast.h"

// Parser takes a flat list of tokens from the Tokenizer and builds an AST.
// Uses recursive descent — one method per grammar rule.
//
// Entry point: parse() returns a Statement variant.
// Throws std::runtime_error with a descriptive message on any syntax error.
//
// Supported statements:
//   SELECT ... FROM ... [JOIN ...] [WHERE ...] [ORDER BY ...] [LIMIT n]
//   INSERT INTO table [(cols)] VALUES (vals)
//   UPDATE table SET col=val [, ...] [WHERE ...]
//   DELETE FROM table [WHERE ...]
//   CREATE TABLE name (col_def [, ...])
//   DROP TABLE name
//   CREATE DATABASE name
//   DROP DATABASE name
//   USE name
//   SHOW TABLES
//   SHOW DATABASES

class Parser {
public:
    explicit Parser(std::vector<Token> tokens);

    // parses the token stream and returns a Statement AST node.
    // throws std::runtime_error on syntax errors.
    Statement parse();

private:
    std::vector<Token> tokens_;
    size_t             pos_;

    // ---- Token navigation ----

    // returns current token without advancing
    const Token& peek() const;

    // returns current token and advances pos_
    const Token& advance();

    // returns true if current token matches type
    bool check(TokenType type) const;

    // if current token matches type, advances and returns true; else false
    bool match(TokenType type);

    // asserts current token is of type, advances and returns it.
    // throws with message if token does not match.
    const Token& expect(TokenType type, const std::string& context);

    // returns true if at end of token stream
    bool at_end() const;

    // formats a syntax error message with current token info
    std::string error_msg(const std::string& expected) const;

    // ---- Statement parsers ----

    Statement parse_statement();

    SelectStmt       parse_select();
    InsertStmt       parse_insert();
    UpdateStmt       parse_update();
    DeleteStmt       parse_delete();
    CreateTableStmt  parse_create_table();
    DropTableStmt    parse_drop_table();
    CreateDatabaseStmt parse_create_database();
    DropDatabaseStmt   parse_drop_database();
    UseStmt          parse_use();
    ShowStmt         parse_show();

    // ---- Clause parsers ----

    // parses WHERE expr — returns nullptr if no WHERE keyword
    WhereExprPtr     parse_where();

    // parses a WHERE expression tree respecting AND/OR precedence
    WhereExprPtr     parse_or_expr();
    WhereExprPtr     parse_and_expr();
    WhereExprPtr     parse_not_expr();
    WhereExprPtr     parse_compare_expr();

    // parses ORDER BY col [ASC|DESC] [, ...]
    std::vector<OrderByClause> parse_order_by();

    // parses a JOIN clause — called repeatedly for multiple JOINs
    JoinClause       parse_join();

    // parses a column list: (col1, col2, ...) — used in INSERT and SELECT
    std::vector<std::string> parse_column_list();

    // parses a value list: (val1, val2, ...) — used in INSERT VALUES
    std::vector<Value>       parse_value_list();

    // parses a single value: integer, float, string, TRUE, FALSE, NULL
    Value parse_value();

    // parses a column reference: name or table.name
    ColumnRef parse_column_ref();

    // parses a column definition inside CREATE TABLE
    ColumnDef parse_column_def();

    // parses SELECT column list: * or col [, col ...]
    std::vector<SelectColumn> parse_select_columns();
};