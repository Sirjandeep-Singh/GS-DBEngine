-- smoke5.sql — smoke test for CREATE INDEX (backfill + live maintenance)
-- and index-assisted SELECT/UPDATE/DELETE query planning.
-- Run with:  ./build/gsdb < tests/smoke5.sql
-- Every INSERT/CREATE/SELECT is labeled below with what it should print
-- so a passing run is easy to eyeball against expectations.

CREATE DATABASE smoke5db;
USE smoke5db;

-- ── Schema + data seeded BEFORE any index exists ───────────────────────────

CREATE TABLE people (
    id    INT PRIMARY KEY,
    dept  VARCHAR(20),
    age   INT,
    score FLOAT
);

INSERT INTO people VALUES (1,  'eng',   25, 1.0);
INSERT INTO people VALUES (2,  'sales', 30, 2.0);
INSERT INTO people VALUES (3,  'eng',   25, 3.0);
INSERT INTO people VALUES (4,  'sales', 22, 4.0);
INSERT INTO people VALUES (5,  'eng',   40, 5.0);
INSERT INTO people VALUES (6,  'sales', 30, 6.0);
INSERT INTO people VALUES (7,  'eng',   25, 7.0);
INSERT INTO people VALUES (8,  'sales', 22, 8.0);

-- ── CREATE INDEX backfills the rows that already exist ──────────────────────

-- expect: OK — single-column index on dept
CREATE INDEX idx_dept ON people (dept);
-- expect: OK — composite index, leftmost column dept
CREATE INDEX idx_dept_age ON people (dept, age);
-- expect: OK — unique index, id has no duplicates
CREATE UNIQUE INDEX idx_id ON people (id);

-- expect: ERROR — name already in use
CREATE INDEX idx_dept ON people (age);
-- expect: ERROR — unknown column
CREATE INDEX idx_bad ON people (nonexistent);
-- expect: ERROR — unknown table
CREATE INDEX idx_bad2 ON no_such_table (id);
-- expect: ERROR — dept has duplicates, UNIQUE index rejected
CREATE UNIQUE INDEX idx_dept_unique ON people (dept);

-- ── Index-assisted SELECT ────────────────────────────────────────────────────

-- expect: 4 rows — ids 1, 3, 5, 7 (single-column equality via idx_dept)
SELECT id FROM people WHERE dept = 'eng';

-- expect: 3 rows — ids 1, 3, 7 (composite index, both columns pin down age=25)
SELECT id FROM people WHERE dept = 'eng' AND age = 25;

-- expect: 2 rows — ids 5, 7 (index narrows to dept='eng', then score>4 filters the rest)
SELECT id FROM people WHERE dept = 'eng' AND score > 4;

-- expect: 1 row — id 3 (int literal '3' coerced to match FLOAT column score)
SELECT id FROM people WHERE score = 3;

-- expect: 5 rows — ids 1,3,5,7 (dept=eng) plus id 8 (score>7), OR must not be
-- incorrectly narrowed down to just the dept branch
SELECT id FROM people WHERE dept = 'eng' OR score > 7;

-- expect: COUNT(*) = 4
SELECT COUNT(*) FROM people WHERE dept = 'eng';

-- ── Index-assisted UPDATE / DELETE, and the index stays live afterward ─────

-- expect: 3 rows affected — ids 1, 3, 7 (dept='eng' AND age=25; id 5 is eng but age=40)
UPDATE people SET age = 99 WHERE dept = 'eng' AND age = 25;
-- expect: 3 rows — ids 1, 3, 7 now have age = 99
SELECT id FROM people WHERE age = 99;

-- expect: 1 row affected — unique index on id narrows to exactly one row
DELETE FROM people WHERE id = 2;
-- expect: 0 rows — id 2 is gone
SELECT id FROM people WHERE id = 2;

-- expect: 4 rows affected — every dept='sales' row (4, 6, 8; id 2 already deleted)
DELETE FROM people WHERE dept = 'sales';
-- expect: 0 rows — no sales rows remain
SELECT id FROM people WHERE dept = 'sales';

-- expect: index kept in sync throughout — only the 4 original 'eng' rows remain
SELECT * FROM people;

DROP DATABASE smoke5db;
