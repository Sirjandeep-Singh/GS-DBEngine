#include "catalog_manager.h"

#include <cstring>
#include <stdexcept>

CatalogManager::CatalogManager(BufferPool& buffer_pool, WALManager& wal)
    : buffer_pool_(buffer_pool), wal_(wal)
{}

void CatalogManager::load(bool is_new_database) {
    if (is_new_database) {
        // CatalogManager owns page 1 — allocate it directly via buffer pool
        // allocate the catalog page — page 1 (page 0 is the db header)
            // only allocate if page 1 doesn't exist yet
            // DiskManager starts with 0 pages for a new db, header allocates page 0
            // so we allocate page 1 here and write an empty catalog to it

        uint32_t page_id;
        Page* page = buffer_pool_.new_page(page_id);

        if (page_id != CATALOG_PAGE_ID) {
            throw std::runtime_error(
                "CatalogManager::load: expected catalog at page 1, got "
                + std::to_string(page_id));
        }

        std::memset(page->data, 0, PAGE_SIZE);
        buffer_pool_.unpin_page(page_id, false);  // empty catalog, nothing to persist yet
        return;
    }

    // existing database — read and deserialize catalog page
    Page* page = buffer_pool_.fetch_page(CATALOG_PAGE_ID);
    std::vector<uint8_t> data(page->data, page->data + PAGE_SIZE);
    buffer_pool_.unpin_page(CATALOG_PAGE_ID, false);
    deserialize(data);
}

void CatalogManager::create_table(const TableSchema& schema) {
    if (tables_.count(schema.name)) {
        throw std::runtime_error("CatalogManager::create_table: table already exists: " + schema.name);
    }
    tables_[schema.name] = schema;
    persist();
}

void CatalogManager::drop_table(const std::string& table_name) {
    if (!tables_.count(table_name)) {
        throw std::runtime_error("CatalogManager::drop_table: table does not exist: " + table_name);
    }
    tables_.erase(table_name);

    // Drop every index belonging to this table too — both user-created
    // (CREATE INDEX) and the auto-named ones backing UNIQUE constraints
    // (see Executor::execute_create_table). Without this, dropping the
    // table leaves IndexSchema entries in indexes_ pointing at a
    // table_name that no longer exists: get_indexes_for_table() would
    // wrongly attach them to a future table created with the same name,
    // and their names would stay permanently reserved even though nothing
    // references them anymore. This mirrors DROP INDEX's existing gap of
    // not reclaiming the underlying B+ tree pages via the free list — that
    // page-orphaning is a separate, pre-existing limitation this doesn't
    // attempt to fix, only the catalog-metadata leak.
    for (auto it = indexes_.begin(); it != indexes_.end(); ) {
        if (it->second.table_name == table_name) {
            it = indexes_.erase(it);
        } else {
            ++it;
        }
    }

    persist();
}

const TableSchema& CatalogManager::get_table(const std::string& table_name) const {
    auto it = tables_.find(table_name);
    if (it == tables_.end()) {
        throw std::runtime_error("CatalogManager::get_table: table does not exist: " + table_name);
    }
    return it->second;
}

bool CatalogManager::table_exists(const std::string& table_name) const {
    return tables_.count(table_name) > 0;
}

std::vector<std::string> CatalogManager::list_tables() const {
    std::vector<std::string> names;
    names.reserve(tables_.size());
    for (auto& [name, _] : tables_) {
        names.push_back(name);
    }
    return names;
}

void CatalogManager::create_index(const IndexSchema& index) {
    if (indexes_.count(index.name)) {
        throw std::runtime_error("CatalogManager::create_index: index already exists: " + index.name);
    }
    indexes_[index.name] = index;
    persist();
}

void CatalogManager::drop_index(const std::string& index_name) {
    if (!indexes_.count(index_name)) {
        throw std::runtime_error("CatalogManager::drop_index: index does not exist: " + index_name);
    }
    indexes_.erase(index_name);
    persist();
}

