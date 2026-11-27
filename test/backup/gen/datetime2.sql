-- Spec 093 T8c: datetime2(7) in a backup, read as TYPE mssql reads it -- TIMESTAMP_NS by default, a value
-- outside 1677-09-21 .. 2262-04-11 NULL with a WARNING, or under datetime2_7_as 'timestamp' TIMESTAMP over
-- 0001-9999 with the 100 ns digit truncated. Values at both TIMESTAMP_NS edges and at datetime2's, a 100 ns
-- digit on each; the other scales beside them (read as TIMESTAMP whatever the option); a system-versioned table
-- (its period end is 9999-12-31 23:59:59.9999999, the value that motivated D8); generated values over the
-- whole range; ROW- and PAGE-compressed tables and a heap; a datetime2(7) clustered key. Then
-- datetime2_changes.sql's changes and a differential (datetime2_over). Run by default, so the oracle lane
-- compares it with a RESTORE through TYPE mssql on every version, also with datetime2_7_as 'timestamp' on both
-- sides (compare_rows.sh DATETIME2_TIMESTAMP); 2025's committed under test/data/backup/datetime2 (made by its
-- generate.sh). Compressed: small. Synthetic values only (T4).
SET NOCOUNT ON;
GO
IF DB_ID('bak_datetime2') IS NOT NULL DROP DATABASE bak_datetime2;
GO
CREATE DATABASE bak_datetime2;
GO
USE bak_datetime2;
GO
-- 1. The edges. TIMESTAMP_NS holds int64 nanoseconds from 1970: 1677-09-21 00:12:43.145224192 to
-- 2262-04-11 23:47:16.854775807. Each edge with a value just inside and just outside (a 100 ns step). (The
-- TDS path's kernel, which both paths share, checks the lower edge by whole days: all of 1677-09-21 reads NULL,
-- its inside value too; the upper edge to the tick.)
CREATE TABLE dbo.d_edges (id int PRIMARY KEY, d7 datetime2(7) NULL, d3 datetime2(3) NULL, d0 datetime2(0) NULL);
INSERT INTO dbo.d_edges VALUES
	(1, '0001-01-01 00:00:00.0000000', '0001-01-01 00:00:00.000', '0001-01-01 00:00:00'),
	(2, '0001-01-01 00:00:00.0000001', '0001-01-01 00:00:00.001', '0001-01-01 00:00:01'),
	(3, '1677-09-21 00:12:43.1452241', '1677-09-21 00:12:43.145', '1677-09-21 00:12:43'),
	(4, '1677-09-21 00:12:43.1452242', '1677-09-21 00:12:43.146', '1677-09-21 00:12:44'),
	(5, '1969-12-31 23:59:59.9999999', '1969-12-31 23:59:59.999', '1969-12-31 23:59:59'),
	(6, '1970-01-01 00:00:00.0000000', '1970-01-01 00:00:00.000', '1970-01-01 00:00:00'),
	(7, '2000-02-29 12:34:56.1234567', '2000-02-29 12:34:56.123', '2000-02-29 12:34:56'),
	(8, '2262-04-11 23:47:16.8547758', '2262-04-11 23:47:16.854', '2262-04-11 23:47:16'),
	(9, '2262-04-11 23:47:16.8547759', '2262-04-11 23:47:16.855', '2262-04-11 23:47:17'),
	(10, '9999-12-31 23:59:59.9999999', '9999-12-31 23:59:59.999', '9999-12-31 23:59:59'),
	(11, NULL, NULL, NULL),
	(12, '1500-06-15 08:00:00.0000005', '1500-06-15 08:00:00.500', '1500-06-15 08:00:00'),
	(13, '3000-01-01 00:00:00.9999999', '3000-01-01 00:00:00.999', '3000-01-01 00:00:00');
