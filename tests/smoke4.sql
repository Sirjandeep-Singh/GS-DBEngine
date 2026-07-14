-- smoke4.sql — regression + SQL-surface boundary check after generalizing
-- the BTree layer to composite/non-int Key (vector<Value>).
--
-- IMPORTANT: the BTree layer itself now fully supports non-int and
-- composite keys (proven directly in tests/test_btree.cpp — VARCHAR keys,
-- 2-column composite keys, splits/merges/range_scan all pass). What this
-- script checks is the SQL surface on top of it, which intentionally
-- still only exposes a single-column INT PRIMARY KEY in this pass —
-- wiring composite/non-int PK through the parser and TableSchema is the
-- deferred follow-up.
--
-- Expected results:
--   1) baseline INT PK table — works exactly as before the refactor
--      (regression check: the refactor changed nothing observable here)
--   2) VARCHAR PRIMARY KEY — CREATE TABLE succeeds (the parser never
--      restricted PK to INT), but INSERT throws, because
--      Table::extract_primary_key still enforces INT-only until the
--      follow-up lands
--   3) table-level composite PRIMARY KEY (a, b) — fails to parse, since
--      parse_create_table() has no table-level PRIMARY KEY clause yet
--      (only inline single-column PRIMARY KEY as a column constraint)

CREATE DATABASE smoke4;
USE smoke4;

-- 1) baseline: single-column INT PK, unchanged behavior
CREATE TABLE orders (id INT PRIMARY KEY, total INT);
INSERT INTO orders VALUES (1, 100);
INSERT INTO orders VALUES (2, 250);
SELECT * FROM orders;

-- 2) VARCHAR PRIMARY KEY — CREATE TABLE succeeds, INSERT is expected to
--    fail with "primary key must be INT"
CREATE TABLE countries (code VARCHAR(3) PRIMARY KEY, name VARCHAR(50));
INSERT INTO countries VALUES ('USA', 'United States');

-- 3) composite PRIMARY KEY — expected to fail to parse
CREATE TABLE order_items (order_id INT, product_id INT, qty INT, PRIMARY KEY (order_id, product_id));

DROP DATABASE smoke4;