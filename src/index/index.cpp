#include "index.h"

#include <stdexcept>

Index::Index(const IndexSchema& schema, size_t pk_arity,
             BufferPool& buffer_pool, WALManager& wal, FreeListManager& free_list)
    : schema_(schema), pk_arity_(pk_arity),
      btree_(buffer_pool, wal, free_list, schema.root_page)
{}

uint32_t Index::root_page() const {
    return btree_.root_page();
}

bool Index::is_indexable(const Key& indexed_value) {
    for (const Value& v : indexed_value) {
        if (is_null(v)) return false;
    }
    return true;
}

Key Index::make_entry_key(const Key& indexed_value, const Key& primary_key) const {
    if (indexed_value.size() != schema_.column_names.size()) {
        throw std::runtime_error(
            "Index::make_entry_key: indexed_value arity (" + std::to_string(indexed_value.size()) +
            ") does not match index '" + schema_.name + "' (" +
            std::to_string(schema_.column_names.size()) + ")");
    }
    if (primary_key.size() != pk_arity_) {
        throw std::runtime_error(
            "Index::make_entry_key: primary_key arity (" + std::to_string(primary_key.size()) +
            ") does not match table primary key arity (" + std::to_string(pk_arity_) + ")");
    }

    Key entry_key = indexed_value;
    entry_key.insert(entry_key.end(), primary_key.begin(), primary_key.end());
    return entry_key;
}

void Index::check_unique(const Key& indexed_value, const Key& primary_key) const {
    if (!schema_.is_unique) return;

    auto existing = btree_.prefix_scan(indexed_value);
    for (auto& [existing_key, _] : existing) {
        Key existing_pk(existing_key.begin() + indexed_value.size(), existing_key.end());
        if (existing_pk != primary_key) {
            throw std::runtime_error(
                "Index::insert_entry: duplicate value violates unique index '" + schema_.name + "'");
        }
    }
}

void Index::insert_entry(const Key& indexed_value, const Key& primary_key) {
    if (!is_indexable(indexed_value)) return;

    check_unique(indexed_value, primary_key);

    Key entry_key = make_entry_key(indexed_value, primary_key);
    btree_.insert(entry_key, {});
}

void Index::insert_entry(uint32_t transaction_id, const Key& indexed_value, const Key& primary_key) {
    if (!is_indexable(indexed_value)) return;

    // validate before writing — this call cannot roll back a page
    // mutation already made under transaction_id by an earlier step
    // (see BTree's transaction_id overload of insert for why), so the
    // unique check must run first, same as the self-contained overload.
    check_unique(indexed_value, primary_key);

    Key entry_key = make_entry_key(indexed_value, primary_key);
    btree_.insert(transaction_id, entry_key, {});
}

void Index::remove_entry(const Key& indexed_value, const Key& primary_key) {
    if (!is_indexable(indexed_value)) return;

    Key entry_key = make_entry_key(indexed_value, primary_key);
    btree_.remove(entry_key);
}

void Index::remove_entry(uint32_t transaction_id, const Key& indexed_value, const Key& primary_key) {
    if (!is_indexable(indexed_value)) return;

    Key entry_key = make_entry_key(indexed_value, primary_key);
    btree_.remove(transaction_id, entry_key);
}

std::vector<Key> Index::find(const Key& indexed_value) const {
    std::vector<Key> result;
    if (!is_indexable(indexed_value)) return result;

    // indexed_value may be a LEFTMOST PREFIX of the index's columns, not
    // necessarily all of them — e.g. an index on (last_name, first_name)
    // supports find({last_name}) alone (all rows with that last name,
    // any first name), same as it supports find({last_name, first_name})
    // for an exact match. Either way, every stored entry's key has
    // exactly schema_.column_names.size() leading elements before the
    // primary key starts, regardless of how many of those elements the
    // caller supplied to narrow the search — so the split point is
    // always the FULL index arity, never indexed_value.size().
    if (indexed_value.size() > schema_.column_names.size()) {
        throw std::runtime_error(
            "Index::find: search value has more columns (" + std::to_string(indexed_value.size()) +
            ") than index '" + schema_.name + "' indexes (" +
            std::to_string(schema_.column_names.size()) + ")");
    }

    auto entries = btree_.prefix_scan(indexed_value);
    result.reserve(entries.size());
    for (auto& [key, _] : entries) {
        result.emplace_back(key.begin() + schema_.column_names.size(), key.end());
    }
    return result;
}