const IndexSchema& CatalogManager::get_index(const std::string& index_name) const {
    auto it = indexes_.find(index_name);
    if (it == indexes_.end()) {
        throw std::runtime_error("CatalogManager::get_index: index does not exist: " + index_name);
    }
    return it->second;
}

std::vector<IndexSchema> CatalogManager::get_indexes_for_table(const std::string& table_name) const {
    std::vector<IndexSchema> result;
    for (auto& [_, idx] : indexes_) {
        if (idx.table_name == table_name) {
            result.push_back(idx);
        }
    }
    return result;
}

std::vector<std::pair<std::string, ForeignKeyConstraint>>
CatalogManager::get_foreign_keys_referencing(const std::string& parent_table) const {
    std::vector<std::pair<std::string, ForeignKeyConstraint>> result;
    for (auto& [_, schema] : tables_) {
        for (auto& fk : schema.foreign_keys) {
            if (fk.ref_table == parent_table) {
                result.emplace_back(schema.name, fk);
            }
        }
    }
    return result;
}

void CatalogManager::update_table_root(const std::string& table_name, uint32_t new_root_page) {
    auto it = tables_.find(table_name);
    if (it == tables_.end()) {
        throw std::runtime_error("CatalogManager::update_table_root: table does not exist: " + table_name);
    }
    it->second.root_page = new_root_page;
    persist();
}

void CatalogManager::update_index_root(const std::string& index_name, uint32_t new_root_page) {
    auto it = indexes_.find(index_name);
    if (it == indexes_.end()) {
        throw std::runtime_error("CatalogManager::update_index_root: index does not exist: " + index_name);
    }
    it->second.root_page = new_root_page;
    persist();
}

void CatalogManager::persist() {
    std::vector<uint8_t> data = serialize();
 
    if (data.size() > PAGE_SIZE) {
        throw std::runtime_error("CatalogManager::persist: catalog exceeds one page — too many tables/indexes");
    }
 
    // build the page locally — not yet touching buffer pool or disk
    Page page;
    std::memset(page.data, 0, PAGE_SIZE);
    std::memcpy(page.data, data.data(), data.size());
 
    // write through WAL — commit() applies it to the buffer pool
    uint32_t txn = wal_.begin();
    wal_.write(txn, CATALOG_PAGE_ID, page);
    wal_.commit(txn);
}

// serialization helpers — write primitives into a byte vector

static void write_uint32(std::vector<uint8_t>& buf, uint32_t val) {
    buf.push_back((val >>  0) & 0xFF);
    buf.push_back((val >>  8) & 0xFF);
    buf.push_back((val >> 16) & 0xFF);
    buf.push_back((val >> 24) & 0xFF);
}

static void write_uint8(std::vector<uint8_t>& buf, uint8_t val) {
    buf.push_back(val);
}

static void write_string(std::vector<uint8_t>& buf, const std::string& str) {
    write_uint32(buf, static_cast<uint32_t>(str.size()));
    for (char c : str) buf.push_back(static_cast<uint8_t>(c));
}

// deserialization helpers — read primitives from a byte vector at an offset

static uint32_t read_uint32(const std::vector<uint8_t>& buf, size_t& pos) {
    uint32_t val = 0;
    val |= static_cast<uint32_t>(buf[pos+0]) <<  0;
    val |= static_cast<uint32_t>(buf[pos+1]) <<  8;
    val |= static_cast<uint32_t>(buf[pos+2]) << 16;
    val |= static_cast<uint32_t>(buf[pos+3]) << 24;
    pos += 4;
    return val;
}

static uint8_t read_uint8(const std::vector<uint8_t>& buf, size_t& pos) {
    return buf[pos++];
}

static std::string read_string(const std::vector<uint8_t>& buf, size_t& pos) {
    uint32_t len = read_uint32(buf, pos);
    std::string str(buf.begin() + pos, buf.begin() + pos + len);
    pos += len;
    return str;
}

static void write_float(std::vector<uint8_t>& buf, float val) {
    uint32_t bits;
    std::memcpy(&bits, &val, sizeof(bits));
    write_uint32(buf, bits);
}

