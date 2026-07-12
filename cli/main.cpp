// cli/main.cpp — GS-DBEngine interactive REPL

// Compile (from project root, until CMakeLists.txt is written):
//   g++ -std=c++17 cli/main.cpp src/database.cpp src/executor/executor.cpp \
//       src/parser/parser.cpp src/parser/tokenizer.cpp src/table/table.cpp \
//       src/row/serializer.cpp src/btree/btree.cpp src/btree/btree_node.cpp \
//       src/btree/free_list_manager.cpp src/btree/key.cpp src/index/index.cpp \
//       src/catalog/catalog_manager.cpp src/storage/disk_manager.cpp \
//       src/header/header_manager.cpp src/storage/buffer_pool.cpp \
//       src/wal/wal_manager.cpp -Isrc -o tests/gsdb_temp && ./tests/gsdb_temp

// Usage:
//   ./tests/gsdb              — interactive REPL
//   ./tests/gsdb < file.sql   — pipe a SQL script

#include <algorithm>
#include <iostream>
#include <numeric>
#include <string>
#include <unistd.h>
#include <vector>

#include "database.h"

// ─────────────────────────────────────────────────────────────────────────────
// Table formatter
// ─────────────────────────────────────────────────────────────────────────────

// Prints a QueryResult as a fixed-width ASCII table, e.g.:
//   +----+-------+-----+
//   | id | name  | age |
//   +----+-------+-----+
//   |  1 | Alice |  25 |
//   +----+-------+-----+
//   1 row(s)
static void print_table(const QueryResult& r)
{
    if (r.columns.empty() && r.rows.empty()) {
        // DDL / DML with no result set — just print the affected count
        if (r.rows_affected > 0) {
            std::cout << "OK (" << r.rows_affected << " row(s) affected)\n";
        } else {
            std::cout << "OK\n";
        }
        return;
    }

    const size_t ncols = r.columns.size();

    // Compute maximum width for each column (header vs every data cell)
    std::vector<size_t> widths(ncols);
    for (size_t c = 0; c < ncols; ++c) {
        widths[c] = r.columns[c].size();
    }
    for (const auto& row : r.rows) {
        for (size_t c = 0; c < ncols && c < row.size(); ++c) {
            widths[c] = std::max(widths[c], row[c].size());
        }
    }

    // Separator line: +----+-------+-----+
    auto print_separator = [&]() {
        std::cout << '+';
        for (size_t c = 0; c < ncols; ++c) {
            std::cout << std::string(widths[c] + 2, '-') << '+';
        }
        std::cout << '\n';
    };

    // Row line: | val | val | val |  (left-aligned, space-padded)
    auto print_row = [&](const std::vector<std::string>& cells) {
        std::cout << '|';
        for (size_t c = 0; c < ncols; ++c) {
            const std::string& cell = (c < cells.size()) ? cells[c] : "";
            std::cout << ' ' << cell
                      << std::string(widths[c] - cell.size(), ' ')
                      << " |";
        }
        std::cout << '\n';
    };

    print_separator();
    print_row(r.columns);
    print_separator();
    for (const auto& row : r.rows) {
        print_row(row);
    }
    print_separator();

    std::cout << r.rows.size() << " row(s)\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// REPL helpers
// ─────────────────────────────────────────────────────────────────────────────

// Returns true if 'input' contains a ';' (ignoring content inside
// single-quoted strings so that 'hello; world' doesn't count).
static bool has_terminator(const std::string& input)
{
    bool in_string = false;
    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        if (!in_string && c == '-' && i + 1 < input.size() && input[i + 1] == '-') {
            // skip to end of line — a '-- comment' isn't SQL, so quotes
            // and semicolons inside it must not affect statement splitting
            while (i < input.size() && input[i] != '\n') ++i;
            continue;
        }
        if (c == '\'' && !in_string) {
            in_string = true;
        } else if (c == '\'' && in_string) {
            // Handle escaped single-quote: ''
            if (i + 1 < input.size() && input[i + 1] == '\'') {
                ++i;  // skip the second quote
            } else {
                in_string = false;
            }
        } else if (c == ';' && !in_string) {
            return true;
        }
    }
    return false;
}

