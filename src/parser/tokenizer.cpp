#include "tokenizer.h"

#include <stdexcept>
#include <algorithm>
#include <cctype>

Tokenizer::Tokenizer(const std::string& input)
    : input_(input), pos_(0), line_(1), col_(1)
{}

bool Tokenizer::at_end() const {
    return pos_ >= input_.size();
}

char Tokenizer::peek() const {
    if (at_end()) return '\0';
    return input_[pos_];
}

char Tokenizer::advance() {
    char c = input_[pos_++];
    if (c == '\n') { line_++; col_ = 1; }
    else           { col_++; }
    return c;
}

void Tokenizer::skip_whitespace_and_comments() {
    while (!at_end()) {
        char c = peek();

        if (std::isspace(static_cast<unsigned char>(c))) {
            advance();
            continue;
        }

        // single-line comment: -- until end of line
        if (c == '-' && pos_ + 1 < input_.size() && input_[pos_ + 1] == '-') {
            while (!at_end() && peek() != '\n') advance();
            continue;
        }

        break;
    }
}

Token Tokenizer::read_string_literal() {
    size_t start_line = line_;
    size_t start_col  = col_;
    std::string value;
    while (!at_end()) {
        char c = advance();
        if (c == '\'') {
            // handle escaped single quote: ''
            if (!at_end() && peek() == '\'') {
                advance();
                value += '\'';
            } else {
                return Token(TokenType::STRING_LITERAL, value, start_line, start_col);
            }
        } else {
            value += c;
        }
    }

    throw std::runtime_error(
        "Tokenizer: unterminated string literal at line "
        + std::to_string(start_line) + " col " + std::to_string(start_col));
}

Token Tokenizer::read_number() {
    size_t start_line = line_;
    size_t start_col  = col_;
    std::string value;
    bool is_float = false;

    while (!at_end() && (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '.')) {
        if (peek() == '.') {
            //if a second . appears why not throw an error? because it will be tokenized as a float and then the second . will be tokenized as a dot. so we can just break here and let the next token be a dot
            if (is_float) break;  // second dot — stop here
            is_float = true;
        }
        value += advance();
    }

    TokenType type = is_float ? TokenType::FLOAT_LITERAL : TokenType::INTEGER_LITERAL;
    return Token(type, value, start_line, start_col);
}

Token Tokenizer::read_identifier_or_keyword() {
    size_t start_line = line_;
    size_t start_col  = col_;
    std::string value;

    while (!at_end() && (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_')) {
        value += advance();
    }

    TokenType type = keyword_or_identifier(value);
    return Token(type, value, start_line, start_col);
}

Token Tokenizer::read_operator(char first) {
    size_t start_line = line_;
    size_t start_col  = col_ - 1;  // col_ already advanced past `first`

    if (first == '!' && !at_end() && peek() == '=') {
        advance();
        return Token(TokenType::NEQ, "!=", start_line, start_col);
    }
    if (first == '<' && !at_end() && peek() == '=') {
        advance();
        return Token(TokenType::LTE, "<=", start_line, start_col);
    }
    if (first == '>' && !at_end() && peek() == '=') {
        advance();
        return Token(TokenType::GTE, ">=", start_line, start_col);
    }

    // single character operators
    switch (first) {
        case '=': return Token(TokenType::EQ,        "=",  start_line, start_col);
        case '<': return Token(TokenType::LT,        "<",  start_line, start_col);
        case '>': return Token(TokenType::GT,        ">",  start_line, start_col);
        case '+': return Token(TokenType::PLUS,      "+",  start_line, start_col);
        case '-': return Token(TokenType::MINUS,     "-",  start_line, start_col);
        case '(': return Token(TokenType::LPAREN,    "(",  start_line, start_col);
        case ')': return Token(TokenType::RPAREN,    ")",  start_line, start_col);
        case ',': return Token(TokenType::COMMA,     ",",  start_line, start_col);
        case ';': return Token(TokenType::SEMICOLON, ";",  start_line, start_col);
        case '*': return Token(TokenType::STAR,      "*",  start_line, start_col);
        case '.': return Token(TokenType::DOT,       ".",  start_line, start_col);
        default:
            throw std::runtime_error(
                "Tokenizer: unknown character '" + std::string(1, first)
                + "' at line " + std::to_string(start_line)
                + " col " + std::to_string(start_col));
    }
}

Token Tokenizer::next_token() {
    skip_whitespace_and_comments();

    if (at_end()) {
        return Token(TokenType::END_OF_FILE, "", line_, col_);
    }

    size_t start_line = line_;
    size_t start_col  = col_;
    char c = advance();

    if (c == '\'') return read_string_literal();
    if (std::isdigit(static_cast<unsigned char>(c))) {
        // put back and re-read as number
        //what is pos and how does it differ from line and col? pos is the index in the input string, line and col are for error reporting. so we need to decrement pos and col to re-read the number correctly
        pos_--;
        col_--;
        return read_number();
    }
    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
        pos_--;
        col_--;
        return read_identifier_or_keyword();
    }

    return read_operator(c);
}

