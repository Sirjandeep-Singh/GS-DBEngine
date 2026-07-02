// g++ -std=c++17 tests/test_tokenizer.cpp src/parser/tokenizer.cpp -o tests/test_tokenizer && ./tests/test_tokenizer
#include <iostream>
#include <cassert>
#include <vector>

#include "../src/parser/token.h"
#include "../src/parser/tokenizer.h"

// helper — tokenize and strip the trailing END_OF_FILE token
std::vector<Token> tokenize(const std::string& sql) {
    Tokenizer t(sql);
    auto tokens = t.tokenize();
    if (!tokens.empty() && tokens.back().type == TokenType::END_OF_FILE) {
        tokens.pop_back();
    }
    return tokens;
}

// helper — assert token at index has expected type and value
void expect(const std::vector<Token>& tokens, size_t idx,
            TokenType type, const std::string& value = "") {
    assert(idx < tokens.size());
    assert(tokens[idx].type == type);
    if (!value.empty()) {
        assert(tokens[idx].value == value);
    }
}

// ─────────────────────────────────────────────
// Basic keyword recognition
// ─────────────────────────────────────────────

void test_keywords_recognized() {
    auto tokens = tokenize("SELECT FROM WHERE INSERT INTO VALUES UPDATE SET DELETE CREATE TABLE DROP");

    assert(tokens[0].type  == TokenType::SELECT);
    assert(tokens[1].type  == TokenType::FROM);
    assert(tokens[2].type  == TokenType::WHERE);
    assert(tokens[3].type  == TokenType::INSERT);
    assert(tokens[4].type  == TokenType::INTO);
    assert(tokens[5].type  == TokenType::VALUES);
    assert(tokens[6].type  == TokenType::UPDATE);
    assert(tokens[7].type  == TokenType::SET);
    assert(tokens[8].type  == TokenType::DELETE);
    assert(tokens[9].type  == TokenType::CREATE);
    assert(tokens[10].type == TokenType::TABLE);
    assert(tokens[11].type == TokenType::DROP);

    std::cout << "[PASS] basic keywords recognized correctly\n";
}

void test_keywords_case_insensitive() {
    auto tokens = tokenize("select FROM Where INSERT");

    assert(tokens[0].type == TokenType::SELECT);
    assert(tokens[1].type == TokenType::FROM);
    assert(tokens[2].type == TokenType::WHERE);
    assert(tokens[3].type == TokenType::INSERT);

    std::cout << "[PASS] keywords are case-insensitive\n";
}

void test_type_keywords() {
    auto tokens = tokenize("INT FLOAT BOOLEAN VARCHAR");

    assert(tokens[0].type == TokenType::INT_KW);
    assert(tokens[1].type == TokenType::FLOAT_KW);
    assert(tokens[2].type == TokenType::BOOLEAN_KW);
    assert(tokens[3].type == TokenType::VARCHAR_KW);

    std::cout << "[PASS] type keywords recognized correctly\n";
}

void test_boolean_keywords() {
    auto tokens = tokenize("TRUE FALSE NULL");

    assert(tokens[0].type == TokenType::TRUE_KW);
    assert(tokens[1].type == TokenType::FALSE_KW);
    assert(tokens[2].type == TokenType::NULL_KW);

    std::cout << "[PASS] TRUE, FALSE, NULL recognized correctly\n";
}

void test_join_keywords() {
    auto tokens = tokenize("JOIN INNER LEFT RIGHT ON");

    assert(tokens[0].type == TokenType::JOIN);
    assert(tokens[1].type == TokenType::INNER);
    assert(tokens[2].type == TokenType::LEFT);
    assert(tokens[3].type == TokenType::RIGHT);
    assert(tokens[4].type == TokenType::ON);

    std::cout << "[PASS] JOIN keywords recognized correctly\n";
}

void test_order_limit_keywords() {
    auto tokens = tokenize("ORDER BY ASC DESC LIMIT");

    assert(tokens[0].type == TokenType::ORDER);
    assert(tokens[1].type == TokenType::BY);
    assert(tokens[2].type == TokenType::ASC);
    assert(tokens[3].type == TokenType::DESC);
    assert(tokens[4].type == TokenType::LIMIT);

    std::cout << "[PASS] ORDER, BY, ASC, DESC, LIMIT recognized correctly\n";
}

