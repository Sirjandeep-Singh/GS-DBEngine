#include "parser.h"

#include <stdexcept>
#include <cmath>

Parser::Parser(std::vector<Token> tokens)
    : tokens_(std::move(tokens)), pos_(0)
{}

// ─────────────────────────────────────────────
// Token navigation
// ─────────────────────────────────────────────

const Token& Parser::peek() const {
    return tokens_[pos_];
}

const Token& Parser::advance() {
    const Token& t = tokens_[pos_];
    if (t.type != TokenType::END_OF_FILE) pos_++;
    return t;
}

bool Parser::check(TokenType type) const {
    return peek().type == type;
}

bool Parser::match(TokenType type) {
    if (check(type)) { advance(); return true; }
    return false;
}

const Token& Parser::expect(TokenType type, const std::string& context) {
    if (!check(type)) {
        throw std::runtime_error(
            "Parser: expected " + token_type_name(type)
            + " in " + context
            + " but got '" + peek().value + "'"
            + " at line " + std::to_string(peek().line)
            + " col "  + std::to_string(peek().col));
    }
    return advance();
}

bool Parser::at_end() const {
    return peek().type == TokenType::END_OF_FILE;
}

std::string Parser::error_msg(const std::string& expected) const {
    return "Parser: expected " + expected
         + " but got '" + peek().value + "'"
         + " at line " + std::to_string(peek().line)
         + " col "  + std::to_string(peek().col);
}

// ─────────────────────────────────────────────
// Entry point
// ─────────────────────────────────────────────

Statement Parser::parse() {
    Statement stmt = parse_statement();
    match(TokenType::SEMICOLON);  // optional trailing semicolon
    if (!at_end()) {
        throw std::runtime_error(error_msg("end of input"));
    }
    return stmt;
}

Statement Parser::parse_statement() {
    if (check(TokenType::SELECT)) return parse_select();
    if (check(TokenType::INSERT)) return parse_insert();
    if (check(TokenType::UPDATE)) return parse_update();
    if (check(TokenType::DELETE)) return parse_delete();
    if (check(TokenType::DROP))   {
        advance();
        if (check(TokenType::TABLE))    return parse_drop_table();
        if (check(TokenType::DATABASE)) return parse_drop_database();
        throw std::runtime_error(error_msg("TABLE or DATABASE after DROP"));
    }
    if (check(TokenType::CREATE)) {
        advance();
        if (check(TokenType::TABLE))    return parse_create_table();
        if (check(TokenType::DATABASE)) return parse_create_database();
        throw std::runtime_error(error_msg("TABLE or DATABASE after CREATE"));
    }
    if (check(TokenType::USE))  return parse_use();
    if (check(TokenType::SHOW)) return parse_show();

    throw std::runtime_error(error_msg("SELECT, INSERT, UPDATE, DELETE, CREATE, DROP, USE, or SHOW"));
}

// ─────────────────────────────────────────────
// SELECT
// ─────────────────────────────────────────────

Statement Parser::parse_select() {
    return parse_select_body();
}

SelectStmt Parser::parse_select_body() {
    expect(TokenType::SELECT, "SELECT");

    SelectStmt stmt;
    stmt.columns = parse_select_columns();

    expect(TokenType::FROM, "SELECT");
    stmt.table_name = expect(TokenType::IDENTIFIER, "SELECT FROM").value;

    // optional table alias
    if (check(TokenType::IDENTIFIER)) {
        stmt.table_alias = advance().value;
    }

    // zero or more JOINs
    while (check(TokenType::INNER) || check(TokenType::LEFT) ||
           check(TokenType::RIGHT) || check(TokenType::JOIN)) {
        stmt.joins.push_back(parse_join());
    }

    // optional WHERE
    stmt.where = parse_where();

    // optional ORDER BY
    if (check(TokenType::ORDER)) {
        stmt.order_by = parse_order_by();
    }

    // optional LIMIT
    if (match(TokenType::LIMIT)) {
        const Token& n = expect(TokenType::INTEGER_LITERAL, "LIMIT");
        stmt.limit = static_cast<uint32_t>(std::stoul(n.value));
    }

    return stmt;
}

