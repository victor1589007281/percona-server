-- ============================================================================
-- MySQL Binlog Parser Validation Test SQL
-- ============================================================================
-- This file contains SQL statements to generate all types of binlog events
-- for testing and validating binlog parser implementations.
--
-- Usage:
--   1. Enable binlog: SET GLOBAL binlog_format = 'ROW';
--   2. Enable GTID: SET GLOBAL gtid_mode = ON;
--   3. Execute these statements in order
--   4. Parse the generated binlog to verify your parser
-- ============================================================================

-- Setup: Enable required binlog settings
SET GLOBAL binlog_format = 'ROW';
SET GLOBAL binlog_row_image = 'FULL';
SET GLOBAL binlog_rows_query_log_events = ON;
SET SESSION sql_log_bin = 1;

-- Create test database
DROP DATABASE IF EXISTS binlog_test;
CREATE DATABASE binlog_test;
USE binlog_test;

-- ============================================================================
-- Test 1: FORMAT_DESCRIPTION_EVENT (15)
-- ============================================================================
-- Generated automatically when opening a new binlog file
FLUSH LOGS;

-- ============================================================================
-- Test 2: QUERY_EVENT (2)
-- ============================================================================
-- DDL statements generate QUERY_EVENT

-- Simple CREATE TABLE
CREATE TABLE test_query_event (
    id INT PRIMARY KEY AUTO_INCREMENT,
    name VARCHAR(100),
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB;

-- ALTER TABLE
ALTER TABLE test_query_event ADD COLUMN email VARCHAR(255);

-- CREATE INDEX
CREATE INDEX idx_name ON test_query_event(name);

-- DROP INDEX
DROP INDEX idx_name ON test_query_event;

-- ============================================================================
-- Test 3: STOP_EVENT (3)
-- ============================================================================
-- Generated when server shuts down normally
-- Cannot be triggered by SQL, requires: mysqladmin shutdown

-- ============================================================================
-- Test 4: ROTATE_EVENT (4)
-- ============================================================================
-- Generated when binlog file rotates
FLUSH LOGS;

-- ============================================================================
-- Test 5: INTVAR_EVENT (5)
-- ============================================================================
-- Generated for AUTO_INCREMENT values

CREATE TABLE test_intvar (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    data VARCHAR(50)
) ENGINE=InnoDB;

-- This INSERT will generate INTVAR_EVENT for auto_increment
START TRANSACTION;
INSERT INTO test_intvar (data) VALUES ('test1');
INSERT INTO test_intvar (data) VALUES ('test2');
COMMIT;

-- Test with LAST_INSERT_ID()
START TRANSACTION;
INSERT INTO test_intvar (data) VALUES ('test3');
SELECT LAST_INSERT_ID();
COMMIT;

-- ============================================================================
-- Test 6: RAND_EVENT (13)
-- ============================================================================
-- Generated when RAND() is used in statements

CREATE TABLE test_rand (
    id INT PRIMARY KEY AUTO_INCREMENT,
    random_value DOUBLE
) ENGINE=InnoDB;

-- Use RAND() to generate RAND_EVENT
START TRANSACTION;
INSERT INTO test_rand (random_value) VALUES (RAND());
INSERT INTO test_rand (random_value) VALUES (RAND() * 100);
COMMIT;

-- ============================================================================
-- Test 7: USER_VAR_EVENT (14)
-- ============================================================================
-- Generated when user variables are used

-- Set user variables
SET @my_var = 12345;
SET @my_string = 'Hello World';
SET @my_double = 3.14159;

CREATE TABLE test_user_var (
    id INT PRIMARY KEY AUTO_INCREMENT,
    var_value VARCHAR(100)
) ENGINE=InnoDB;

-- Use user variable in INSERT
START TRANSACTION;
INSERT INTO test_user_var (var_value) VALUES (@my_string);
COMMIT;

-- ============================================================================
-- Test 8: XID_EVENT (16)
-- ============================================================================
-- Generated at transaction commit (InnoDB)

CREATE TABLE test_xid (
    id INT PRIMARY KEY,
    data VARCHAR(50)
) ENGINE=InnoDB;

-- Simple transaction
START TRANSACTION;
INSERT INTO test_xid VALUES (1, 'first');
INSERT INTO test_xid VALUES (2, 'second');
COMMIT;  -- Generates XID_EVENT

-- Another transaction
BEGIN;
UPDATE test_xid SET data = 'updated' WHERE id = 1;
DELETE FROM test_xid WHERE id = 2;
COMMIT;  -- Generates XID_EVENT

-- ============================================================================
-- Test 9: APPEND_BLOCK_EVENT (9) & DELETE_FILE_EVENT (11)
-- Test 10: BEGIN_LOAD_QUERY_EVENT (17) & EXECUTE_LOAD_QUERY_EVENT (18)
-- ============================================================================
-- Generated by LOAD DATA INFILE

CREATE TABLE test_load_data (
    id INT,
    name VARCHAR(100),
    value DECIMAL(10,2)
) ENGINE=InnoDB;

-- Create test data file
-- Note: You need to create /tmp/test_data.csv with content like:
-- 1,Alice,100.50
-- 2,Bob,200.75
-- 3,Charlie,300.25

-- LOAD DATA generates: BEGIN_LOAD_QUERY_EVENT, APPEND_BLOCK_EVENT, EXECUTE_LOAD_QUERY_EVENT, DELETE_FILE_EVENT
LOAD DATA INFILE '/tmp/test_data.csv'
INTO TABLE test_load_data
FIELDS TERMINATED BY ','
LINES TERMINATED BY '\n'
(id, name, value);

-- ============================================================================
-- Test 11: TABLE_MAP_EVENT (19)
-- ============================================================================
-- Generated before any row event (WRITE/UPDATE/DELETE)

CREATE TABLE test_table_map (
    id INT PRIMARY KEY,
    col_tiny TINYINT,
    col_small SMALLINT,
    col_medium MEDIUMINT,
    col_int INT,
    col_bigint BIGINT,
    col_float FLOAT,
    col_double DOUBLE,
    col_decimal DECIMAL(10,2),
    col_date DATE,
    col_time TIME,
    col_datetime DATETIME,
    col_timestamp TIMESTAMP,
    col_year YEAR,
    col_varchar VARCHAR(255),
    col_char CHAR(50),
    col_text TEXT,
    col_blob BLOB,
    col_enum ENUM('A', 'B', 'C'),
    col_set SET('X', 'Y', 'Z'),
    col_json JSON,
    col_bit BIT(8)
) ENGINE=InnoDB;

-- ============================================================================
-- Test 12: WRITE_ROWS_EVENT (30)
-- ============================================================================
-- Generated by INSERT statements

START TRANSACTION;
INSERT INTO test_table_map VALUES (
    1,                          -- id
    127,                        -- col_tiny
    32767,                      -- col_small
    8388607,                    -- col_medium
    2147483647,                 -- col_int
    9223372036854775807,        -- col_bigint
    3.14,                       -- col_float
    2.718281828,                -- col_double
    12345.67,                   -- col_decimal
    '2025-01-15',               -- col_date
    '14:30:00',                 -- col_time
    '2025-01-15 14:30:00',      -- col_datetime
    '2025-01-15 14:30:00',      -- col_timestamp
    2025,                       -- col_year
    'varchar data',             -- col_varchar
    'char data',                -- col_char
    'text data',                -- col_text
    'blob data',                -- col_blob
    'A',                        -- col_enum
    'X,Y',                      -- col_set
    '{"key": "value"}',         -- col_json
    b'10101010'                 -- col_bit
);
COMMIT;

-- Test NULL values
START TRANSACTION;
INSERT INTO test_table_map (id, col_int) VALUES (2, 42);
COMMIT;

-- Bulk insert
START TRANSACTION;
INSERT INTO test_table_map (id, col_int, col_varchar) VALUES
    (3, 100, 'row3'),
    (4, 200, 'row4'),
    (5, 300, 'row5');
COMMIT;

-- ============================================================================
-- Test 13: UPDATE_ROWS_EVENT (31)
-- ============================================================================
-- Generated by UPDATE statements

-- Simple update
START TRANSACTION;
UPDATE test_table_map SET col_varchar = 'updated' WHERE id = 1;
COMMIT;

-- Multiple column update
START TRANSACTION;
UPDATE test_table_map 
SET col_int = 999, 
    col_double = 9.99,
    col_varchar = 'multi-update'
WHERE id = 2;
COMMIT;

-- Bulk update
START TRANSACTION;
UPDATE test_table_map SET col_int = col_int + 1000 WHERE id IN (3, 4, 5);
COMMIT;

-- ============================================================================
-- Test 14: DELETE_ROWS_EVENT (32)
-- ============================================================================
-- Generated by DELETE statements

-- Single delete
START TRANSACTION;
DELETE FROM test_table_map WHERE id = 5;
COMMIT;

-- Multiple delete
START TRANSACTION;
DELETE FROM test_table_map WHERE id IN (3, 4);
COMMIT;

-- ============================================================================
-- Test 15: ROWS_QUERY_LOG_EVENT (29)
-- ============================================================================
-- Generated when binlog_rows_query_log_events=ON (already set above)

CREATE TABLE test_rows_query (
    id INT PRIMARY KEY,
    description VARCHAR(255)
) ENGINE=InnoDB;

START TRANSACTION;
-- This comment and query will be logged in ROWS_QUERY_LOG_EVENT
INSERT INTO test_rows_query VALUES (1, 'This query is logged');
COMMIT;

START TRANSACTION;
/* Complex query with multiple conditions */
UPDATE test_rows_query 
SET description = 'Updated with complex query'
WHERE id = 1 AND description LIKE '%logged%';
COMMIT;

-- ============================================================================
-- Test 16: GTID_LOG_EVENT (33) & ANONYMOUS_GTID_LOG_EVENT (34)
-- ============================================================================
-- Generated automatically when GTID is enabled

-- With GTID enabled (gtid_mode=ON)
SET GTID_NEXT = 'AUTOMATIC';

CREATE TABLE test_gtid (
    id INT PRIMARY KEY,
    data VARCHAR(50)
) ENGINE=InnoDB;

START TRANSACTION;
INSERT INTO test_gtid VALUES (1, 'gtid_test');
COMMIT;

-- Anonymous GTID (when gtid_mode=OFF)
-- SET GTID_NEXT = 'ANONYMOUS';  -- Generates ANONYMOUS_GTID_LOG_EVENT
-- START TRANSACTION;
-- INSERT INTO test_gtid VALUES (2, 'anonymous_gtid');
-- COMMIT;

-- ============================================================================
-- Test 17: PREVIOUS_GTIDS_LOG_EVENT (35)
-- ============================================================================
-- Generated automatically at the start of each binlog file (after FORMAT_DESCRIPTION_EVENT)
-- when GTIDs are enabled
FLUSH LOGS;

-- ============================================================================
-- Test 18: XA_PREPARE_LOG_EVENT (38)
-- ============================================================================
-- Generated by prepared XA transactions

CREATE TABLE test_xa (
    id INT PRIMARY KEY,
    data VARCHAR(50)
) ENGINE=InnoDB;

-- XA transaction
XA START 'xa_test_001';
INSERT INTO test_xa VALUES (1, 'xa_data');
XA END 'xa_test_001';
XA PREPARE 'xa_test_001';  -- Generates XA_PREPARE_LOG_EVENT
XA COMMIT 'xa_test_001';

-- Another XA transaction with rollback
XA START 'xa_test_002';
INSERT INTO test_xa VALUES (2, 'xa_data2');
XA END 'xa_test_002';
XA PREPARE 'xa_test_002';  -- Generates XA_PREPARE_LOG_EVENT
XA ROLLBACK 'xa_test_002';

-- ============================================================================
-- Test 19: PARTIAL_UPDATE_ROWS_EVENT (39)
-- ============================================================================
-- Generated for JSON partial updates

CREATE TABLE test_partial_json (
    id INT PRIMARY KEY,
    json_col JSON
) ENGINE=InnoDB;

-- Insert JSON document
START TRANSACTION;
INSERT INTO test_partial_json VALUES (
    1,
    '{"name": "John", "age": 30, "address": {"city": "NYC", "zip": "10001"}, "tags": ["a", "b", "c"]}'
);
COMMIT;

-- Partial JSON update (generates PARTIAL_UPDATE_ROWS_EVENT in MySQL 8.0+)
START TRANSACTION;
UPDATE test_partial_json 
SET json_col = JSON_SET(json_col, '$.age', 31) 
WHERE id = 1;
COMMIT;

START TRANSACTION;
UPDATE test_partial_json 
SET json_col = JSON_SET(json_col, '$.address.city', 'Los Angeles')
WHERE id = 1;
COMMIT;

START TRANSACTION;
UPDATE test_partial_json 
SET json_col = JSON_ARRAY_APPEND(json_col, '$.tags', 'd')
WHERE id = 1;
COMMIT;

-- ============================================================================
-- Test 20: TRANSACTION_PAYLOAD_EVENT (40)
-- ============================================================================
-- Generated when binlog_transaction_compression=ON

SET SESSION binlog_transaction_compression = ON;

CREATE TABLE test_compressed (
    id INT PRIMARY KEY,
    data VARCHAR(1000)
) ENGINE=InnoDB;

-- Large transaction that will be compressed
START TRANSACTION;
INSERT INTO test_compressed VALUES (1, REPEAT('A', 1000));
INSERT INTO test_compressed VALUES (2, REPEAT('B', 1000));
INSERT INTO test_compressed VALUES (3, REPEAT('C', 1000));
INSERT INTO test_compressed VALUES (4, REPEAT('D', 1000));
INSERT INTO test_compressed VALUES (5, REPEAT('E', 1000));
COMMIT;  -- Generates TRANSACTION_PAYLOAD_EVENT

-- Disable compression
SET SESSION binlog_transaction_compression = OFF;

-- ============================================================================
-- Test 21: INCIDENT_EVENT (26)
-- ============================================================================
-- Generated when something goes wrong on master
-- Cannot be easily triggered by SQL - requires internal server error
-- Usually occurs during replication errors or server crashes

-- ============================================================================
-- Test 22: HEARTBEAT_LOG_EVENT (27)
-- ============================================================================
-- Sent automatically by master to replica
-- Controlled by: SET GLOBAL slave_net_timeout = N;
-- Cannot be directly generated by SQL

-- ============================================================================
-- Test 23: TRANSACTION_CONTEXT_EVENT (36) & VIEW_CHANGE_EVENT (37)
-- ============================================================================
-- Generated by Group Replication
-- Requires Group Replication setup
-- See: https://dev.mysql.com/doc/refman/8.0/en/group-replication.html

-- ============================================================================
-- Test 24: GTID_TAGGED_LOG_EVENT (42)
-- ============================================================================
-- Generated when using tagged GTIDs (MySQL 8.3+)

-- Enable tagged GTIDs
-- SET GLOBAL gtid_mode = ON_WITH_TAGS;

CREATE TABLE test_gtid_tagged (
    id INT PRIMARY KEY,
    data VARCHAR(50)
) ENGINE=InnoDB;

-- Transaction with GTID tag
-- SET GTID_NEXT = 'aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa:1:tag1';
START TRANSACTION;
INSERT INTO test_gtid_tagged VALUES (1, 'tagged_gtid');
COMMIT;

SET GTID_NEXT = 'AUTOMATIC';

-- ============================================================================
-- Test 25: Complex Scenarios
-- ============================================================================

-- Multi-table transaction
CREATE TABLE test_orders (
    order_id INT PRIMARY KEY AUTO_INCREMENT,
    customer_name VARCHAR(100),
    order_date TIMESTAMP DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB;

CREATE TABLE test_order_items (
    item_id INT PRIMARY KEY AUTO_INCREMENT,
    order_id INT,
    product_name VARCHAR(100),
    quantity INT,
    price DECIMAL(10,2),
    FOREIGN KEY (order_id) REFERENCES test_orders(order_id)
) ENGINE=InnoDB;

-- Transaction with multiple tables
START TRANSACTION;
INSERT INTO test_orders (customer_name) VALUES ('Alice');
SET @order_id = LAST_INSERT_ID();
INSERT INTO test_order_items (order_id, product_name, quantity, price) 
VALUES (@order_id, 'Widget', 5, 19.99);
INSERT INTO test_order_items (order_id, product_name, quantity, price) 
VALUES (@order_id, 'Gadget', 3, 29.99);
COMMIT;

-- ============================================================================
-- Test 26: All MySQL Data Types
-- ============================================================================

CREATE TABLE test_all_types (
    -- Numeric types
    col_tinyint TINYINT,
    col_tinyint_unsigned TINYINT UNSIGNED,
    col_smallint SMALLINT,
    col_smallint_unsigned SMALLINT UNSIGNED,
    col_mediumint MEDIUMINT,
    col_mediumint_unsigned MEDIUMINT UNSIGNED,
    col_int INT,
    col_int_unsigned INT UNSIGNED,
    col_bigint BIGINT,
    col_bigint_unsigned BIGINT UNSIGNED,
    
    -- Floating point
    col_float FLOAT,
    col_double DOUBLE,
    col_decimal DECIMAL(10,2),
    col_decimal_large DECIMAL(65,30),
    
    -- Date and time
    col_date DATE,
    col_time TIME,
    col_time_fsp TIME(6),
    col_datetime DATETIME,
    col_datetime_fsp DATETIME(6),
    col_timestamp TIMESTAMP,
    col_timestamp_fsp TIMESTAMP(6),
    col_year YEAR,
    
    -- String types
    col_char CHAR(10),
    col_varchar VARCHAR(255),
    col_binary BINARY(10),
    col_varbinary VARBINARY(255),
    col_tinytext TINYTEXT,
    col_text TEXT,
    col_mediumtext MEDIUMTEXT,
    col_longtext LONGTEXT,
    col_tinyblob TINYBLOB,
    col_blob BLOB,
    col_mediumblob MEDIUMBLOB,
    col_longblob LONGBLOB,
    
    -- Enum and Set
    col_enum ENUM('small', 'medium', 'large'),
    col_set SET('red', 'green', 'blue', 'yellow'),
    
    -- JSON
    col_json JSON,
    
    -- Geometry (spatial)
    col_geometry GEOMETRY,
    col_point POINT,
    col_linestring LINESTRING,
    col_polygon POLYGON,
    
    -- Bit
    col_bit BIT(64),
    
    id INT PRIMARY KEY AUTO_INCREMENT
) ENGINE=InnoDB;

-- Insert with all types
START TRANSACTION;
INSERT INTO test_all_types (
    col_tinyint, col_tinyint_unsigned,
    col_smallint, col_smallint_unsigned,
    col_mediumint, col_mediumint_unsigned,
    col_int, col_int_unsigned,
    col_bigint, col_bigint_unsigned,
    col_float, col_double, col_decimal, col_decimal_large,
    col_date, col_time, col_time_fsp,
    col_datetime, col_datetime_fsp,
    col_timestamp, col_timestamp_fsp,
    col_year,
    col_char, col_varchar, col_binary, col_varbinary,
    col_tinytext, col_text, col_mediumtext, col_longtext,
    col_tinyblob, col_blob, col_mediumblob, col_longblob,
    col_enum, col_set,
    col_json,
    col_geometry, col_point,
    col_bit
) VALUES (
    -128, 255,
    -32768, 65535,
    -8388608, 16777215,
    -2147483648, 4294967295,
    -9223372036854775808, 18446744073709551615,
    3.14159, 2.718281828459045, 12345.67, 12345678901234567890.123456789012345678,
    '2025-01-15', '14:30:45', '14:30:45.123456',
    '2025-01-15 14:30:45', '2025-01-15 14:30:45.123456',
    '2025-01-15 14:30:45', '2025-01-15 14:30:45.123456',
    2025,
    'char', 'varchar test', 'binary\0\0\0\0', 'varbinary test',
    'tiny text', 'regular text', 'medium text content', 'long text content',
    'tiny blob', 'regular blob', 'medium blob content', 'long blob content',
    'medium', 'red,blue',
    '{"key1": "value1", "key2": 123, "nested": {"a": 1}}',
    ST_GeomFromText('POINT(1 1)'), ST_GeomFromText('POINT(10 10)'),
    b'1010101010101010101010101010101010101010101010101010101010101010'
);
COMMIT;

-- Update all types
START TRANSACTION;
UPDATE test_all_types SET
    col_tinyint = 100,
    col_varchar = 'updated',
    col_json = JSON_SET(col_json, '$.key1', 'updated_value'),
    col_enum = 'large',
    col_set = 'green,yellow'
WHERE id = 1;
COMMIT;

-- ============================================================================
-- Test 27: Edge Cases
-- ============================================================================

CREATE TABLE test_edge_cases (
    id INT PRIMARY KEY AUTO_INCREMENT,
    empty_string VARCHAR(100),
    null_value INT,
    zero_value INT,
    negative_value INT,
    max_value BIGINT,
    min_value BIGINT,
    empty_text TEXT,
    empty_blob BLOB,
    unicode_text VARCHAR(255)
) ENGINE=InnoDB;

START TRANSACTION;
INSERT INTO test_edge_cases VALUES (
    NULL,                           -- id (auto_increment)
    '',                             -- empty_string
    NULL,                           -- null_value
    0,                              -- zero_value
    -999999,                        -- negative_value
    9223372036854775807,            -- max_value
    -9223372036854775808,           -- min_value
    '',                             -- empty_text
    '',                             -- empty_blob
    '你好世界 🌍 مرحبا العالم'      -- unicode_text
);
COMMIT;

-- ============================================================================
-- Cleanup and Summary
-- ============================================================================

-- Generate one final ROTATE_EVENT
FLUSH LOGS;

-- Show generated binlog files
SHOW BINARY LOGS;

-- Optional: Show binlog events
-- SHOW BINLOG EVENTS IN 'mysql-bin.000001';

-- Note: To examine the generated binlog:
-- mysqlbinlog --verbose --base64-output=DECODE-ROWS mysql-bin.000001

SELECT 'Binlog test SQL execution completed!' AS Status;
SELECT 'All major binlog event types should now be present in the binlog' AS Info;
SELECT 'Use mysqlbinlog or your parser to verify' AS NextStep;