// Returns true if 'line' is a quit command (case-insensitive).
static bool is_quit(const std::string& line)
{
    std::string trimmed = line;
    // Strip leading/trailing whitespace
    size_t s = trimmed.find_first_not_of(" \t\r\n");
    size_t e = trimmed.find_last_not_of(" \t\r\n;");
    if (s == std::string::npos) return false;
    trimmed = trimmed.substr(s, e - s + 1);

    std::string lower = trimmed;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return (lower == "exit" || lower == "quit" || lower == "\\q");
}

// Handles the "\path" meta-command, if 'line' is one.
//   \path             — print the current data directory
//   \path <dir>       — change the data directory to <dir> (persists across
//                        sessions; closes any active database first)
//   \path default     — reset to the default data directory (also persists)
// Returns true if 'line' was a \path command (handled, whether it succeeded
// or errored) — false if 'line' isn't a \path command at all, so the caller
// should fall through to normal SQL buffering.
static bool try_handle_path_command(const std::string& line, Database& db)
{
    std::string trimmed = line;
    size_t s = trimmed.find_first_not_of(" \t\r\n");
    if (s == std::string::npos) return false;
    trimmed = trimmed.substr(s);

    if (trimmed.rfind("\\path", 0) != 0) return false;  // doesn't start with \path

    std::string rest = trimmed.substr(5);
    size_t rs = rest.find_first_not_of(" \t\r\n");

    if (rs == std::string::npos) {
        // No argument — just show the current data directory.
        std::cout << "Current data directory: " << db.data_dir().string() << "\n";
        return true;
    }

    rest = rest.substr(rs);
    size_t re = rest.find_last_not_of(" \t\r\n");
    rest = rest.substr(0, re + 1);

    std::string lower = rest;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    std::string previous_db = db.current_database();

    try {
        if (lower == "default") {
            db.reset_data_dir();
            std::cout << "Data directory reset to default: " << db.data_dir().string() << "\n";
        } else {
            db.set_data_dir(rest);
            std::cout << "Data directory changed to: " << db.data_dir().string() << "\n";
        }
        if (!previous_db.empty()) {
            std::cout << "(previously active database '" << previous_db << "' was closed — "
                       << "run USE again if you need it)\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────

int main()
{
    Database db;  // uses ~/Documents/GS-DBEngine/ as data_dir

    const bool interactive = isatty(fileno(stdin));

    if (interactive) {
        std::cout << "GS-DBEngine v0.1\n"
                  << "Type SQL statements terminated with ';'.\n"
                  << "Type 'exit' or 'quit' to leave.\n"
                  << "Type '\\path' to see/change the data directory.\n"
                  << "Data directory: " << db.data_dir().string() << "\n\n";
    }

    std::string buffer;  // accumulated SQL (may span multiple lines)

    while (true) {
        // Print prompt only in interactive mode
        if (interactive) {
            if (buffer.empty()) {
                std::cout << "gsdb> " << std::flush;
            } else {
                std::cout << "   -> " << std::flush;
            }
        }

        std::string line;
        if (!std::getline(std::cin, line)) {
            // EOF (Ctrl+D or end of piped file)
            if (!buffer.empty()) {
                // Execute any trailing SQL that has no semicolon
                QueryResult r = db.execute(buffer);
                if (!r.success) {
                    std::cerr << "ERROR: " << r.error_message << '\n';
                }
            }
            if (interactive) std::cout << "\nBye.\n";
            break;
        }

        // Check for quit before adding to buffer
        if (buffer.empty() && is_quit(line)) {
            if (interactive) std::cout << "Bye.\n";
            break;
        }

        // \path is a REPL meta-command, not SQL — handle it directly and
        // skip SQL buffering entirely. Only recognized when not already
        // mid-statement (buffer.empty()), same as is_quit() above.
        if (buffer.empty() && try_handle_path_command(line, db)) {
            if (interactive) std::cout << '\n';
            continue;
        }

        // Accumulate the line (preserve newlines for multi-line SQL)
        if (!buffer.empty()) buffer += '\n';
        buffer += line;

        // Only execute once we see a semicolon
        if (!has_terminator(buffer)) {
            continue;
        }

        // Execute the complete statement
        QueryResult result = db.execute(buffer);
        buffer.clear();

        if (!result.success) {
            std::cerr << "ERROR: " << result.error_message << '\n';
        } else {
            print_table(result);
        }

        if (interactive) std::cout << '\n';
    }

    return 0;
}