std::vector<SelectColumn> Parser::parse_select_columns() {
    std::vector<SelectColumn> cols;

    if (check(TokenType::STAR)) {
        advance();
        cols.push_back({true, {}, AggregateType::NONE});
        return cols;
    }

    do {
        SelectColumn col;
        if (match(TokenType::COUNT)) {
            expect(TokenType::LPAREN, "COUNT(...)");
            if (match(TokenType::STAR)) {
                col.aggregate = AggregateType::COUNT_STAR;
            } else {
                col.column    = parse_column_ref();
                col.aggregate = AggregateType::COUNT_COLUMN;
            }
            expect(TokenType::RPAREN, "COUNT(...)");
        } else {
            col.is_star = false;
            col.column  = parse_column_ref();
        }
        cols.push_back(std::move(col));
    } while (match(TokenType::COMMA));

    return cols;
}

JoinClause Parser::parse_join() {
    JoinClause join;

    if (match(TokenType::INNER)) {
        join.type = JoinType::INNER;
        expect(TokenType::JOIN, "INNER JOIN");
    } else if (match(TokenType::LEFT)) {
        join.type = JoinType::LEFT;
        expect(TokenType::JOIN, "LEFT JOIN");
    } else if (match(TokenType::RIGHT)) {
        join.type = JoinType::RIGHT;
        expect(TokenType::JOIN, "RIGHT JOIN");
    } else {
        // bare JOIN defaults to INNER
        expect(TokenType::JOIN, "JOIN");
        join.type = JoinType::INNER;
    }

    join.table_name = expect(TokenType::IDENTIFIER, "JOIN table name").value;

    // optional alias
    if (check(TokenType::IDENTIFIER)) {
        join.alias = advance().value;
    }

    expect(TokenType::ON, "JOIN ON");

    join.left_col  = parse_column_ref();
    expect(TokenType::EQ, "JOIN ON condition");
    join.right_col = parse_column_ref();

    return join;
}

std::vector<OrderByClause> Parser::parse_order_by() {
    expect(TokenType::ORDER, "ORDER BY");
    expect(TokenType::BY,    "ORDER BY");

    std::vector<OrderByClause> clauses;
    do {
        OrderByClause clause;
        clause.column    = parse_column_ref();
        clause.ascending = true;
        if (match(TokenType::DESC)) clause.ascending = false;
        else match(TokenType::ASC);  // optional ASC, default anyway
        clauses.push_back(std::move(clause));
    } while (match(TokenType::COMMA));

    return clauses;
}

// ─────────────────────────────────────────────
// INSERT
// ─────────────────────────────────────────────

Statement Parser::parse_insert() {
    expect(TokenType::INSERT, "INSERT");
    expect(TokenType::INTO,   "INSERT INTO");

    InsertStmt stmt;
    stmt.table_name = expect(TokenType::IDENTIFIER, "INSERT INTO table").value;

    // optional column list: (col1, col2, ...)
    if (match(TokenType::LPAREN)) {
        do {
            stmt.columns.push_back(expect(TokenType::IDENTIFIER, "INSERT column list").value);
        } while (match(TokenType::COMMA));
        expect(TokenType::RPAREN, "INSERT column list");
    }

    expect(TokenType::VALUES, "INSERT VALUES");
    stmt.values = parse_value_list();

    return stmt;
}

// ─────────────────────────────────────────────
// UPDATE
// ─────────────────────────────────────────────

Statement Parser::parse_update() {
    expect(TokenType::UPDATE, "UPDATE");

    UpdateStmt stmt;
    stmt.table_name = expect(TokenType::IDENTIFIER, "UPDATE table").value;

    expect(TokenType::SET, "UPDATE SET");

    do {
        SetClause sc;
        sc.column_name = expect(TokenType::IDENTIFIER, "UPDATE SET column").value;
        expect(TokenType::EQ, "UPDATE SET =");
        sc.value = parse_value();
        stmt.assignments.push_back(std::move(sc));
    } while (match(TokenType::COMMA));

    stmt.where = parse_where();

    return stmt;
}

