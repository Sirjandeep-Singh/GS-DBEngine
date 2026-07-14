#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <stdexcept>
#include "schema.h"
#include "../storage/buffer_pool.h"
#include "../wal/wal_manager.h"

// CatalogManager owns all table and index schemas.
//
// On startup   : load() reads all schemas from the catalog page into memory
// CREATE TABLE : create_table() persists schema to disk + adds to in-memory map
// DROP TABLE   : drop_table() removes schema from disk + in-memory map
// Queries      : get_table() returns schema from in-memory map — no disk read
//
// The catalog is stored as a serialized blob on a reserved catalog page (page 1).
// All schema changes go through WALManager for durability.

class CatalogManager {
public:
    CatalogManager(BufferPool& buffer_pool, WALManager& wal);

    // called on startup — reads catalog page into memory
    // if catalog page does not exist yet, initializes an empty catalog
    void load(bool is_new_database);

    // persists a new table schema to disk and registers it in memory
    // throws if a table with this name already exists
    void create_table(const TableSchema& schema);

    // removes a table schema from disk and memory
    // throws if table does not exist
    void drop_table(const std::string& table_name);

    // returns the schema for a table by name
    // throws if table does not exist
    const TableSchema& get_table(const std::string& table_name) const;

    // returns true if a table with this name exists
    bool table_exists(const std::string& table_name) const;

    // returns all table names
    std::vector<std::string> list_tables() const;

    // persists a new index schema to disk and registers it in memory
    void create_index(const IndexSchema& index);

    // removes an index schema from disk and memory
    void drop_index(const std::string& index_name);

    // returns the index schema by name
    // throws if index does not exist
    const IndexSchema& get_index(const std::string& index_name) const;

    // returns all indexes for a given table
    std::vector<IndexSchema> get_indexes_for_table(const std::string& table_name) const;

    // returns every FOREIGN KEY constraint, across every table, whose
    // ref_table is `parent_table` — as (child_table_name, constraint)
    // pairs. Used for parent-side enforcement (DELETE/UPDATE on the
    // referenced table) and to block DROP TABLE on a table something
    // else still references. O(number of tables); fine at catalog scale.
    std::vector<std::pair<std::string, ForeignKeyConstraint>>
    get_foreign_keys_referencing(const std::string& parent_table) const;

    // updates the root_page of a table's B+ tree — called when B+ tree root changes
    void update_table_root(const std::string& table_name, uint32_t new_root_page);

    // updates the root_page of an index's B+ tree
    void update_index_root(const std::string& index_name, uint32_t new_root_page);

    static const uint32_t CATALOG_PAGE_ID = 1;  // catalog always lives on page 1

private:
    BufferPool& buffer_pool_;
    WALManager& wal_;

    std::unordered_map<std::string, TableSchema> tables_;
    std::unordered_map<std::string, IndexSchema> indexes_;

    // serializes all schemas into the catalog page and writes to disk via WAL
    void persist();

    // serializes tables_ and indexes_ into raw bytes
    std::vector<uint8_t> serialize() const;

    // deserializes raw bytes back into tables_ and indexes_
    void deserialize(const std::vector<uint8_t>& data);
};