static float read_float(const std::vector<uint8_t>& buf, size_t& pos) {
    uint32_t bits = read_uint32(buf, pos);
    float val;
    std::memcpy(&val, &bits, sizeof(val));
    return val;
}

// Value tag bytes — match the alternative index order in row.h's Value variant.
enum ValueTag : uint8_t { TAG_NULL = 0, TAG_INT = 1, TAG_FLOAT = 2, TAG_BOOL = 3, TAG_STRING = 4 };

static void write_value(std::vector<uint8_t>& buf, const Value& v) {
    if (is_null(v)) {
        write_uint8(buf, TAG_NULL);
    } else if (std::holds_alternative<int32_t>(v)) {
        write_uint8(buf, TAG_INT);
        write_uint32(buf, static_cast<uint32_t>(get_int(v)));
    } else if (std::holds_alternative<float>(v)) {
        write_uint8(buf, TAG_FLOAT);
        write_float(buf, get_float(v));
    } else if (std::holds_alternative<bool>(v)) {
        write_uint8(buf, TAG_BOOL);
        write_uint8(buf, get_bool(v) ? 1 : 0);
    } else {
        write_uint8(buf, TAG_STRING);
        write_string(buf, get_string(v));
    }
}

static Value read_value(const std::vector<uint8_t>& buf, size_t& pos) {
    uint8_t tag = read_uint8(buf, pos);
    switch (tag) {
        case TAG_NULL:   return std::monostate{};
        case TAG_INT:    return static_cast<int32_t>(read_uint32(buf, pos));
        case TAG_FLOAT:  return read_float(buf, pos);
        case TAG_BOOL:   return read_uint8(buf, pos) != 0;
        case TAG_STRING: return read_string(buf, pos);
        default:
            throw std::runtime_error("CatalogManager::read_value: unknown value tag");
    }
}

std::vector<uint8_t> CatalogManager::serialize() const {
    std::vector<uint8_t> buf;

    // tables
    write_uint32(buf, static_cast<uint32_t>(tables_.size()));
    for (auto& [_, schema] : tables_) {
        write_string(buf, schema.name);
        write_uint32(buf, schema.root_page);
        write_uint32(buf, static_cast<uint32_t>(schema.primary_key_indices.size()));
        for (uint32_t pk_idx : schema.primary_key_indices) {
            write_uint32(buf, pk_idx);
        }
        write_uint32(buf, static_cast<uint32_t>(schema.columns.size()));
        for (auto& col : schema.columns) {
            write_string(buf, col.name);
            write_uint8(buf, static_cast<uint8_t>(col.type));
            write_uint32(buf, col.max_length);
            write_uint8(buf, col.is_nullable    ? 1 : 0);
            write_uint8(buf, col.is_primary_key ? 1 : 0);
            write_uint8(buf, col.auto_increment ? 1 : 0);
            write_uint8(buf, col.has_default    ? 1 : 0);
            if (col.has_default) {
                write_value(buf, col.default_value);
            }
        }
        write_uint32(buf, static_cast<uint32_t>(schema.checks.size()));
        for (auto& c : schema.checks) {
            write_uint32(buf, c.column_index);
            write_uint8(buf, static_cast<uint8_t>(c.op));
            write_value(buf, c.operand);
        }

        write_uint32(buf, static_cast<uint32_t>(schema.foreign_keys.size()));
        for (auto& fk : schema.foreign_keys) {
            write_uint32(buf, static_cast<uint32_t>(fk.column_indices.size()));
            for (uint32_t ci : fk.column_indices) write_uint32(buf, ci);
            write_string(buf, fk.ref_table);
            write_uint32(buf, static_cast<uint32_t>(fk.ref_column_indices.size()));
            for (uint32_t ci : fk.ref_column_indices) write_uint32(buf, ci);
            write_uint8(buf, static_cast<uint8_t>(fk.on_delete));
            write_string(buf, fk.child_index_name);
            write_string(buf, fk.ref_index_name);
        }

        write_string(buf, schema.create_sql);
    }

    // indexes
    write_uint32(buf, static_cast<uint32_t>(indexes_.size()));
    for (auto& [_, idx] : indexes_) {
        write_string(buf, idx.name);
        write_string(buf, idx.table_name);
        write_uint32(buf, static_cast<uint32_t>(idx.column_names.size()));
        for (auto& col_name : idx.column_names) {
            write_string(buf, col_name);
        }
        write_uint32(buf, idx.root_page);
        write_uint8(buf, idx.is_unique ? 1 : 0);
    }

    return buf;
}