std::vector<Token> Tokenizer::tokenize() {
    std::vector<Token> tokens;
    pos_  = 0;
    line_ = 1;
    col_  = 1;

    while (true) {
        Token t = next_token();
        tokens.push_back(t);
        if (t.type == TokenType::END_OF_FILE) break;
    }

    return tokens;
}

// ─────────────────────────────────────────────
// Keyword map
// ─────────────────────────────────────────────

TokenType Tokenizer::keyword_or_identifier(const std::string& word) {
    // uppercase the word for case-insensitive comparison
    std::string upper = word;
    std::transform(upper.begin(), upper.end(), upper.begin(),
        [](unsigned char c) { return std::toupper(c); });

    if (upper == "SELECT")         return TokenType::SELECT;
    if (upper == "FROM")           return TokenType::FROM;
    if (upper == "WHERE")          return TokenType::WHERE;
    if (upper == "INSERT")         return TokenType::INSERT;
    if (upper == "INTO")           return TokenType::INTO;
    if (upper == "VALUES")         return TokenType::VALUES;
    if (upper == "UPDATE")         return TokenType::UPDATE;
    if (upper == "SET")            return TokenType::SET;
    if (upper == "DELETE")         return TokenType::DELETE;
    if (upper == "CREATE")         return TokenType::CREATE;
    if (upper == "TABLE")          return TokenType::TABLE;
    if (upper == "DROP")           return TokenType::DROP;
    if (upper == "USE")            return TokenType::USE;
    if (upper == "SHOW")           return TokenType::SHOW;
    if (upper == "DATABASES")      return TokenType::DATABASES;
    if (upper == "DATABASE")       return TokenType::DATABASE;
    if (upper == "TABLES")         return TokenType::TABLES;
    if (upper == "INDEX")          return TokenType::INDEX;
    if (upper == "UNIQUE")         return TokenType::UNIQUE;
    if (upper == "AND")            return TokenType::AND;
    if (upper == "OR")             return TokenType::OR;
    if (upper == "NOT")            return TokenType::NOT;
    if (upper == "NULL")           return TokenType::NULL_KW;
    if (upper == "IS")             return TokenType::IS;
    if (upper == "LIKE")           return TokenType::LIKE;
    if (upper == "ORDER")          return TokenType::ORDER;
    if (upper == "BY")             return TokenType::BY;
    if (upper == "ASC")            return TokenType::ASC;
    if (upper == "DESC")           return TokenType::DESC;
    if (upper == "LIMIT")          return TokenType::LIMIT;
    if (upper == "JOIN")           return TokenType::JOIN;
    if (upper == "INNER")          return TokenType::INNER;
    if (upper == "LEFT")           return TokenType::LEFT;
    if (upper == "RIGHT")          return TokenType::RIGHT;
    if (upper == "ON")             return TokenType::ON;
    if (upper == "PRIMARY")        return TokenType::PRIMARY;
    if (upper == "KEY")            return TokenType::KEY;
    if (upper == "AUTO_INCREMENT") return TokenType::AUTO_INCREMENT;
    if (upper == "CHECK")          return TokenType::CHECK;
    if (upper == "COUNT")          return TokenType::COUNT;
    if (upper == "IN")             return TokenType::IN;
    if (upper == "EXISTS")         return TokenType::EXISTS;
    if (upper == "FOREIGN")        return TokenType::FOREIGN;
    if (upper == "REFERENCES")     return TokenType::REFERENCES;
    if (upper == "CASCADE")        return TokenType::CASCADE;
    if (upper == "RESTRICT")       return TokenType::RESTRICT;
    if (upper == "INT")            return TokenType::INT_KW;
    if (upper == "FLOAT")          return TokenType::FLOAT_KW;
    if (upper == "BOOLEAN")        return TokenType::BOOLEAN_KW;
    if (upper == "VARCHAR")        return TokenType::VARCHAR_KW;
    if (upper == "TRUE")           return TokenType::TRUE_KW;
    if (upper == "FALSE")          return TokenType::FALSE_KW;

    return TokenType::IDENTIFIER;
}

