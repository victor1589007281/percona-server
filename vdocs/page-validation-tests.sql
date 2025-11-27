-- ============================================================================
-- InnoDB Page Parser Validation Test SQL
-- ============================================================================
-- This file contains SQL statements to generate various types of InnoDB pages
-- for testing and validating page parser implementations.
--
-- Usage:
--   1. Execute these statements in order
--   2. Use ibd2sdi or innochecksum to examine generated .ibd files
--   3. Parse the .ibd files with your page parser
-- ============================================================================

DROP DATABASE IF EXISTS page_test;
CREATE DATABASE page_test;
USE page_test;

-- ============================================================================
-- Test 1: FIL_PAGE_TYPE_FSP_HDR (8) - File Space Header
-- ============================================================================
-- Page 0 of every tablespace is FSP_HDR

CREATE TABLE test_fsp_hdr (
    id INT PRIMARY KEY
) ENGINE=InnoDB;

-- Page 0 of test_fsp_hdr.ibd is FSP_HDR

-- ============================================================================
-- Test 2: FIL_PAGE_INDEX (17855) - B-Tree Index Pages
-- ============================================================================
-- Most common page type

CREATE TABLE test_index_pages (
    id INT PRIMARY KEY AUTO_INCREMENT,
    col1 VARCHAR(100),
    col2 INT,
    col3 VARCHAR(255),
    INDEX idx_col1 (col1),
    INDEX idx_col2 (col2)
) ENGINE=InnoDB;

-- Insert data to create multiple index pages
INSERT INTO test_index_pages (col1, col2, col3)
SELECT 
    CONCAT('data_', i),
    i,
    REPEAT('X', 200)
FROM (
    SELECT a.i + b.i*10 + c.i*100 AS i
    FROM 
        (SELECT 0 AS i UNION SELECT 1 UNION SELECT 2 UNION SELECT 3 UNION SELECT 4 
         UNION SELECT 5 UNION SELECT 6 UNION SELECT 7 UNION SELECT 8 UNION SELECT 9) a,
        (SELECT 0 AS i UNION SELECT 1 UNION SELECT 2 UNION SELECT 3 UNION SELECT 4 
         UNION SELECT 5 UNION SELECT 6 UNION SELECT 7 UNION SELECT 8 UNION SELECT 9) b,
        (SELECT 0 AS i UNION SELECT 1 UNION SELECT 2 UNION SELECT 3 UNION SELECT 4) c
) numbers;

-- ============================================================================
-- Test 3: FIL_PAGE_RTREE (17854) - R-Tree Spatial Index Pages
-- ============================================================================

CREATE TABLE test_spatial (
    id INT PRIMARY KEY AUTO_INCREMENT,
    name VARCHAR(100),
    location POINT NOT NULL SRID 0,
    area POLYGON NOT NULL SRID 0,
    SPATIAL INDEX idx_location (location),
    SPATIAL INDEX idx_area (area)
) ENGINE=InnoDB;

-- Insert spatial data
INSERT INTO test_spatial (name, location, area) VALUES
    ('Point1', ST_GeomFromText('POINT(0 0)'), ST_GeomFromText('POLYGON((0 0,0 10,10 10,10 0,0 0))')),
    ('Point2', ST_GeomFromText('POINT(5 5)'), ST_GeomFromText('POLYGON((5 5,5 15,15 15,15 5,5 5))')),
    ('Point3', ST_GeomFromText('POINT(10 10)'), ST_GeomFromText('POLYGON((10 10,10 20,20 20,20 10,10 10))')),
    ('Point4', ST_GeomFromText('POINT(-5 -5)'), ST_GeomFromText('POLYGON((-5 -5,-5 5,5 5,5 -5,-5 -5))'));

-- Add more points to create multiple R-tree pages
INSERT INTO test_spatial (name, location, area)
SELECT 
    CONCAT('Generated_', i),
    ST_GeomFromText(CONCAT('POINT(', i % 100, ' ', i % 50, ')')),
    ST_GeomFromText(CONCAT('POLYGON((', i%100, ' ', i%50, ',', 
                          i%100, ' ', (i%50)+10, ',',
                          (i%100)+10, ' ', (i%50)+10, ',',
                          (i%100)+10, ' ', i%50, ',',
                          i%100, ' ', i%50, '))'))
