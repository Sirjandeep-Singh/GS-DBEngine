#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "storage/disk_manager.h"
#include "storage/header_manager.h"
#include "storage/buffer_pool.h"
#include "wal/wal_manager.h"
#include "catalog/catalog_manager.h"
#include "executor/executor.h"
#include "parser/parser.h"
#include "parser/tokenizer.h"

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
//   - data_dir can be changed at runtime via set_data_dir() (CLI: \path <dir>);
//     this closes any active database first (see set_data_dir doc comment)
//   - data_dir changes persist across sessions via a fixed config file at
//     ~/Documents/GSDB-config.txt (NOT inside data_dir itself — it has to
//     live somewhere fixed, independent of whatever data_dir currently is)
// ─────────────────────────────────────────────────────────────────────────────

class Database {
public:
    // Constructs the orchestrator.
    // data_dir defaults to resolve_data_dir() — the persisted override from
    // GSDB-config.txt if one exists, otherwise default_data_dir().
    // Passing an explicit override (e.g. in tests) bypasses persistence entirely.
    // Ensures data_dir exists on construction.
    explicit Database(std::filesystem::path data_dir = resolve_data_dir());

    // Calls close_database(): flushes buffer pool and checkpoints the active DB.
    ~Database();

    // Primary entry point for the CLI.
    // Tokenizes and parses sql, then dispatches to execute(Statement).
    QueryResult execute(const std::string& sql);

    // Dispatches a pre-parsed statement.
    // Database-level statements are handled here.
    // All other statements are forwarded to the active Executor.
    QueryResult execute(const Statement& stmt);

    // Returns the name of the currently active database, or "" if none.
    const std::string& current_database() const { return current_db_name_; }

    // Returns the directory where all databases are currently stored.
    const std::filesystem::path& data_dir() const { return data_dir_; }

    // Changes the directory where databases are stored.
    //
    // Closes any currently active database first (same as normal shutdown:
    // flushes buffer pool, checkpoints WAL) since the storage stack for the
    // old data_dir is no longer meaningful once the path changes. The caller
    // must USE a database again afterward — this mirrors close_database()'s
    // existing behavior elsewhere (e.g. DROP DATABASE on the active database).
    //
    // Creates new_dir if it doesn't already exist. Throws std::filesystem
    // errors on failure (e.g. permission denied) — the data_dir is left
    // unchanged in that case.
    //
    // Persists new_dir to GSDB-config.txt so it survives future sessions.
    void set_data_dir(std::filesystem::path new_dir);

    // Resets data_dir back to default_data_dir() (~/Documents/GS-DBEngine/)
    // and removes GSDB-config.txt, so future sessions don't read a stale
    // override. Same closing behavior as set_data_dir().
    void reset_data_dir();

    // Returns ~/Documents/GS-DBEngine/ resolved via $HOME.
    // Used as the true, unconditional default (ignores any persisted override).
    static std::filesystem::path default_data_dir();

    // Returns the persisted data_dir from GSDB-config.txt if that file exists
    // and is non-empty, otherwise default_data_dir(). Used as the constructor's
    // default argument, so `Database db;` picks up a prior \path change
    // automatically.
    static std::filesystem::path resolve_data_dir();

private:
    // Fixed location of the persistence file: ~/Documents/GSDB-config.txt.
    // Deliberately NOT inside data_dir_ — it must stay put even when
    // data_dir_ itself moves.
    static std::filesystem::path config_file_path();

    // Writes dir (as a single line) to config_file_path(), creating parent
    // directories if needed. Used by set_data_dir().
    static void persist_data_dir(const std::filesystem::path& dir);

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