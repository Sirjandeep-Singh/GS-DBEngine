#include "database.h"

#include <cstdlib>
#include <fstream>
#include <stdexcept>

#include "parser/tokenizer.h"
#include "parser/parser.h"

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

std::filesystem::path Database::default_data_dir()
{
    const char* home = std::getenv("HOME");
    if (!home) {
        throw std::runtime_error("Database: $HOME is not set, cannot resolve default data_dir");
    }
    return std::filesystem::path(home) / "Documents" / "GS-DBEngine";
}

std::filesystem::path Database::config_file_path()
{
    const char* home = std::getenv("HOME");
    if (!home) {
        throw std::runtime_error("Database: $HOME is not set, cannot resolve config file path");
    }
    return std::filesystem::path(home) / "Documents" / "GSDB-config.txt";
}

std::filesystem::path Database::resolve_data_dir()
{
    std::error_code ec;
    std::filesystem::path cfg = config_file_path();
    if (!std::filesystem::exists(cfg, ec) || ec) {
        return default_data_dir();
    }

    std::ifstream in(cfg);
    std::string line;
    if (std::getline(in, line) && !line.empty()) {
        return std::filesystem::path(line);
    }
    return default_data_dir();
}

void Database::persist_data_dir(const std::filesystem::path& dir)
{
    std::filesystem::path cfg = config_file_path();
    std::filesystem::create_directories(cfg.parent_path());

    std::ofstream out(cfg, std::ios::trunc);
    if (!out) {
        throw std::runtime_error("Database: failed to write " + cfg.string());
    }
    out << dir.string() << "\n";
}

Database::Database(std::filesystem::path data_dir)
    : data_dir_(std::move(data_dir))
{
    std::filesystem::create_directories(data_dir_);
}

Database::~Database()
{
    close_database();  // flush buffer pool + clean WAL state on normal shutdown
}

// ─────────────────────────────────────────────────────────────────────────────
// Data directory management
// ─────────────────────────────────────────────────────────────────────────────

void Database::set_data_dir(std::filesystem::path new_dir)
{
    // Close whatever's active first — the old storage stack (open file
    // handles, buffer pool contents) belongs to the *old* data_dir and is
    // meaningless once we point at a new one. close_database() is already
    // a safe no-op if nothing is active.
    close_database();

    // Create the new directory if it doesn't exist yet. Throws on failure
    // (e.g. permission denied) — data_dir_ is left unchanged in that case,
    // since we haven't reassigned it yet.
    std::filesystem::create_directories(new_dir);

    data_dir_ = std::move(new_dir);

    // Persist so this survives future sessions.
    persist_data_dir(data_dir_);
}

void Database::reset_data_dir()
{
    close_database();

    data_dir_ = default_data_dir();
    std::filesystem::create_directories(data_dir_);

    // Remove the override file entirely, rather than persisting the default
    // path into it — this way, if default_data_dir()'s resolution logic ever
    // changes (e.g. a different $HOME), a fresh session still picks that up
    // instead of reading a now-stale literal path.
    std::error_code ec;
    std::filesystem::remove(config_file_path(), ec);
}

// ─────────────────────────────────────────────────────────────────────────────
// Path helpers
// ─────────────────────────────────────────────────────────────────────────────

std::filesystem::path Database::db_dir(const std::string& name) const
{
    return data_dir_ / name;
}

std::filesystem::path Database::db_file(const std::string& name) const
{
    return db_dir(name) / (name + ".db");
}

std::filesystem::path Database::wal_file(const std::string& name) const
{
    return db_dir(name) / (name + ".wal");
}

// ─────────────────────────────────────────────────────────────────────────────
// execute(string) — tokenize → parse → dispatch
// ─────────────────────────────────────────────────────────────────────────────