FROM (
    SELECT a.i + b.i*10 + c.i*100 AS i
    FROM 
        (SELECT 0 AS i UNION SELECT 1 UNION SELECT 2 UNION SELECT 3 UNION SELECT 4) a,
        (SELECT 0 AS i UNION SELECT 1 UNION SELECT 2 UNION SELECT 3 UNION SELECT 4) b,
        (SELECT 0 AS i UNION SELECT 1 UNION SELECT 2) c
) numbers;

-- ============================================================================
-- Test 4: FIL_PAGE_UNDO_LOG (2) - Undo Log Pages
-- ============================================================================
-- Undo pages are created automatically with transactions

CREATE TABLE test_undo (
    id INT PRIMARY KEY,
    data VARCHAR(500)
) ENGINE=InnoDB;

-- Generate undo log activity
START TRANSACTION;
INSERT INTO test_undo SELECT i, REPEAT('A', 400) FROM
    (SELECT 1 AS i UNION SELECT 2 UNION SELECT 3 UNION SELECT 4 UNION SELECT 5) t;
COMMIT;

START TRANSACTION;
UPDATE test_undo SET data = REPEAT('B', 400);
COMMIT;

START TRANSACTION;
DELETE FROM test_undo WHERE id <= 2;
COMMIT;

-- ============================================================================
-- Test 5: FIL_PAGE_INODE (3) - Index Node Pages
-- ============================================================================
-- INODE pages are created automatically for file segment management
-- Creating multiple indexes will create INODE pages

CREATE TABLE test_many_indexes (
    id INT PRIMARY KEY,
    col1 INT, col2 INT, col3 INT, col4 INT, col5 INT,
    col6 INT, col7 INT, col8 INT, col9 INT, col10 INT,
    INDEX idx1 (col1), INDEX idx2 (col2), INDEX idx3 (col3),
    INDEX idx4 (col4), INDEX idx5 (col5), INDEX idx6 (col6),
    INDEX idx7 (col7), INDEX idx8 (col8), INDEX idx9 (col9),
    INDEX idx10 (col10)
) ENGINE=InnoDB;

INSERT INTO test_many_indexes
SELECT i, i, i, i, i, i, i, i, i, i, i FROM
    (SELECT a.i + b.i*10 AS i FROM 
        (SELECT 0 AS i UNION SELECT 1 UNION SELECT 2 UNION SELECT 3 UNION SELECT 4) a,
        (SELECT 0 AS i UNION SELECT 1 UNION SELECT 2 UNION SELECT 3 UNION SELECT 4) b
    ) numbers;

-- ============================================================================
-- Test 6: FIL_PAGE_TYPE_BLOB (10) - Uncompressed BLOB Pages
-- ============================================================================
-- BLOB pages for externally stored data

CREATE TABLE test_blob (
    id INT PRIMARY KEY,
    small_text TEXT,
    large_text LONGTEXT,
    blob_data BLOB,
    large_blob LONGBLOB
) ENGINE=InnoDB ROW_FORMAT=DYNAMIC;

-- Insert large data to force external storage
INSERT INTO test_blob VALUES (
    1,
    REPEAT('Small ', 100),
    REPEAT('This is a very long text that will be stored externally. ', 1000),
    REPEAT(CHAR(65), 50000),
    REPEAT(CHAR(66), 100000)
);

INSERT INTO test_blob VALUES (
    2,
    'Another small text',
    REPEAT('More long text data that exceeds the inline storage threshold. ', 2000),
    REPEAT(CHAR(67), 75000),
    REPEAT(CHAR(68), 150000)
);

-- ============================================================================
-- Test 7: FIL_PAGE_TYPE_ZBLOB (11-12) - Compressed BLOB Pages
-- ============================================================================

CREATE TABLE test_compressed_blob (
    id INT PRIMARY KEY,
    large_text LONGTEXT,
    large_blob LONGBLOB
) ENGINE=InnoDB ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8;