// ─────────────────────────────────────────────
// DELETE
// ─────────────────────────────────────────────

Statement Parser::parse_delete() {
    expect(TokenType::DELETE, "DELETE");
    expect(TokenType::FROM,   "DELETE FROM");

    DeleteStmt stmt;
    stmt.table_name = expect(TokenType::IDENTIFIER, "DELETE FROM table").value;
    stmt.where      = parse_where();

    return stmt;
}

// ─────────────────────────────────────────────
// CREATE TABLE
// ─────────────────────────────────────────────

Statement Parser::parse_create_table() {
    expect(TokenType::TABLE, "CREATE TABLE");

    CreateTableStmt stmt;
    stmt.table_name = expect(TokenType::IDENTIFIER, "CREATE TABLE name").value;

    expect(TokenType::LPAREN, "CREATE TABLE columns");

    do {
        if (check(TokenType::CHECK)) {
            // table-level constraint: CHECK (expr) — not tied to one column def
            advance();  // consume CHECK
            expect(TokenType::LPAREN, "CHECK (...)");
            stmt.table_checks.push_back(parse_or_expr());
            expect(TokenType::RPAREN, "CHECK (...)");
        } else {
            stmt.columns.push_back(parse_column_def());
        }
    } while (match(TokenType::COMMA));

    expect(TokenType::RPAREN, "CREATE TABLE columns");

    return stmt;
}

ColumnDef Parser::parse_column_def() {
    ColumnDef def;
    def.name        = expect(TokenType::IDENTIFIER, "column definition name").value;
    def.varchar_len = 0;
    def.is_primary_key  = false;
    def.is_nullable     = true;
    def.auto_increment  = false;

    // type
    if (match(TokenType::INT_KW))     { def.type_name = "INT"; }
    else if (match(TokenType::FLOAT_KW))   { def.type_name = "FLOAT"; }
    else if (match(TokenType::BOOLEAN_KW)) { def.type_name = "BOOLEAN"; }
    else if (match(TokenType::VARCHAR_KW)) {
        def.type_name = "VARCHAR";
        expect(TokenType::LPAREN, "VARCHAR(n)");
        def.varchar_len = static_cast<uint32_t>(
            std::stoul(expect(TokenType::INTEGER_LITERAL, "VARCHAR length").value));
        expect(TokenType::RPAREN, "VARCHAR(n)");
    } else {
        throw std::runtime_error(error_msg("column type (INT, FLOAT, BOOLEAN, VARCHAR)"));
    }

    // optional constraints — can appear in any order
    bool parsing_constraints = true;
    while (parsing_constraints) {
        if (check(TokenType::PRIMARY) && pos_ + 1 < tokens_.size()
            && tokens_[pos_ + 1].type == TokenType::KEY) {
            advance(); advance();  // consume PRIMARY KEY
            def.is_primary_key = true;
            def.is_nullable    = false;
        } else if (match(TokenType::AUTO_INCREMENT)) {
            def.auto_increment = true;
        } else if (check(TokenType::NOT) && pos_ + 1 < tokens_.size()
                   && tokens_[pos_ + 1].type == TokenType::NULL_KW) {
            advance(); advance();  // consume NOT NULL
            def.is_nullable = false;
        } else if (match(TokenType::CHECK)) {
            expect(TokenType::LPAREN, "CHECK (...)");
            def.check = parse_or_expr();
            expect(TokenType::RPAREN, "CHECK (...)");
        } else {
            parsing_constraints = false;
        }
    }

    return def;
}

// ─────────────────────────────────────────────
// DROP TABLE / DROP DATABASE
// ─────────────────────────────────────────────

Statement Parser::parse_drop_table() {
    expect(TokenType::TABLE, "DROP TABLE");
    DropTableStmt stmt;
    stmt.table_name = expect(TokenType::IDENTIFIER, "DROP TABLE name").value;
    return stmt;
}

