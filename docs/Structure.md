<pre>
mydbengine/
├── src/
│   ├── storage/
│   │   ├── disk_manager.h
│   │   ├── disk_manager.cpp
│   │   ├── header_manager.h
│   │   ├── header_manager.cpp
│   │   ├── page.h               ← just the Page struct, no .cpp needed
│   │   └── buffer_pool.h
│   │   └── buffer_pool.cpp
│   │
│   ├── wal/
│   │   ├── wal_manager.h
│   │   └── wal_manager.cpp
│   │
│   ├── catalog/
│   │   ├── schema.h             ← Table, Column structs
│   │   ├── schema.cpp
│   │   ├── catalog_manager.h
│   │   └── catalog_manager.cpp
│   │
│   ├── btree/
│   │   ├── btree.h
│   │   ├── btree.cpp
│   │   ├── btree_node.h         ← internal/leaf node structs
│   │   └── btree_node.cpp
│   │
│   ├── row/
│   │   ├── row.h                ← Row struct, column value types
│   │   ├── row.cpp
│   │   ├── serializer.h
│   │   └── serializer.cpp
│   │
│   ├── table/
│   │   ├── table.h
│   │   └── table.cpp
│   │
│   ├── parser/
│   │   ├── tokenizer.h
│   │   ├── tokenizer.cpp
│   │   ├── parser.h
│   │   ├── parser.cpp
│   │   ├── ast.h                ← all AST node structs
│   │   └── ast.cpp
│   │
│   ├── executor/
│   │   ├── executor.h
│   │   └── executor.cpp
│   │
│   └── database.h               ← top level class
│   └── database.cpp
│
├── tests/
│   ├── test_disk_manager.cpp
│   ├── test_buffer_pool.cpp
│   ├── test_btree.cpp
│   └── ...
│
├── databases/                   ← where .db and .wal files live at runtime
│
├── CMakeLists.txt               ← build system
└── README.md
</pre>