-- Insert large data (will be compressed)
INSERT INTO test_compressed_blob VALUES (
    1,
    REPEAT('Compressed text data that will use ZBLOB pages. ', 2000),
    REPEAT(CHAR(70), 100000)
);

INSERT INTO test_compressed_blob VALUES (
    2,
    REPEAT('More compressed data with lots of repetition to test compression. ', 3000),
    REPEAT(CHAR(71), 200000)
);

-- ============================================================================
-- Test 8: FIL_PAGE_COMPRESSED (14) - Transparent Page Compression
-- ============================================================================
-- Note: Requires filesystem support (hole punching)

CREATE TABLE test_page_compressed (
    id INT PRIMARY KEY AUTO_INCREMENT,
    data VARCHAR(1000)
) ENGINE=InnoDB COMPRESSION="zlib";

INSERT INTO test_page_compressed (data)
SELECT REPEAT('PageCompressed', 70) FROM
    (SELECT a.i + b.i*10 AS i FROM 
        (SELECT 0 AS i UNION SELECT 1 UNION SELECT 2 UNION SELECT 3 UNION SELECT 4) a,
        (SELECT 0 AS i UNION SELECT 1 UNION SELECT 2 UNION SELECT 3 UNION SELECT 4) b
    ) numbers;

-- ============================================================================
-- Test 9: FIL_PAGE_ENCRYPTED (15) - Encrypted Pages
-- ============================================================================
-- Requires keyring plugin

-- CREATE TABLE test_encrypted (
--     id INT PRIMARY KEY,
--     sensitive_data VARCHAR(500)
-- ) ENGINE=InnoDB ENCRYPTION='Y';
-- 
-- INSERT INTO test_encrypted SELECT i, REPEAT('Secret', 60) FROM
--     (SELECT 1 AS i UNION SELECT 2 UNION SELECT 3 UNION SELECT 4 UNION SELECT 5) t;

-- ============================================================================
-- Test 10: FIL_PAGE_TYPE_XDES (9) - Extent Descriptor Pages
-- ============================================================================
-- XDES pages appear every 16384 pages (256 MB for 16KB pages)
-- Create a large tablespace to generate XDES pages

CREATE TABLE test_large_tablespace (
    id INT PRIMARY KEY AUTO_INCREMENT,
    data VARCHAR(2000)
) ENGINE=InnoDB;

-- Insert enough data to span multiple extents
INSERT INTO test_large_tablespace (data)
SELECT REPEAT('LargeTablespace', 140) FROM
    (SELECT a.i + b.i*10 + c.i*100 AS i FROM 
        (SELECT 0 AS i UNION SELECT 1 UNION SELECT 2 UNION SELECT 3 UNION SELECT 4 
         UNION SELECT 5 UNION SELECT 6 UNION SELECT 7 UNION SELECT 8 UNION SELECT 9) a,
        (SELECT 0 AS i UNION SELECT 1 UNION SELECT 2 UNION SELECT 3 UNION SELECT 4 
         UNION SELECT 5 UNION SELECT 6 UNION SELECT 7 UNION SELECT 8 UNION SELECT 9) b,
        (SELECT 0 AS i UNION SELECT 1 UNION SELECT 2 UNION SELECT 3 UNION SELECT 4) c
    ) numbers;

-- ============================================================================
-- Test 11: FIL_PAGE_SDI (17853) / FIL_PAGE_SDI_BLOB (18-19)
-- ============================================================================
-- SDI pages store serialized dictionary information
-- Created automatically by MySQL 8.0+

CREATE TABLE test_sdi (
    id INT PRIMARY KEY,
    col1 VARCHAR(100),
    col2 INT,
    col3 DATE,
    col4 DECIMAL(10,2),
    UNIQUE KEY uk_col1 (col1),
    KEY idx_col2 (col2),
    KEY idx_col3 (col3)
) ENGINE=InnoDB;

INSERT INTO test_sdi SELECT i, CONCAT('SDI_', i), i*10, DATE_ADD('2025-01-01', INTERVAL i DAY), i*1.5
FROM (SELECT a.i + b.i*10 AS i FROM 
    (SELECT 0 AS i UNION SELECT 1 UNION SELECT 2 UNION SELECT 3 UNION SELECT 4) a,
    (SELECT 0 AS i UNION SELECT 1 UNION SELECT 2) b
) numbers;