Statement Parser::parse_drop_database() {
    expect(TokenType::DATABASE, "DROP DATABASE");
    DropDatabaseStmt stmt;
    stmt.name = expect(TokenType::IDENTIFIER, "DROP DATABASE name").value;
    return stmt;
}

// ─────────────────────────────────────────────
// CREATE DATABASE
// ─────────────────────────────────────────────

Statement Parser::parse_create_database() {
    expect(TokenType::DATABASE, "CREATE DATABASE");
    CreateDatabaseStmt stmt;
    stmt.name = expect(TokenType::IDENTIFIER, "CREATE DATABASE name").value;
    return stmt;
}

// ─────────────────────────────────────────────
// USE
// ─────────────────────────────────────────────

Statement Parser::parse_use() {
    expect(TokenType::USE, "USE");
    UseStmt stmt;
    stmt.name = expect(TokenType::IDENTIFIER, "USE database name").value;
    return stmt;
}

// ─────────────────────────────────────────────
// SHOW
// ─────────────────────────────────────────────

Statement Parser::parse_show() {
    expect(TokenType::SHOW, "SHOW");
    ShowStmt stmt;

    if (match(TokenType::TABLES)) {
        stmt.target = ShowTarget::TABLES;
    } else if (match(TokenType::DATABASES)) {
        stmt.target = ShowTarget::DATABASES;
    } else {
        throw std::runtime_error(error_msg("TABLES or DATABASES after SHOW"));
    }

    return stmt;
}

// ─────────────────────────────────────────────
// WHERE clause
// ─────────────────────────────────────────────

WhereExprPtr Parser::parse_where() {
    if (!match(TokenType::WHERE)) return nullptr;
    return parse_or_expr();
}

// OR has the lowest precedence
WhereExprPtr Parser::parse_or_expr() {
    auto left = parse_and_expr();

    while (match(TokenType::OR)) {
        auto right = parse_and_expr();
        auto node  = std::make_unique<WhereExpr>();
        node->kind       = WhereExpr::Kind::LOGICAL;
        node->logical_op = LogicalOp::OR;
        node->left       = std::move(left);
        node->right      = std::move(right);
        left = std::move(node);
    }

    return left;
}

// AND has higher precedence than OR
WhereExprPtr Parser::parse_and_expr() {
    auto left = parse_not_expr();

    while (match(TokenType::AND)) {
        auto right = parse_not_expr();
        auto node  = std::make_unique<WhereExpr>();
        node->kind       = WhereExpr::Kind::LOGICAL;
        node->logical_op = LogicalOp::AND;
        node->left       = std::move(left);
        node->right      = std::move(right);
        left = std::move(node);
    }

    return left;
}

// NOT has higher precedence than AND
WhereExprPtr Parser::parse_not_expr() {
    if (match(TokenType::NOT)) {
        auto operand = parse_compare_expr();
        auto node    = std::make_unique<WhereExpr>();
        node->kind       = WhereExpr::Kind::LOGICAL;
        node->logical_op = LogicalOp::NOT;
        node->left       = std::move(operand);
        node->right      = nullptr;
        return node;
    }
    return parse_compare_expr();
}

