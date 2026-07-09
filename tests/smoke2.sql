-- smoke2.sql — smoke test for CHECK constraints and COUNT(*) / COUNT(column)
-- Run with:  ./build/gsdb < smoke2.sql
-- Every INSERT/CREATE/SELECT is labeled below with the row it should print
-- so a passing run is easy to eyeball against expectations.

CREATE DATABASE smoke2db;
USE smoke2db;

-- ── CHECK constraints ────────────────────────────────────────────────────

-- column-level CHECK + table-level CHECK (AND-split into two constraints)
CREATE TABLE accounts (
    id      INT PRIMARY KEY AUTO_INCREMENT,
    balance INT CHECK (balance >= 0),
    age     INT,
    CHECK (age > 0 AND age < 150)
);

-- expect: OK
INSERT INTO accounts (balance, age) VALUES (100, 30);
-- expect: OK  (NULL age passes CHECK -- three-valued logic, not a violation)
INSERT INTO accounts (balance, age) VALUES (50, NULL);
-- expect: ERROR -- balance >= 0 violated
INSERT INTO accounts (balance, age) VALUES (-5, 30);
-- expect: ERROR -- age < 150 violated
INSERT INTO accounts (balance, age) VALUES (100, 200);
-- expect: ERROR -- age > 0 violated
INSERT INTO accounts (balance, age) VALUES (100, 0);

-- expect: ERROR -- UPDATE must also enforce CHECK, no row changed
UPDATE accounts SET balance = -1 WHERE id = 1;

-- expect: ERROR at CREATE TABLE -- OR is not supported (only AND-of-comparisons)
CREATE TABLE bad1 (id INT PRIMARY KEY, x INT CHECK (x = 1 OR x = 2));
-- expect: ERROR at CREATE TABLE -- unknown column referenced in CHECK
CREATE TABLE bad2 (id INT PRIMARY KEY, x INT, CHECK (y > 0));

-- expect: 2 rows -- id 1 (100/30) and id 2 (50/NULL), no bad rows made it in
SELECT * FROM accounts;

-- ── COUNT(*) and COUNT(column) ───────────────────────────────────────────

CREATE TABLE people (
    id   INT PRIMARY KEY AUTO_INCREMENT,
    name VARCHAR(20),
    age  INT
);

INSERT INTO people (name, age) VALUES ('alice', 20);
INSERT INTO people (name, age) VALUES ('bob', NULL);
INSERT INTO people (name, age) VALUES ('cara', 30);

-- expect: COUNT(*) = 3
SELECT COUNT(*) FROM people;
-- expect: COUNT(age) = 2  (bobs NULL age excluded)
SELECT COUNT(age) FROM people;
-- expect: COUNT(*) = 2  (WHERE applied before counting)
SELECT COUNT(*) FROM people WHERE age > 15;
-- expect: one row, COUNT(*) = 3, COUNT(age) = 2 -- multiple aggregates together
SELECT COUNT(*), COUNT(age) FROM people;

-- expect: ERROR -- cannot mix a plain column with an aggregate, no GROUP BY yet
SELECT name, COUNT(*) FROM people;
-- expect: ERROR -- aggregates not yet supported with JOIN
SELECT COUNT(*) FROM people INNER JOIN accounts ON people.id = accounts.id;

DROP DATABASE smoke2db;