GO
-- 2. A system-versioned table: every current row's period end is 9999-12-31 23:59:59.9999999, NULL as
-- TIMESTAMP_NS; the history table holds the closed periods.
CREATE TABLE dbo.d_temporal (
	id int PRIMARY KEY,
	name varchar(20) NOT NULL,
	valid_from datetime2(7) GENERATED ALWAYS AS ROW START NOT NULL,
	valid_to datetime2(7) GENERATED ALWAYS AS ROW END NOT NULL,
	PERIOD FOR SYSTEM_TIME (valid_from, valid_to))
	WITH (SYSTEM_VERSIONING = ON (HISTORY_TABLE = dbo.d_temporal_history));
WITH n AS (SELECT TOP (300) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.d_temporal (id, name) SELECT i, CONCAT('n', i) FROM n;
UPDATE dbo.d_temporal SET name = CONCAT('m', id) WHERE id % 3 = 0;
DELETE FROM dbo.d_temporal WHERE id % 17 = 0;
GO
-- 3. Generated values over the whole range, a 100 ns digit on most, and NULLs.
CREATE TABLE dbo.d_mixed (id int PRIMARY KEY, d7 datetime2(7) NULL, d5 datetime2(5) NULL);
WITH n AS (SELECT TOP (3000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.d_mixed SELECT i,
	CASE WHEN i % 11 = 0 THEN NULL
		ELSE DATEADD(nanosecond, (i * 7919 % 10000000) * 100,
			DATEADD(second, i * 4111 % 86400, DATEADD(day, i * 1217 % 3652058, CAST('0001-01-01' AS datetime2(7))))) END,
	DATEADD(day, i * 911 % 3652058, CAST('0001-01-01 12:00:00.12345' AS datetime2(5)))
FROM n;
GO
-- 4. A heap, and ROW and PAGE compression (spec 086's CD form of a datetime2, put back as the record holds it);
-- PAGE with repeated values for the dictionary and the anchor.
CREATE TABLE dbo.d_heap (id int NOT NULL, d7 datetime2(7) NULL);
INSERT INTO dbo.d_heap SELECT id, d7 FROM dbo.d_mixed WHERE id <= 1000;
CREATE TABLE dbo.d_row (id int PRIMARY KEY, d7 datetime2(7) NULL) WITH (DATA_COMPRESSION = ROW);
INSERT INTO dbo.d_row SELECT id, d7 FROM dbo.d_edges;
INSERT INTO dbo.d_row SELECT 100 + id, d7 FROM dbo.d_mixed WHERE id <= 1500;
CREATE TABLE dbo.d_page (id int PRIMARY KEY, d7 datetime2(7) NULL) WITH (DATA_COMPRESSION = PAGE);
INSERT INTO dbo.d_page SELECT id, d7 FROM dbo.d_edges;
WITH n AS (SELECT TOP (2000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.d_page SELECT 100 + i, CASE i % 3 WHEN 0 THEN CAST('9999-12-31 23:59:59.9999999' AS datetime2(7))
	WHEN 1 THEN CAST('2024-01-01 00:00:00.0000001' AS datetime2(7)) ELSE CAST('0001-01-01' AS datetime2(7)) END FROM n;
ALTER TABLE dbo.d_page REBUILD WITH (DATA_COMPRESSION = PAGE);
GO
-- 5. A datetime2(7) clustered key, not unique: the uniquifier beside it, keys outside TIMESTAMP_NS's range.
CREATE TABLE dbo.d_key (k datetime2(7) NOT NULL, n int NOT NULL);
CREATE CLUSTERED INDEX cx_d_key ON dbo.d_key (k);
INSERT INTO dbo.d_key VALUES ('0001-01-01', 1), ('0001-01-01', 2), ('2000-01-01 00:00:00.0000001', 3),
	('9999-12-31 23:59:59.9999999', 4), ('9999-12-31 23:59:59.9999999', 5), ('1677-09-21 00:12:43.1452242', 6);
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_datetime2 TO DISK = '$(DIR)/datetime2_full.bak' WITH FORMAT, INIT, CHECKSUM, COMPRESSION, NAME = 'datetime2_full';
GO
-- 6 is datetime2_changes.sql's, which generate.sh runs after the golden catalog of datetime2_full.
