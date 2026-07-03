-- tests/smoke.sql
-- Pipe this into the CLI binary: ./gsdb < tests/smoke.sql
-- Expected: no ERROR lines in output.

-- Database lifecycle
CREATE DATABASE smokedb;
USE smokedb;

-- Table creation
CREATE TABLE users (
    id   INT PRIMARY KEY AUTO_INCREMENT,
    name VARCHAR(50) NOT NULL,
    age  INT
);

-- Inserts
INSERT INTO users VALUES (NULL, 'Alice', 25);
INSERT INTO users VALUES (NULL, 'Bob',   30);
INSERT INTO users VALUES (NULL, 'Carol', 22);

-- Basic SELECT
SELECT * FROM users;

-- WHERE
SELECT * FROM users WHERE age > 24;

-- ORDER BY
SELECT * FROM users ORDER BY age ASC;

-- LIMIT
SELECT * FROM users LIMIT 2;

-- UPDATE
UPDATE users SET age = 26 WHERE name = 'Alice';
SELECT * FROM users WHERE name = 'Alice';

-- DELETE
DELETE FROM users WHERE name = 'Bob';
SELECT * FROM users;

-- NULL handling
INSERT INTO users VALUES (NULL, 'Dave', NULL);
SELECT * FROM users WHERE age IS NULL;
SELECT * FROM users WHERE age IS NOT NULL;

-- JOIN smoke test
CREATE TABLE orders (
    id      INT PRIMARY KEY AUTO_INCREMENT,
    user_id INT NOT NULL,
    item    VARCHAR(50) NOT NULL
);

INSERT INTO orders VALUES (NULL, 1, 'Widget');
INSERT INTO orders VALUES (NULL, 3, 'Gadget');

SELECT * FROM users INNER JOIN orders ON users.id = orders.user_id;
SELECT * FROM users LEFT  JOIN orders ON users.id = orders.user_id;

-- SHOW / DROP
SHOW TABLES;
SHOW DATABASES;
DROP DATABASE smokedb;
SHOW DATABASES;
