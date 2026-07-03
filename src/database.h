#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "../storage/disk_manager.h"
#include "../storage/header_manager.h"
#include "../storage/buffer_pool.h"
#include "../wal/wal_manager.h"
#include "../catalog/catalog_manager.h"
#include "../executor/executor.h"
#include "../parser/parser.h"
#include "../parser/tokenizer.h"

// ─────────────────────────────────────────────────────────────────────────────
// Database
//
// Top-level orchestrator. Sits above Executor in the layer hierarchy.
//
// Responsibilities:
//   - Intercepts database-level statements (CREATE/DROP DATABASE, USE, SHOW DATABASES)
//   - Routes all table-level statements to the active Executor
//   - Manages the full storage stack lifecycle on USE (open / close)
//   - Owns the data directory layout on disk
//
// File layout:
//   ~/Documents/GS-DBEngine/       ← DATA_DIR (default, fixed)
//     mydb/
//       mydb.db
//       mydb.wal
//     otherdb/
//       otherdb.db
//       otherdb.wal
//
// Design decisions (see docs/DECISIONS.md):
//   - CREATE DATABASE does NOT auto-USE; explicit USE required
//   - Default data_dir = ~/Documents/GS-DBEngine/ — fixed constant for now
//   - No active database → non-DB statements return "No database selected"
// ─────────────────────────────────────────────────────────────────────────────

class Database {
public:
    // Constructs the orchestrator.
    // data_dir defaults to ~/Documents/GS-DBEngine/.
    // Passing an override is useful for tests (temp directory).
    // Ensures data_dir exists on construction.
    explicit Database(std::filesystem::path data_dir = default_data_dir());

    // Primary entry point for the CLI.
    // Tokenizes and parses sql, then dispatches to execute(Statement).
    QueryResult execute(const std::string& sql);

    // Dispatches a pre-parsed statement.
    // Database-level statements are handled here.
    // All other statements are forwarded to the active Executor.
    QueryResult execute(const Statement& stmt);

    // Returns the name of the currently active database, or "" if none.
    const std::string& current_database() const { return current_db_name_; }

    // Returns ~/Documents/GS-DBEngine/ resolved via $HOME.
    // Used as the default data_dir argument.
    static std::filesystem::path default_data_dir();

private:
    std::filesystem::path data_dir_;
    std::string           current_db_name_;  // empty = no database selected

    // Storage stack for the currently active database.
    // All pointers are null when no database is selected.
    std::unique_ptr<DiskManager>    disk_manager_;
    std::unique_ptr<HeaderManager>  header_manager_;
    std::unique_ptr<BufferPool>     buffer_pool_;
    std::unique_ptr<WALManager>     wal_manager_;
    std::unique_ptr<CatalogManager> catalog_manager_;
    std::unique_ptr<Executor>       executor_;

    // ── Database-level statement handlers ────────────────────────────────────

    QueryResult handle_create_database(const CreateDatabaseStmt& stmt);
    QueryResult handle_drop_database(const DropDatabaseStmt& stmt);
    QueryResult handle_use(const UseStmt& stmt);
    QueryResult handle_show_databases();

    // ── Storage stack lifecycle ───────────────────────────────────────────────

    // Bootstraps the storage stack for an existing database:
    //   recover WAL → load catalog → construct Executor
    void open_database(const std::string& name);

    // Flushes the buffer pool and destroys all stack members.
    // No-op if no database is currently selected.
    void close_database();

    // ── Path helpers ──────────────────────────────────────────────────────────

    // data_dir_ / name /
    std::filesystem::path db_dir(const std::string& name)  const;
    // data_dir_ / name / name.db
    std::filesystem::path db_file(const std::string& name) const;
    // data_dir_ / name / name.wal
    std::filesystem::path wal_file(const std::string& name) const;
};