-- ============================================================================
-- Test 12: FIL_PAGE_TYPE_LOB_* (22-29) - LOB Pages (MySQL 8.0+)
-- ============================================================================
-- New LOB format for large objects

CREATE TABLE test_lob (
    id INT PRIMARY KEY,
    lob_text LONGTEXT,
    lob_blob LONGBLOB
) ENGINE=InnoDB ROW_FORMAT=DYNAMIC;

-- Insert very large LOBs to use new LOB pages
INSERT INTO test_lob VALUES (
    1,
    REPEAT('LOB text data with new LOB page format. ', 5000),
    REPEAT(CHAR(80), 200000)
);

INSERT INTO test_lob VALUES (
    2,
    REPEAT('More LOB data to create multiple LOB pages. ', 10000),
    REPEAT(CHAR(81), 500000)
);

-- ============================================================================
-- Test 13: Multiple Page Sizes
-- ============================================================================

-- 4KB page size
CREATE TABLE test_page_4k (
    id INT PRIMARY KEY,
    data VARCHAR(500)
) ENGINE=InnoDB ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=4;

INSERT INTO test_page_4k SELECT i, REPEAT('4K', 160) FROM
    (SELECT 1 AS i UNION SELECT 2 UNION SELECT 3 UNION SELECT 4 UNION SELECT 5) t;

-- 8KB page size
CREATE TABLE test_page_8k (
    id INT PRIMARY KEY,
    data VARCHAR(1000)
) ENGINE=InnoDB ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8;

INSERT INTO test_page_8k SELECT i, REPEAT('8K', 320) FROM
    (SELECT 1 AS i UNION SELECT 2 UNION SELECT 3 UNION SELECT 4 UNION SELECT 5) t;

-- ============================================================================
-- Test 14: Compact vs Redundant vs Dynamic Row Formats
-- ============================================================================

-- Redundant (old format, deprecated)
-- CREATE TABLE test_redundant (
--     id INT PRIMARY KEY,
--     data VARCHAR(500)
-- ) ENGINE=InnoDB ROW_FORMAT=REDUNDANT;

-- Compact format
CREATE TABLE test_compact (
    id INT PRIMARY KEY,
    data VARCHAR(500)
) ENGINE=InnoDB ROW_FORMAT=COMPACT;

INSERT INTO test_compact SELECT i, REPEAT('Compact', 60) FROM
    (SELECT 1 AS i UNION SELECT 2 UNION SELECT 3 UNION SELECT 4 UNION SELECT 5) t;

-- Dynamic format (default in MySQL 5.7+)
CREATE TABLE test_dynamic (
    id INT PRIMARY KEY,
    data VARCHAR(500)
) ENGINE=InnoDB ROW_FORMAT=DYNAMIC;

INSERT INTO test_dynamic SELECT i, REPEAT('Dynamic', 60) FROM
    (SELECT 1 AS i UNION SELECT 2 UNION SELECT 3 UNION SELECT 4 UNION SELECT 5) t;

-- ============================================================================
-- Test 15: Page Fragmentation
-- ============================================================================

CREATE TABLE test_fragmentation (
    id INT PRIMARY KEY,
    data VARCHAR(500)
) ENGINE=InnoDB;

-- Insert sequential data
INSERT INTO test_fragmentation SELECT i, REPEAT('X', 400) FROM
    (SELECT a.i + b.i*10 AS i FROM 
        (SELECT 0 AS i UNION SELECT 1 UNION SELECT 2 UNION SELECT 3 UNION SELECT 4) a,
        (SELECT 0 AS i UNION SELECT 1 UNION SELECT 2 UNION SELECT 3 UNION SELECT 4) b
    ) numbers;

-- Delete every other record to fragment pages
DELETE FROM test_fragmentation WHERE id % 2 = 0;

-- Check fragmentation
-- ANALYZE TABLE test_fragmentation;

