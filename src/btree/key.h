#pragma once

#include <cstdint>
#include <vector>
#include "../row/row.h"

// A BTree key is an ordered tuple of Values. A single-column key (e.g. an
// INT or VARCHAR primary key) is simply a 1-element vector; a composite
// key (e.g. a two-column PRIMARY KEY (order_id, product_id)) is an
// N-element vector. This is what lets one BTree implementation serve both
// cases without a separate code path — non-int keys and composite keys
// are the same underlying capability.
//
// Ordering is lexicographic (tuple/dictionary order): compare element 0
// first; only if element 0 is equal do we look at element 1, and so on.
// std::vector<Value>'s default operator< / operator== already implement
// exactly this, via std::variant's own comparison operators (which compare
// the active alternative's value, given the same column always holds the
// same alternative type). So plain `<` / `==` on Key do the right thing
// everywhere in the BTree — no custom comparator needed.
using Key = std::vector<Value>;

// KeyCodec serializes a Key to/from a self-describing byte blob for
// storage in a BTree page cell.
//
// This is deliberately different from RowSerializer: RowSerializer needs
// a TableSchema to know each column's type ahead of time. The BTree layer
// has no notion of TableSchema at all (it's schema-free, generic storage),
// so each encoded element carries its own type tag inline instead.
//
// Wire format:
//   [num_elements: uint16_t]
//   per element:
//     [type_tag: uint8_t]   NULL=0, INT=1, FLOAT=2, BOOL=3, VARCHAR=4
//     [payload]             NULL:    none
//                            INT:     4 bytes, little-endian signed
//                            FLOAT:   4 bytes, IEEE 754 little-endian
//                            BOOL:    1 byte
//                            VARCHAR: [uint32_t length][length bytes UTF-8]
//
// The encoding is self-delimiting (the element count plus each element's
// own tag/length tells you exactly where it ends), so no outer length
// prefix is needed to know where a decoded Key stops.
class KeyCodec {
public:
    static std::vector<uint8_t> encode(const Key& key);

    // decodes a Key starting at *ptr; advances ptr past the bytes consumed.
    static Key decode(const uint8_t*& ptr);

private:
    enum class Tag : uint8_t {
        NUL     = 0,
        INT     = 1,
        FLOAT   = 2,
        BOOLEAN = 3,
        VARCHAR = 4,
    };
};
