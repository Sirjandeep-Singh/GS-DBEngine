#pragma once

#include <string>
#include <vector>
#include "token.h"

// Tokenizer splits a SQL string into a flat list of tokens.
// It is the first stage of the parser pipeline.
//
// Input:  "SELECT name FROM users WHERE age > 25;"
// Output: [SELECT] [IDENTIFIER:name] [FROM] [IDENTIFIER:users]
//         [WHERE] [IDENTIFIER:age] [GT] [INTEGER_LITERAL:25] [SEMICOLON] [END_OF_FILE]
//
// Rules:
//   - Keywords are case-insensitive (SELECT = select = Select)
//   - Identifiers are case-sensitive (tableName != tablename)
//   - String literals are delimited by single quotes: 'hello'
//   - Single-line comments start with -- and are skipped
//   - Whitespace is skipped
//   - Unknown characters throw a std::runtime_error

class Tokenizer {
public:
    explicit Tokenizer(const std::string& input);

    // tokenizes the entire input and returns all tokens
    // always ends with an END_OF_FILE token
    // throws std::runtime_error on unrecognized characters or unterminated strings
    std::vector<Token> tokenize();

private:
    const std::string& input_;
    size_t             pos_;
    size_t             line_;
    size_t             col_;

    // returns current character without advancing
    char peek() const;

    // returns current character and advances pos_
    char advance();

    // returns true if pos_ is at or past the end of input
    bool at_end() const;

    // skips whitespace and -- comments
    void skip_whitespace_and_comments();

    // reads and returns the next token
    Token next_token();

    // reads a string literal (after opening quote has been seen)
    Token read_string_literal();

    // reads a numeric literal (integer or float)
    Token read_number();

    // reads an identifier or keyword
    Token read_identifier_or_keyword();

    // reads a two-character operator if the next char matches expected,
    // otherwise returns a single-character token
    Token read_operator(char first);

    // maps an identifier string to its keyword token type,
    // or IDENTIFIER if it is not a keyword
    static TokenType keyword_or_identifier(const std::string& word);
};