-- ============================================================================
-- Test 16: B-Tree Splits and Merges
-- ============================================================================

CREATE TABLE test_btree_splits (
    id INT PRIMARY KEY AUTO_INCREMENT,
    data VARCHAR(1000)
) ENGINE=InnoDB;

-- Insert to cause page splits
INSERT INTO test_btree_splits (data) SELECT REPEAT('Split', 200) FROM
    (SELECT a.i + b.i*10 AS i FROM 
        (SELECT 0 AS i UNION SELECT 1 UNION SELECT 2 UNION SELECT 3 UNION SELECT 4) a,
        (SELECT 0 AS i UNION SELECT 1 UNION SELECT 2) b
    ) numbers;

-- Insert in middle to cause more splits
INSERT INTO test_btree_splits (id, data) VALUES (500, REPEAT('Middle', 200));
INSERT INTO test_btree_splits (id, data) VALUES (250, REPEAT('Quarter', 200));

-- ============================================================================
-- Test 17: Secondary Index Pages
-- ============================================================================

CREATE TABLE test_secondary_indexes (
    id INT PRIMARY KEY AUTO_INCREMENT,
    email VARCHAR(100) UNIQUE,
    name VARCHAR(100),
    age INT,
    city VARCHAR(50),
    INDEX idx_name (name),
    INDEX idx_age (age),
    INDEX idx_city (city),
    INDEX idx_name_age (name, age),
    INDEX idx_city_age (city, age)
) ENGINE=InnoDB;

INSERT INTO test_secondary_indexes (email, name, age, city)
SELECT 
    CONCAT('user', i, '@example.com'),
    CONCAT('User', i),
    20 + (i % 50),
    CONCAT('City', (i % 10))
FROM (
    SELECT a.i + b.i*10 AS i FROM 
        (SELECT 0 AS i UNION SELECT 1 UNION SELECT 2 UNION SELECT 3 UNION SELECT 4 
         UNION SELECT 5 UNION SELECT 6 UNION SELECT 7 UNION SELECT 8 UNION SELECT 9) a,
        (SELECT 0 AS i UNION SELECT 1 UNION SELECT 2 UNION SELECT 3 UNION SELECT 4) b
) numbers;

-- ============================================================================
-- Test 18: Nullable Columns (Null Bitmap)
-- ============================================================================

CREATE TABLE test_nulls (
    id INT PRIMARY KEY,
    col1 VARCHAR(100),
    col2 INT,
    col3 DATE,
    col4 TEXT,
    col5 DECIMAL(10,2)
) ENGINE=InnoDB;

-- Insert with various null patterns
INSERT INTO test_nulls VALUES (1, 'data1', 100, '2025-01-01', 'text1', 10.5);
INSERT INTO test_nulls VALUES (2, NULL, 200, '2025-01-02', 'text2', 20.5);
INSERT INTO test_nulls VALUES (3, 'data3', NULL, '2025-01-03', 'text3', 30.5);
INSERT INTO test_nulls VALUES (4, 'data4', 400, NULL, 'text4', 40.5);
INSERT INTO test_nulls VALUES (5, 'data5', 500, '2025-01-05', NULL, 50.5);
INSERT INTO test_nulls VALUES (6, NULL, NULL, NULL, NULL, NULL);

-- ============================================================================
-- Cleanup and Summary
-- ============================================================================

SHOW TABLE STATUS FROM page_test;

SELECT 'Page test SQL execution completed!' AS Status;
SELECT 'Various page types should now be present in .ibd files' AS Info;
SELECT 'Use innochecksum, ibd2sdi, or your parser to examine pages' AS NextStep;

-- To examine pages, you can:
-- 1. innochecksum /path/to/page_test/*.ibd
-- 2. ibd2sdi /path/to/page_test/test_*.ibd
-- 3. hexdump -C /path/to/page_test/test_index_pages.ibd | head -100

-- Page locations (datadir):
-- - Default datadir: /var/lib/mysql/ (Linux) or /usr/local/mysql/data/ (Mac)
-- - Database directory: datadir/page_test/
-- - Table files: test_*.ibd

-- Note: To find datadir:
-- SELECT @@datadir;