QueryResult Database::execute(const std::string& sql)
{
    try {
        Tokenizer tokenizer(sql);
        std::vector<Token> tokens = tokenizer.tokenize();
        Parser parser(std::move(tokens));
        Statement stmt = parser.parse();

        // Stash the verbatim source text on CREATE TABLE statements so
        // DESCRIBE can hand back exactly what the user typed later (the
        // same approach SQLite takes with sqlite_schema.sql). Trim
        // surrounding whitespace and a single trailing semicolon so the
        // stored text is copy-paste-ready on its own.
        if (auto* create = std::get_if<CreateTableStmt>(&stmt)) {
            size_t begin = sql.find_first_not_of(" \t\r\n");
            size_t end   = sql.find_last_not_of(" \t\r\n");
            std::string trimmed = (begin == std::string::npos)
                ? std::string()
                : sql.substr(begin, end - begin + 1);
            if (!trimmed.empty() && trimmed.back() == ';') {
                trimmed.pop_back();
                size_t end2 = trimmed.find_last_not_of(" \t\r\n");
                trimmed = (end2 == std::string::npos) ? std::string() : trimmed.substr(0, end2 + 1);
            }
            create->source_text = std::move(trimmed);
        }

        return execute(stmt);
    } catch (const std::exception& e) {
        return QueryResult{false, e.what()};
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// execute(Statement) — intercept database-level statements, forward the rest
// ─────────────────────────────────────────────────────────────────────────────

QueryResult Database::execute(const Statement& stmt)
{
    // Database-level statements are handled here, regardless of whether a
    // database is currently active.
    if (auto* s = std::get_if<CreateDatabaseStmt>(&stmt)) {
        return handle_create_database(*s);
    }
    if (auto* s = std::get_if<DropDatabaseStmt>(&stmt)) {
        return handle_drop_database(*s);
    }
    if (auto* s = std::get_if<UseStmt>(&stmt)) {
        return handle_use(*s);
    }
    if (auto* s = std::get_if<ShowStmt>(&stmt)) {
        if (s->target == ShowTarget::DATABASES) {
            return handle_show_databases();
        }
        // SHOW TABLES falls through to the active Executor below.
    }

    // Everything else requires an active database.
    if (!executor_) {
        return {false, "No database selected"};
    }

    try {
        return executor_->execute(stmt);
    } catch (const std::exception& e) {
        return QueryResult{false, e.what()};
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// CREATE DATABASE
// ─────────────────────────────────────────────────────────────────────────────

QueryResult Database::handle_create_database(const CreateDatabaseStmt& stmt)
{
    const std::string& name = stmt.name;

    if (std::filesystem::exists(db_dir(name))) {
        return {false, "Database '" + name + "' already exists"};
    }

    try {
        std::filesystem::create_directories(db_dir(name));

        // Bootstrap sequence for a brand new database. This stack is local —
        // it is torn down at the end of this function without becoming the
        // active database (CREATE DATABASE does not auto-USE).
        DiskManager    disk(db_file(name).string());
        BufferPool     buffer_pool(disk);
        WALManager     wal(wal_file(name).string(), buffer_pool);

        HeaderManager  header(buffer_pool, wal);
        header.init();

        CatalogManager catalog(buffer_pool, wal);
        catalog.load(/*is_new_database=*/true);

        // Flush everything to disk before the stack unwinds.
        buffer_pool.flush_all();
    } catch (const std::exception& e) {
        // Roll back the partially created directory so a failed CREATE
        // DATABASE doesn't leave a corrupt/half-initialized database behind.
        std::error_code ec;
        std::filesystem::remove_all(db_dir(name), ec);
        return {false, std::string("CREATE DATABASE failed: ") + e.what()};
    }

    return {true, "", {}, {}, 0};
}

// ─────────────────────────────────────────────────────────────────────────────
// DROP DATABASE
// ─────────────────────────────────────────────────────────────────────────────

QueryResult Database::handle_drop_database(const DropDatabaseStmt& stmt)
{
    const std::string& name = stmt.name;

    if (!std::filesystem::exists(db_dir(name))) {
        return {false, "Database '" + name + "' does not exist"};
    }

    // If the database being dropped is currently active, close it first so
    // no file handles are left open on the directory we're about to delete.
    if (current_db_name_ == name) {
        close_database();
    }

    std::error_code ec;
    std::filesystem::remove_all(db_dir(name), ec);
    if (ec) {
        return {false, "DROP DATABASE failed: " + ec.message()};
    }

    return {true, "", {}, {}, 0};
}

// ─────────────────────────────────────────────────────────────────────────────
// USE
// ─────────────────────────────────────────────────────────────────────────────

QueryResult Database::handle_use(const UseStmt& stmt)
{
    const std::string& name = stmt.name;

    // No-op if the requested database is already active.
    if (name == current_db_name_) {
        return {true, "", {}, {}, 0};
    }

    if (!std::filesystem::exists(db_dir(name))) {
        return {false, "Database '" + name + "' does not exist"};
    }

    close_database();

    try {
        open_database(name);
    } catch (const std::exception& e) {
        // Ensure we don't leave a half-open stack or a stale current_db_name_.
        close_database();
        return {false, std::string("USE failed: ") + e.what()};
    }

    return {true, "", {}, {}, 0};
}

// ─────────────────────────────────────────────────────────────────────────────
// SHOW DATABASES
// ─────────────────────────────────────────────────────────────────────────────

QueryResult Database::handle_show_databases()
{
    QueryResult result;
    result.success = true;
    result.columns = {"Databases"};

    for (const auto& entry : std::filesystem::directory_iterator(data_dir_)) {
        if (entry.is_directory()) {
            result.rows.push_back({entry.path().filename().string()});
        }
    }

    // Sort alphabetically — directory_iterator order is unspecified.
    std::sort(result.rows.begin(), result.rows.end());

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Storage stack lifecycle
// ─────────────────────────────────────────────────────────────────────────────

void Database::open_database(const std::string& name)
{
    // Bootstrap sequence for an existing database:
    //   open disk → buffer pool → wal → recover → header load → free list → catalog load
    disk_manager_    = std::make_unique<DiskManager>(db_file(name).string());
    buffer_pool_     = std::make_unique<BufferPool>(*disk_manager_);
    wal_manager_     = std::make_unique<WALManager>(wal_file(name).string(), *buffer_pool_);

    // Replay any committed-but-not-checkpointed transactions left over from
    // a prior crash, then checkpoint (flush to .db, truncate .wal).
    wal_manager_->recover();

    header_manager_  = std::make_unique<HeaderManager>(*buffer_pool_, *wal_manager_);
    header_manager_->load();

    free_list_manager_ = std::make_unique<FreeListManager>(*buffer_pool_, *wal_manager_, *header_manager_);

    catalog_manager_ = std::make_unique<CatalogManager>(*buffer_pool_, *wal_manager_);
    catalog_manager_->load(/*is_new_database=*/false);

    executor_ = std::make_unique<Executor>(*catalog_manager_, *buffer_pool_, *wal_manager_, *free_list_manager_);

    current_db_name_ = name;
}

void Database::close_database()
{
    if (current_db_name_.empty()) {
        return;  // no-op — nothing is active
    }

    if (buffer_pool_) {
        buffer_pool_->flush_all();
    }

    // Destroy in reverse dependency order.
    executor_.reset();
    catalog_manager_.reset();
    free_list_manager_.reset();
    wal_manager_.reset();
    header_manager_.reset();
    buffer_pool_.reset();
    disk_manager_.reset();

    current_db_name_.clear();
}