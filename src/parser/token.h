#pragma once

#include <string>

enum class TokenType {
    // ---- Keywords ----
    SELECT, FROM, WHERE, INSERT, INTO, VALUES,
    UPDATE, SET, DELETE, CREATE, TABLE, DROP,
    USE, SHOW, DATABASES, DATABASE, TABLES,
    AND, OR, NOT, NULL_KW,               // NULL_KW to avoid clash with NULL macro
    IS, LIKE, ORDER, BY, ASC, DESC,
    LIMIT, JOIN, INNER, LEFT, RIGHT, ON,
    PRIMARY, KEY, AUTO_INCREMENT, CHECK,
    INT_KW, FLOAT_KW, BOOLEAN_KW, VARCHAR_KW,  // type keywords
    TRUE_KW, FALSE_KW,                           // boolean literals as keywords

    // ---- Literals ----
    INTEGER_LITERAL,   // e.g. 42
    FLOAT_LITERAL,     // e.g. 3.14
    STRING_LITERAL,    // e.g. 'hello'

    // ---- Identifiers ----
    IDENTIFIER,        // table names, column names

    // ---- Symbols ----
    LPAREN,    // (
    RPAREN,    // )
    COMMA,     // ,
    SEMICOLON, // ;
    STAR,      // *
    DOT,       // .
    EQ,        // =
    NEQ,       // !=
    LT,        // <
    GT,        // >
    LTE,       // <=
    GTE,       // >=
    PLUS,      // +
    MINUS,     // -

    // ---- Special ----
    END_OF_FILE,
};

struct Token {
    TokenType   type;
    std::string value;   // raw text of the token
    size_t      line;    // for error reporting
    size_t      col;

    Token(TokenType type, std::string value, size_t line, size_t col)
        : type(type), value(std::move(value)), line(line), col(col) {}
};

// returns a human readable name for a token type — used in error messages
std::string token_type_name(TokenType type);