void test_database_keywords() {
    auto tokens = tokenize("USE SHOW DATABASES DATABASE TABLES");

    assert(tokens[0].type == TokenType::USE);
    assert(tokens[1].type == TokenType::SHOW);
    assert(tokens[2].type == TokenType::DATABASES);
    assert(tokens[3].type == TokenType::DATABASE);
    assert(tokens[4].type == TokenType::TABLES);

    std::cout << "[PASS] database management keywords recognized correctly\n";
}

// ─────────────────────────────────────────────
// Identifiers
// ─────────────────────────────────────────────

void test_identifier_recognized() {
    auto tokens = tokenize("users");

    assert(tokens[0].type  == TokenType::IDENTIFIER);
    assert(tokens[0].value == "users");

    std::cout << "[PASS] identifier recognized correctly\n";
}

void test_identifier_not_lowercased() {
    auto tokens = tokenize("userName TableName");

    assert(tokens[0].value == "userName");
    assert(tokens[1].value == "TableName");

    std::cout << "[PASS] identifiers preserve original case\n";
}

void test_identifier_with_underscore() {
    auto tokens = tokenize("first_name last_name");

    assert(tokens[0].type  == TokenType::IDENTIFIER);
    assert(tokens[0].value == "first_name");
    assert(tokens[1].value == "last_name");

    std::cout << "[PASS] identifiers with underscores recognized correctly\n";
}

// ─────────────────────────────────────────────
// Literals
// ─────────────────────────────────────────────

void test_integer_literal() {
    auto tokens = tokenize("42 0 999");

    assert(tokens[0].type  == TokenType::INTEGER_LITERAL);
    assert(tokens[0].value == "42");
    assert(tokens[1].type  == TokenType::INTEGER_LITERAL);
    assert(tokens[1].value == "0");
    assert(tokens[2].value == "999");

    std::cout << "[PASS] integer literals recognized correctly\n";
}

void test_float_literal() {
    auto tokens = tokenize("3.14 0.5 100.0");

    assert(tokens[0].type  == TokenType::FLOAT_LITERAL);
    assert(tokens[0].value == "3.14");
    assert(tokens[1].value == "0.5");
    assert(tokens[2].value == "100.0");

    std::cout << "[PASS] float literals recognized correctly\n";
}

void test_string_literal() {
    auto tokens = tokenize("'hello' 'world'");

    assert(tokens[0].type  == TokenType::STRING_LITERAL);
    assert(tokens[0].value == "hello");
    assert(tokens[1].value == "world");

    std::cout << "[PASS] string literals recognized and unquoted correctly\n";
}

void test_string_literal_with_spaces() {
    auto tokens = tokenize("'hello world'");

    assert(tokens[0].type  == TokenType::STRING_LITERAL);
    assert(tokens[0].value == "hello world");

    std::cout << "[PASS] string literals with spaces recognized correctly\n";
}

void test_empty_string_literal() {
    auto tokens = tokenize("''");

    assert(tokens[0].type  == TokenType::STRING_LITERAL);
    assert(tokens[0].value == "");

    std::cout << "[PASS] empty string literal recognized correctly\n";
}

void test_negative_number() {
    auto tokens = tokenize("-42");

    assert(tokens[0].type  == TokenType::MINUS);
    assert(tokens[1].type  == TokenType::INTEGER_LITERAL);
    assert(tokens[1].value == "42");

    std::cout << "[PASS] negative number tokenized as MINUS + INTEGER_LITERAL\n";
}

// ─────────────────────────────────────────────
// Symbols and operators
// ─────────────────────────────────────────────

void test_symbols() {
    auto tokens = tokenize("( ) , ; * .");

    assert(tokens[0].type == TokenType::LPAREN);
    assert(tokens[1].type == TokenType::RPAREN);
    assert(tokens[2].type == TokenType::COMMA);
    assert(tokens[3].type == TokenType::SEMICOLON);
    assert(tokens[4].type == TokenType::STAR);
    assert(tokens[5].type == TokenType::DOT);

    std::cout << "[PASS] symbols recognized correctly\n";
}