void CatalogManager::deserialize(const std::vector<uint8_t>& data) {
    tables_.clear();
    indexes_.clear();

    if (data.empty() || (data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 0)) {
        return;  // empty catalog
    }

    size_t pos = 0;

    uint32_t num_tables = read_uint32(data, pos);
    for (uint32_t i = 0; i < num_tables; i++) {
        TableSchema schema;
        schema.name              = read_string(data, pos);
        schema.root_page         = read_uint32(data, pos);

        uint32_t num_pk_cols = read_uint32(data, pos);
        schema.primary_key_indices.reserve(num_pk_cols);
        for (uint32_t j = 0; j < num_pk_cols; j++) {
            schema.primary_key_indices.push_back(read_uint32(data, pos));
        }

        uint32_t num_cols = read_uint32(data, pos);
        for (uint32_t j = 0; j < num_cols; j++) {
            Column col;
            col.name           = read_string(data, pos);
            col.type           = static_cast<ColumnType>(read_uint8(data, pos));
            col.max_length     = read_uint32(data, pos);
            col.is_nullable    = read_uint8(data, pos) != 0;
            col.is_primary_key = read_uint8(data, pos) != 0;
            col.auto_increment = read_uint8(data, pos) != 0;
            col.has_default    = read_uint8(data, pos) != 0;
            if (col.has_default) {
                col.default_value = read_value(data, pos);
            }
            schema.columns.push_back(col);
        }

        uint32_t num_checks = read_uint32(data, pos);
        for (uint32_t j = 0; j < num_checks; j++) {
            CheckConstraint c;
            c.column_index = read_uint32(data, pos);
            c.op            = static_cast<CheckOp>(read_uint8(data, pos));
            c.operand       = read_value(data, pos);
            schema.checks.push_back(std::move(c));
        }

        uint32_t num_fks = read_uint32(data, pos);
        for (uint32_t j = 0; j < num_fks; j++) {
            ForeignKeyConstraint fk;
            uint32_t num_fk_cols = read_uint32(data, pos);
            fk.column_indices.reserve(num_fk_cols);
            for (uint32_t k = 0; k < num_fk_cols; k++) {
                fk.column_indices.push_back(read_uint32(data, pos));
            }
            fk.ref_table = read_string(data, pos);
            uint32_t num_ref_cols = read_uint32(data, pos);
            fk.ref_column_indices.reserve(num_ref_cols);
            for (uint32_t k = 0; k < num_ref_cols; k++) {
                fk.ref_column_indices.push_back(read_uint32(data, pos));
            }
            fk.on_delete        = static_cast<FkOnDelete>(read_uint8(data, pos));
            fk.child_index_name = read_string(data, pos);
            fk.ref_index_name   = read_string(data, pos);
            schema.foreign_keys.push_back(std::move(fk));
        }

        schema.create_sql = read_string(data, pos);

        tables_[schema.name] = schema;
    }

    uint32_t num_indexes = read_uint32(data, pos);
    for (uint32_t i = 0; i < num_indexes; i++) {
        IndexSchema idx;
        idx.name        = read_string(data, pos);
        idx.table_name  = read_string(data, pos);
        uint32_t num_index_cols = read_uint32(data, pos);
        idx.column_names.reserve(num_index_cols);
        for (uint32_t j = 0; j < num_index_cols; j++) {
            idx.column_names.push_back(read_string(data, pos));
        }
        idx.root_page   = read_uint32(data, pos);
        idx.is_unique   = read_uint8(data, pos) != 0;
        indexes_[idx.name] = idx;
    }
}