-- smoke3.sql — smoke test for IN / NOT IN / EXISTS / NOT EXISTS with
-- subqueries and correlated subqueries.
-- Run with:  ./build/gsdb < smoke3.sql
-- Every INSERT/CREATE/SELECT is labeled below with the row it should print
-- so a passing run is easy to eyeball against expectations.

CREATE DATABASE smoke3db;
USE smoke3db;

-- ── Schema ────────────────────────────────────────────────────────────────

CREATE TABLE customers (
    id   INT PRIMARY KEY AUTO_INCREMENT,
    name VARCHAR(50) NOT NULL
);

CREATE TABLE orders (
    id          INT PRIMARY KEY AUTO_INCREMENT,
    customer_id INT,
    item        VARCHAR(50) NOT NULL,
    amount      INT
);

-- customers: 1 Alice, 2 Bob, 3 Carol, 4 Dave
INSERT INTO customers VALUES (NULL, 'Alice');
INSERT INTO customers VALUES (NULL, 'Bob');
INSERT INTO customers VALUES (NULL, 'Carol');
INSERT INTO customers VALUES (NULL, 'Dave');

-- orders: Alice has 2, Bob has 1, Carol/Dave have none.
-- One order has a NULL customer_id -- deliberately, to test the classic
-- NOT IN + NULL trap.
INSERT INTO orders VALUES (NULL, 1, 'Widget', 10);
INSERT INTO orders VALUES (NULL, 1, 'Gizmo',  20);
INSERT INTO orders VALUES (NULL, 2, 'Gadget', 15);
INSERT INTO orders VALUES (NULL, NULL, 'Orphan', 5);

-- ── IN ────────────────────────────────────────────────────────────────────

-- expect: 2 rows -- Alice, Bob (the only customer_ids that appear in orders)
SELECT * FROM customers WHERE id IN (SELECT customer_id FROM orders);

-- expect: 2 rows -- Widget, Gizmo (Alice's orders)
SELECT * FROM orders WHERE customer_id IN (SELECT id FROM customers WHERE name = 'Alice');

-- expect: 0 rows -- no order has amount > 1000
SELECT * FROM customers WHERE id IN (SELECT customer_id FROM orders WHERE amount > 1000);

-- ── NOT IN ────────────────────────────────────────────────────────────────

-- expect: 2 rows -- Carol, Dave (NULLs filtered out of the subquery first,
-- so NOT IN behaves the "expected" way)
SELECT * FROM customers
WHERE id NOT IN (SELECT customer_id FROM orders WHERE customer_id IS NOT NULL);

-- expect: 0 rows -- classic trap: the subquery contains a NULL
-- (the orphan order's customer_id), so x NOT IN (..., NULL) is UNKNOWN
-- for every row, and no rows should be returned
SELECT * FROM customers WHERE id NOT IN (SELECT customer_id FROM orders);

-- expect: 4 rows -- subquery is empty, so NOT IN is trivially true for all rows
SELECT * FROM customers WHERE id NOT IN (SELECT customer_id FROM orders WHERE amount > 1000);

-- ── EXISTS (correlated) ──────────────────────────────────────────────────

-- expect: 2 rows -- Alice, Bob (customers with at least one order)
SELECT * FROM customers c
WHERE EXISTS (SELECT 1 FROM orders o WHERE o.customer_id = c.id);

-- expect: 2 rows -- Alice (Gizmo 20), Bob (Gadget 15); Alice's Widget alone
-- wouldn't qualify but she still has a qualifying order
SELECT * FROM customers c
WHERE EXISTS (SELECT 1 FROM orders o WHERE o.customer_id = c.id AND o.amount > 12);

-- expect: 4 rows -- uncorrelated EXISTS, orders table is non-empty, so
-- true for every customer row
SELECT * FROM customers WHERE EXISTS (SELECT * FROM orders);

-- ── NOT EXISTS (correlated) ──────────────────────────────────────────────

-- expect: 2 rows -- Carol, Dave (no matching order at all)
SELECT * FROM customers c
WHERE NOT EXISTS (SELECT 1 FROM orders o WHERE o.customer_id = c.id);

-- expect: 2 rows -- Alice, Carol; Bob's only order (15) beats the bar so
-- he's excluded, Dave has no orders so vacuously true -> included... 
-- i.e. customers with no order over amount 15
SELECT * FROM customers c
WHERE NOT EXISTS (SELECT 1 FROM orders o WHERE o.customer_id = c.id AND o.amount > 15);

-- expect: 4 rows -- NOT IN's NULL trap does NOT apply to NOT EXISTS;
-- this is the well-behaved equivalent of the NOT IN case above and should
-- return Carol and Dave plus... actually no order matches amount > 1000,
-- so every customer satisfies NOT EXISTS -> all 4 rows
SELECT * FROM customers c
WHERE NOT EXISTS (SELECT 1 FROM orders o WHERE o.customer_id = c.id AND o.amount > 1000);

-- ── Nested / double-correlated ───────────────────────────────────────────

-- expect: 1 row -- Bob: customers who have an order, but none of their
-- orders is the cheapest one in the whole table (amount = 5, the orphan
-- order, doesn't count since it's nobody's, so really this checks Bob
-- has no order under 15)
SELECT * FROM customers c
WHERE EXISTS (SELECT 1 FROM orders o WHERE o.customer_id = c.id)
  AND NOT EXISTS (
        SELECT 1 FROM orders o2
        WHERE o2.customer_id = c.id AND o2.amount < 15
      );

-- ── Error cases ───────────────────────────────────────────────────────────

-- expect: ERROR -- subquery for IN must return exactly one column
SELECT * FROM customers WHERE id IN (SELECT customer_id, item FROM orders);

-- expect: ERROR -- unknown column referenced inside correlated subquery
SELECT * FROM customers c
WHERE EXISTS (SELECT 1 FROM orders o WHERE o.customer_id = c.nonexistent_col);

-- expect: ERROR -- unknown outer alias referenced inside subquery
SELECT * FROM customers c
WHERE EXISTS (SELECT 1 FROM orders o WHERE o.customer_id = z.id);

DROP DATABASE smoke3db;