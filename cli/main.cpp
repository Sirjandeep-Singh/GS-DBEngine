// cli/main.cpp — GS-DBEngine interactive REPL
//
// Compile (from project root, until CMakeLists.txt is written):
//   g++ -std=c++17 cli/main.cpp src/database.cpp src/executor/executor.cpp \
//       src/parser/parser.cpp src/parser/tokenizer.cpp src/table/table.cpp \
//       src/row/serializer.cpp src/btree/btree.cpp src/btree/btree_node.cpp \
//       src/catalog/catalog_manager.cpp src/storage/disk_manager.cpp \
//       src/storage/header_manager.cpp src/storage/buffer_pool.cpp \
//       src/wal/wal_manager.cpp -Isrc -o gsdb && ./gsdb
//
// Usage:
//   ./gsdb              — interactive REPL
//   ./gsdb < file.sql   — pipe a SQL script

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
                  << "Type 'exit' or 'quit' to leave.\n\n";
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