void test_comparison_operators() {
    auto tokens = tokenize("= != < > <= >=");

    assert(tokens[0].type == TokenType::EQ);
    assert(tokens[1].type == TokenType::NEQ);
    assert(tokens[2].type == TokenType::LT);
    assert(tokens[3].type == TokenType::GT);
    assert(tokens[4].type == TokenType::LTE);
    assert(tokens[5].type == TokenType::GTE);

    std::cout << "[PASS] comparison operators recognized correctly\n";
}

// ─────────────────────────────────────────────
// Whitespace and comments
// ─────────────────────────────────────────────

void test_whitespace_skipped() {
    auto tokens = tokenize("SELECT    name   FROM    users");

    assert(tokens.size() == 4);
    assert(tokens[0].type == TokenType::SELECT);
    assert(tokens[1].type == TokenType::IDENTIFIER);
    assert(tokens[2].type == TokenType::FROM);
    assert(tokens[3].type == TokenType::IDENTIFIER);

    std::cout << "[PASS] extra whitespace is skipped\n";
}

void test_comment_skipped() {
    auto tokens = tokenize("SELECT name -- this is a comment\nFROM users");

    assert(tokens.size() == 4);
    assert(tokens[0].type == TokenType::SELECT);
    assert(tokens[1].type == TokenType::IDENTIFIER);
    assert(tokens[2].type == TokenType::FROM);
    assert(tokens[3].type == TokenType::IDENTIFIER);

    std::cout << "[PASS] single-line comments are skipped\n";
}

void test_newlines_skipped() {
    auto tokens = tokenize("SELECT\nname\nFROM\nusers");

    assert(tokens.size() == 4);

    std::cout << "[PASS] newlines treated as whitespace\n";
}

// ─────────────────────────────────────────────
// Full SQL statements
// ─────────────────────────────────────────────

void test_select_statement() {
    auto tokens = tokenize("SELECT * FROM users WHERE age > 25;");

    expect(tokens, 0, TokenType::SELECT);
    expect(tokens, 1, TokenType::STAR);
    expect(tokens, 2, TokenType::FROM);
    expect(tokens, 3, TokenType::IDENTIFIER, "users");
    expect(tokens, 4, TokenType::WHERE);
    expect(tokens, 5, TokenType::IDENTIFIER, "age");
    expect(tokens, 6, TokenType::GT);
    expect(tokens, 7, TokenType::INTEGER_LITERAL, "25");
    expect(tokens, 8, TokenType::SEMICOLON);

    std::cout << "[PASS] SELECT statement tokenized correctly\n";
}

void test_insert_statement() {
    auto tokens = tokenize("INSERT INTO users (name, age) VALUES ('Alice', 25);");

    expect(tokens, 0, TokenType::INSERT);
    expect(tokens, 1, TokenType::INTO);
    expect(tokens, 2, TokenType::IDENTIFIER, "users");
    expect(tokens, 3, TokenType::LPAREN);
    expect(tokens, 4, TokenType::IDENTIFIER, "name");
    expect(tokens, 5, TokenType::COMMA);
    expect(tokens, 6, TokenType::IDENTIFIER, "age");
    expect(tokens, 7, TokenType::RPAREN);
    expect(tokens, 8, TokenType::VALUES);
    expect(tokens, 9, TokenType::LPAREN);
    expect(tokens, 10, TokenType::STRING_LITERAL, "Alice");
    expect(tokens, 11, TokenType::COMMA);
    expect(tokens, 12, TokenType::INTEGER_LITERAL, "25");
    expect(tokens, 13, TokenType::RPAREN);
    expect(tokens, 14, TokenType::SEMICOLON);

    std::cout << "[PASS] INSERT statement tokenized correctly\n";
}

void test_create_table_statement() {
    auto tokens = tokenize(
        "CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(50));");

    expect(tokens, 0,  TokenType::CREATE);
    expect(tokens, 1,  TokenType::TABLE);
    expect(tokens, 2,  TokenType::IDENTIFIER, "users");
    expect(tokens, 3,  TokenType::LPAREN);
    expect(tokens, 4,  TokenType::IDENTIFIER, "id");
    expect(tokens, 5,  TokenType::INT_KW);
    expect(tokens, 6,  TokenType::PRIMARY);
    expect(tokens, 7,  TokenType::KEY);
    expect(tokens, 8,  TokenType::AUTO_INCREMENT);
    expect(tokens, 9,  TokenType::COMMA);
    expect(tokens, 10, TokenType::IDENTIFIER, "name");
    expect(tokens, 11, TokenType::VARCHAR_KW);
    expect(tokens, 12, TokenType::LPAREN);
    expect(tokens, 13, TokenType::INTEGER_LITERAL, "50");
    expect(tokens, 14, TokenType::RPAREN);
    expect(tokens, 15, TokenType::RPAREN);
    expect(tokens, 16, TokenType::SEMICOLON);

    std::cout << "[PASS] CREATE TABLE statement tokenized correctly\n";
}