WhereExprPtr Parser::parse_compare_expr() {
    // EXISTS (SELECT ...) — doesn't start with a column reference at all,
    // so it has to be checked before parse_column_ref() is called. NOT
    // EXISTS falls out of parse_not_expr() wrapping this in a LOGICAL/NOT
    // node — no separate handling needed here.
    if (check(TokenType::EXISTS)) {
        advance();
        auto node      = std::make_unique<WhereExpr>();
        node->kind     = WhereExpr::Kind::EXISTS;
        node->subquery = parse_subquery();
        return node;
    }

    auto node = std::make_unique<WhereExpr>();
    node->kind    = WhereExpr::Kind::COMPARE;
    node->compare.column = parse_column_ref();

    // IS NULL / IS NOT NULL
    if (match(TokenType::IS)) {
        if (match(TokenType::NOT)) {
            expect(TokenType::NULL_KW, "IS NOT NULL");
            node->compare.op = CompareOp::IS_NOT_NULL;
        } else {
            expect(TokenType::NULL_KW, "IS NULL");
            node->compare.op = CompareOp::IS_NULL;
        }
        node->compare.operand = std::monostate{};
        return node;
    }

    // column [NOT] IN (SELECT ...)
    if (match(TokenType::NOT)) {
        expect(TokenType::IN, "NOT IN (...)");
        node->compare.op       = CompareOp::NOT_IN_SUBQUERY;
        node->compare.subquery = parse_subquery();
        return node;
    }
    if (match(TokenType::IN)) {
        node->compare.op       = CompareOp::IN_SUBQUERY;
        node->compare.subquery = parse_subquery();
        return node;
    }

    // comparison operators
    if (match(TokenType::EQ))  node->compare.op = CompareOp::EQ;
    else if (match(TokenType::NEQ)) node->compare.op = CompareOp::NEQ;
    else if (match(TokenType::LTE)) node->compare.op = CompareOp::LTE;
    else if (match(TokenType::GTE)) node->compare.op = CompareOp::GTE;
    else if (match(TokenType::LT))  node->compare.op = CompareOp::LT;
    else if (match(TokenType::GT))  node->compare.op = CompareOp::GT;
    else if (match(TokenType::LIKE)) node->compare.op = CompareOp::LIKE;
    else throw std::runtime_error(error_msg("comparison operator (=, !=, <, >, <=, >=, IS, LIKE)"));

    node->compare.operand = parse_value();
    return node;
}

std::unique_ptr<SelectStmt> Parser::parse_subquery() {
    expect(TokenType::LPAREN, "subquery");
    if (!check(TokenType::SELECT)) {
        throw std::runtime_error(error_msg(
            "SELECT (value lists like IN (1, 2, 3) are not supported yet — use a subquery)"));
    }
    auto sub = std::make_unique<SelectStmt>(parse_select_body());
    expect(TokenType::RPAREN, "subquery");
    return sub;
}

// ─────────────────────────────────────────────
// Shared helpers
// ─────────────────────────────────────────────

ColumnRef Parser::parse_column_ref() {
    ColumnRef ref;
    ref.column_name = expect(TokenType::IDENTIFIER, "column reference").value;

    // check for table.column qualified form
    if (match(TokenType::DOT)) {
        ref.table_name  = ref.column_name;
        ref.column_name = expect(TokenType::IDENTIFIER, "qualified column reference").value;
    }

    return ref;
}

Value Parser::parse_value() {
    // handle unary minus for negative numbers
    bool negative = match(TokenType::MINUS);

    if (check(TokenType::INTEGER_LITERAL)) {
        int32_t v = std::stoi(advance().value);
        return negative ? -v : v;
    }
    if (check(TokenType::FLOAT_LITERAL)) {
        float v = std::stof(advance().value);
        return negative ? -v : v;
    }
    if (negative) {
        throw std::runtime_error(error_msg("number after minus sign"));
    }
    if (check(TokenType::STRING_LITERAL)) {
        return std::string(advance().value);
    }
    if (match(TokenType::TRUE_KW))  return bool(true);
    if (match(TokenType::FALSE_KW)) return bool(false);
    if (match(TokenType::NULL_KW))  return std::monostate{};

    throw std::runtime_error(error_msg("literal value (number, string, TRUE, FALSE, NULL)"));
}

std::vector<std::string> Parser::parse_column_list() {
    std::vector<std::string> cols;
    expect(TokenType::LPAREN, "column list");
    do {
        cols.push_back(expect(TokenType::IDENTIFIER, "column name in list").value);
    } while (match(TokenType::COMMA));
    expect(TokenType::RPAREN, "column list");
    return cols;
}

std::vector<Value> Parser::parse_value_list() {
    std::vector<Value> vals;
    expect(TokenType::LPAREN, "value list");
    do {
        vals.push_back(parse_value());
    } while (match(TokenType::COMMA));
    expect(TokenType::RPAREN, "value list");
    return vals;
}