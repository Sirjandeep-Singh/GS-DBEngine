<pre>
GS-DBEngine/
├── src/
│   ├── storage/
│   │   ├── disk_manager.h
│   │   ├── disk_manager.cpp
│   │   ├── page.h               ← just the Page struct, no .cpp needed
│   │   └── buffer_pool.h
│   │   └── buffer_pool.cpp
│   │
│   ├── wal/
│   │   ├── wal_manager.h
│   │   └── wal_manager.cpp
│   │
│   ├── header/
│   │   ├── header_manager.h
│   │   └── header_manager.cpp
│   │
│   ├── catalog/
│   │   ├── schema.h             ← Table, Column structs (header-only)
│   │   ├── catalog_manager.h
│   │   └── catalog_manager.cpp
│   │
│   ├── btree/
│   │   ├── btree.h
│   │   ├── btree.cpp
│   │   ├── btree_node.h         ← internal/leaf node structs
│   │   ├── btree_node.cpp
│   │   ├── free_list_manager.h
│   │   └── free_list_manager.cpp
│   │
│   ├── row/
│   │   ├── row.h                ← Row struct, column value types (header-only)
│   │   ├── serializer.h
│   │   └── serializer.cpp
│   │
│   ├── table/
│   │   ├── table.h
│   │   └── table.cpp
│   │
│   ├── parser/
│   │   ├── token.h
│   │   ├── tokenizer.h
│   │   ├── tokenizer.cpp
│   │   ├── parser.h
│   │   ├── parser.cpp
│   │   └── ast.h                ← all AST node structs (header-only)
│   │
│   ├── executor/
│   │   ├── executor.h
│   │   └── executor.cpp
│   │
│   └── database.h               ← top level class
│   └── database.cpp
│
├── tests/
│   ├── test_storage.cpp
│   ├── test_header_manager.cpp
│   ├── test_wal.cpp
│   ├── test_catalog.cpp
│   ├── test_btree_node.cpp
│   ├── test_btree.cpp
│   ├── test_row.cpp
│   ├── test_table.cpp
│   ├── test_tokenizer.cpp
│   ├── test_parser.cpp
│   ├── test_executor.cpp
│   └── test_database.cpp
│
├── cli/
│   └── main.cpp
│
├── ~/Documents/GS-DBEngine/     ← where .db and .wal files live at runtime
│                                  (outside the repo — see DECISIONS.md)
│
├── CMakeLists.txt               ← build system
└── README.md
</pre>