void test_update_statement() {
    auto tokens = tokenize("UPDATE users SET name = 'Bob' WHERE id = 1;");

    expect(tokens, 0, TokenType::UPDATE);
    expect(tokens, 1, TokenType::IDENTIFIER, "users");
    expect(tokens, 2, TokenType::SET);
    expect(tokens, 3, TokenType::IDENTIFIER, "name");
    expect(tokens, 4, TokenType::EQ);
    expect(tokens, 5, TokenType::STRING_LITERAL, "Bob");
    expect(tokens, 6, TokenType::WHERE);
    expect(tokens, 7, TokenType::IDENTIFIER, "id");
    expect(tokens, 8, TokenType::EQ);
    expect(tokens, 9, TokenType::INTEGER_LITERAL, "1");
    expect(tokens, 10, TokenType::SEMICOLON);

    std::cout << "[PASS] UPDATE statement tokenized correctly\n";
}

void test_delete_statement() {
    auto tokens = tokenize("DELETE FROM users WHERE id = 1;");

    expect(tokens, 0, TokenType::DELETE);
    expect(tokens, 1, TokenType::FROM);
    expect(tokens, 2, TokenType::IDENTIFIER, "users");
    expect(tokens, 3, TokenType::WHERE);
    expect(tokens, 4, TokenType::IDENTIFIER, "id");
    expect(tokens, 5, TokenType::EQ);
    expect(tokens, 6, TokenType::INTEGER_LITERAL, "1");
    expect(tokens, 7, TokenType::SEMICOLON);

    std::cout << "[PASS] DELETE statement tokenized correctly\n";
}

// ─────────────────────────────────────────────
// Error cases
// ─────────────────────────────────────────────

void test_unterminated_string_throws() {
    bool threw = false;
    try {
        Tokenizer t("SELECT 'unterminated");
        t.tokenize();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);

    std::cout << "[PASS] unterminated string literal throws\n";
}

void test_unknown_character_throws() {
    bool threw = false;
    try {
        Tokenizer t("SELECT @ FROM users");
        t.tokenize();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);

    std::cout << "[PASS] unknown character throws\n";
}

void test_end_of_file_token() {
    Tokenizer t("SELECT");
    auto tokens = t.tokenize();
    assert(tokens.back().type == TokenType::END_OF_FILE);

    std::cout << "[PASS] tokenizer always ends with END_OF_FILE token\n";
}

int main() {
    std::cout << "\n=== Keyword Tests ===\n";
    test_keywords_recognized();
    test_keywords_case_insensitive();
    test_type_keywords();
    test_boolean_keywords();
    test_join_keywords();
    test_order_limit_keywords();
    test_database_keywords();

    std::cout << "\n=== Identifier Tests ===\n";
    test_identifier_recognized();
    test_identifier_not_lowercased();
    test_identifier_with_underscore();

    std::cout << "\n=== Literal Tests ===\n";
    test_integer_literal();
    test_float_literal();
    test_string_literal();
    test_string_literal_with_spaces();
    test_empty_string_literal();
    test_negative_number();

    std::cout << "\n=== Symbol Tests ===\n";
    test_symbols();
    test_comparison_operators();

    std::cout << "\n=== Whitespace and Comment Tests ===\n";
    test_whitespace_skipped();
    test_comment_skipped();
    test_newlines_skipped();

    std::cout << "\n=== Full Statement Tests ===\n";
    test_select_statement();
    test_insert_statement();
    test_create_table_statement();
    test_update_statement();
    test_delete_statement();

    std::cout << "\n=== Error Tests ===\n";
    test_unterminated_string_throws();
    test_unknown_character_throws();
    test_end_of_file_token();

    std::cout << "\nAll tokenizer tests passed.\n";
    return 0;
}