std::string token_type_name(TokenType type) {
    switch (type) {
        case TokenType::SELECT:          return "SELECT";
        case TokenType::FROM:            return "FROM";
        case TokenType::WHERE:           return "WHERE";
        case TokenType::INSERT:          return "INSERT";
        case TokenType::INTO:            return "INTO";
        case TokenType::VALUES:          return "VALUES";
        case TokenType::UPDATE:          return "UPDATE";
        case TokenType::SET:             return "SET";
        case TokenType::DELETE:          return "DELETE";
        case TokenType::CREATE:          return "CREATE";
        case TokenType::TABLE:           return "TABLE";
        case TokenType::DROP:            return "DROP";
        case TokenType::USE:             return "USE";
        case TokenType::SHOW:            return "SHOW";
        case TokenType::DATABASES:       return "DATABASES";
        case TokenType::DATABASE:        return "DATABASE";
        case TokenType::TABLES:          return "TABLES";
        case TokenType::INDEX:           return "INDEX";
        case TokenType::UNIQUE:          return "UNIQUE";
        case TokenType::AND:             return "AND";
        case TokenType::OR:              return "OR";
        case TokenType::NOT:             return "NOT";
        case TokenType::NULL_KW:         return "NULL";
        case TokenType::IS:              return "IS";
        case TokenType::LIKE:            return "LIKE";
        case TokenType::ORDER:           return "ORDER";
        case TokenType::BY:              return "BY";
        case TokenType::ASC:             return "ASC";
        case TokenType::DESC:            return "DESC";
        case TokenType::LIMIT:           return "LIMIT";
        case TokenType::JOIN:            return "JOIN";
        case TokenType::INNER:           return "INNER";
        case TokenType::LEFT:            return "LEFT";
        case TokenType::RIGHT:           return "RIGHT";
        case TokenType::ON:              return "ON";
        case TokenType::PRIMARY:         return "PRIMARY";
        case TokenType::KEY:             return "KEY";
        case TokenType::AUTO_INCREMENT:  return "AUTO_INCREMENT";
        case TokenType::CHECK:           return "CHECK";
        case TokenType::COUNT:           return "COUNT";
        case TokenType::IN:              return "IN";
        case TokenType::EXISTS:          return "EXISTS";
        case TokenType::FOREIGN:         return "FOREIGN";
        case TokenType::REFERENCES:      return "REFERENCES";
        case TokenType::CASCADE:         return "CASCADE";
        case TokenType::RESTRICT:        return "RESTRICT";
        case TokenType::INT_KW:          return "INT";
        case TokenType::FLOAT_KW:        return "FLOAT";
        case TokenType::BOOLEAN_KW:      return "BOOLEAN";
        case TokenType::VARCHAR_KW:      return "VARCHAR";
        case TokenType::TRUE_KW:         return "TRUE";
        case TokenType::FALSE_KW:        return "FALSE";
        case TokenType::INTEGER_LITERAL: return "INTEGER_LITERAL";
        case TokenType::FLOAT_LITERAL:   return "FLOAT_LITERAL";
        case TokenType::STRING_LITERAL:  return "STRING_LITERAL";
        case TokenType::IDENTIFIER:      return "IDENTIFIER";
        case TokenType::LPAREN:          return "(";
        case TokenType::RPAREN:          return ")";
        case TokenType::COMMA:           return ",";
        case TokenType::SEMICOLON:       return ";";
        case TokenType::STAR:            return "*";
        case TokenType::DOT:             return ".";
        case TokenType::EQ:              return "=";
        case TokenType::NEQ:             return "!=";
        case TokenType::LT:              return "<";
        case TokenType::GT:              return ">";
        case TokenType::LTE:             return "<=";
        case TokenType::GTE:             return ">=";
        case TokenType::PLUS:            return "+";
        case TokenType::MINUS:           return "-";
        case TokenType::END_OF_FILE:     return "EOF";
    }
    return "UNKNOWN";
}