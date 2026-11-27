-- Spec 093 T8b: sql_variant in a backup, read by T1's kernel from the form a record stores it in (measured/S1d.md):
-- every base type at its edges (S1's values), strings under code-page, double-byte, UTF-8 (2019 on) and SC collations, a
-- mixed column of generated values; in every storage form -- a clustered table, a heap whose rows grow and are
-- forwarded, ROW- and PAGE-compressed tables, values pushed off row (row-overflow), a sql_variant clustered key
-- (the uniquifier beside it) -- then variant_changes.sql's changes and a differential (variant_over). Run by
-- default, so the oracle lane compares it with a RESTORE through TYPE mssql (which reads sql_variant as VARIANT,
-- T1) on every version; 2025's committed under test/data/backup/variant (made by its generate.sh). Compressed:
-- small. Synthetic values only (T4).
SET NOCOUNT ON;
GO
IF DB_ID('bak_variant') IS NOT NULL DROP DATABASE bak_variant;
GO
CREATE DATABASE bak_variant;
GO
USE bak_variant;
GO
-- 1. Every base type at its edges (S1's 50 values), each cast to sql_variant on its own: a multi-row VALUES
-- would give the column one type.
CREATE TABLE dbo.v_edges (id int PRIMARY KEY, v sql_variant NULL);
INSERT INTO dbo.v_edges VALUES
	(1, CAST(CAST(1 AS bit) AS sql_variant)), (2, CAST(CAST(0 AS bit) AS sql_variant)),
	(3, CAST(CAST(255 AS tinyint) AS sql_variant)), (4, CAST(CAST(-32768 AS smallint) AS sql_variant)),
	(5, CAST(CAST(-2147483648 AS int) AS sql_variant)), (6, CAST(CAST(-9223372036854775808 AS bigint) AS sql_variant)),
	(7, CAST(CAST(1.5 AS real) AS sql_variant)), (8, CAST(CAST(-1.25e300 AS float) AS sql_variant)),
	(9, CAST(CAST(0.5 AS decimal(1, 1)) AS sql_variant)), (10, CAST(CAST(-1234567.89 AS decimal(9, 2)) AS sql_variant)),
	(11, CAST(CAST(123456789012345678 AS decimal(18, 0)) AS sql_variant)),
	(12, CAST(CAST(1234567890.123456789 AS decimal(19, 9)) AS sql_variant)),
	(13, CAST(CAST(-12345678901234567890.12345678 AS decimal(28, 8)) AS sql_variant)),
	(14, CAST(CAST(99999999999999999999999999999999999999 AS decimal(38, 0)) AS sql_variant)),
	(15, CAST(CAST(-0.00000000000000000000000000000000000001 AS decimal(38, 38)) AS sql_variant)),
	(16, CAST(CAST(12.5 AS numeric(10, 3)) AS sql_variant)), (17, CAST(CAST(-922337203685477.5808 AS money) AS sql_variant)),
	(18, CAST(CAST(214748.3647 AS smallmoney) AS sql_variant)), (19, CAST(CAST('0001-01-01' AS date) AS sql_variant)),
	(20, CAST(CAST('9999-12-31' AS date) AS sql_variant)), (21, CAST(CAST('23:59:59' AS time(0)) AS sql_variant)),
	(22, CAST(CAST('10:00:00.123' AS time(3)) AS sql_variant)), (23, CAST(CAST('10:00:00.123456' AS time(6)) AS sql_variant)),
	(24, CAST(CAST('10:00:00.1234567' AS time(7)) AS sql_variant)),
	(25, CAST(CAST('2026-01-01 10:00:00' AS datetime2(0)) AS sql_variant)),
	(26, CAST(CAST('2026-01-01 10:00:00.123' AS datetime2(3)) AS sql_variant)),
	(27, CAST(CAST('2026-01-01 10:00:00.123456' AS datetime2(6)) AS sql_variant)),
	(28, CAST(CAST('2026-01-01 10:00:00.1234567' AS datetime2(7)) AS sql_variant)),
	(29, CAST(CAST('9999-12-31 23:59:59.9999999' AS datetime2(7)) AS sql_variant)),
	(30, CAST(CAST('0001-01-01 00:00:00' AS datetime2(7)) AS sql_variant)),
	(31, CAST(CAST('2026-11-13 07:08:09.123' AS datetime) AS sql_variant)),
	(32, CAST(CAST('2026-11-13 07:08:00' AS smalldatetime) AS sql_variant)),
	(33, CAST(CAST('2026-01-01 10:00:00 +14:00' AS datetimeoffset(0)) AS sql_variant)),
	(34, CAST(CAST('2026-01-01 10:00:00.123456 -14:00' AS datetimeoffset(6)) AS sql_variant)),
	(35, CAST(CAST('2026-01-01 10:00:00.1234567 +05:30' AS datetimeoffset(7)) AS sql_variant)),
	(36, CAST(CAST('9999-12-31 23:59:59.9999999 +00:00' AS datetimeoffset(7)) AS sql_variant)),
	(37, CAST(CAST('6F9619FF-8B86-D011-B42D-00C04FC964FF' AS uniqueidentifier) AS sql_variant)),
	(38, CAST(CAST(0x0102 AS binary(4)) AS sql_variant)), (39, CAST(CAST(0x010203 AS varbinary(10)) AS sql_variant)),
	(40, CAST(CAST('ab' COLLATE SQL_Latin1_General_CP1_CI_AS AS char(5)) AS sql_variant)),
	(41, CAST(CAST(N'café' COLLATE SQL_Latin1_General_CP1_CI_AS AS varchar(10)) AS sql_variant)),
	(42, CAST(CAST(N'дом' COLLATE Cyrillic_General_CI_AS AS varchar(10)) AS sql_variant)),
	(43, CAST(CAST(N'漢字' COLLATE Japanese_CI_AS AS varchar(10)) AS sql_variant)),
	(45, CAST(CAST(N'ab' COLLATE SQL_Latin1_General_CP1_CI_AS AS nchar(10)) AS sql_variant)),
	(46, CAST(CAST(N'😀x' COLLATE Latin1_General_100_CI_AS_SC AS nvarchar(20)) AS sql_variant)),
	(47, CAST(CAST(N'дом' COLLATE Cyrillic_General_CI_AS AS char(6)) AS sql_variant)), (48, NULL),
	(49, CAST(CAST('' COLLATE SQL_Latin1_General_CP1_CI_AS AS varchar(5)) AS sql_variant)),
	(50, CAST(CAST(0x AS varbinary(5)) AS sql_variant)),
	(51, CAST(CAST(N'ελληνικά' COLLATE Greek_CI_AS AS varchar(20)) AS sql_variant)),
	(52, CAST(CAST(N'한국어' COLLATE Korean_Wansung_CI_AS AS varchar(20)) AS sql_variant)),
	(53, CAST(CAST(N'中文' COLLATE Chinese_PRC_CI_AS AS varchar(20)) AS sql_variant)),
	(54, CAST(CAST(N'ąę' COLLATE Polish_CI_AS AS char(4)) AS sql_variant)),
	(55, CAST(CAST(REPLICATE('w', 8000) AS varchar(8000)) AS sql_variant)),
	(56, CAST(CAST(REPLICATE(N'ж', 4000) AS nvarchar(4000)) AS sql_variant)),
	(57, CAST(CAST(REPLICATE(0xAB, 8000) AS varbinary(8000)) AS sql_variant));
-- A UTF-8 collation exists from 2019 (major 15): before it, no row 44.
IF CAST(SERVERPROPERTY('ProductMajorVersion') AS int) >= 15
	EXEC (N'INSERT INTO dbo.v_edges VALUES (44, CAST(CAST(N''😀é'' COLLATE Latin1_General_100_CI_AS_SC_UTF8 AS varchar(20)) AS sql_variant))');
GO
-- 2. A mixed column: 3000 rows of generated values, a base type each by its id, and NULLs.
CREATE TABLE dbo.v_mixed (id int PRIMARY KEY, tag varchar(10) NOT NULL, v sql_variant NULL);
WITH n AS (SELECT TOP (3000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.v_mixed SELECT i, CONCAT('t', i % 13), CASE i % 13
	WHEN 0 THEN NULL
	WHEN 1 THEN CAST(i AS sql_variant)
	WHEN 2 THEN CAST(CAST(i * 1000003 AS bigint) AS sql_variant)
	WHEN 3 THEN CAST(CAST(i / 7.0 AS float) AS sql_variant)
	WHEN 4 THEN CAST(CAST(i / 100.0 AS decimal(12, 4)) AS sql_variant)
	WHEN 5 THEN CAST(CAST(i AS money) / 3 AS sql_variant)
	WHEN 6 THEN CAST(DATEADD(day, i, CAST('2000-01-01' AS date)) AS sql_variant)
	WHEN 7 THEN CAST(DATEADD(second, i * 37, CAST('2020-02-29 00:00:00' AS datetime)) AS sql_variant)
	WHEN 8 THEN CAST(DATEADD(microsecond, i * 7, CAST('2021-06-01 12:00:00.1234567' AS datetime2(7))) AS sql_variant)
	WHEN 9 THEN CAST(CONCAT('v_', i) AS sql_variant)
	WHEN 10 THEN CAST(CONCAT(N'н_', i) AS sql_variant)
	WHEN 11 THEN CAST(CAST(i AS varbinary(4)) AS sql_variant)
	ELSE CAST(NEWID() AS sql_variant) END
FROM n;
GO
-- 3. A heap whose rows grow: a small value replaced by a long one moves the row (a forwarded record).
CREATE TABLE dbo.v_heap (id int NOT NULL, pad varchar(200) NOT NULL, v sql_variant NULL);
WITH n AS (SELECT TOP (600) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.v_heap SELECT i, REPLICATE('p', 150), CAST(i AS sql_variant) FROM n;
UPDATE dbo.v_heap SET v = CAST(REPLICATE(N'г', 3000) AS sql_variant) WHERE id % 40 = 7;
GO
-- 4. ROW and PAGE compression (the CD record's value put back in its uncompressed form, spec 086); PAGE with
-- repeated values for the dictionary and the anchor.
CREATE TABLE dbo.v_row (id int PRIMARY KEY, v sql_variant NULL) WITH (DATA_COMPRESSION = ROW);
INSERT INTO dbo.v_row SELECT id, v FROM dbo.v_edges;
INSERT INTO dbo.v_row SELECT 1000 + id, v FROM dbo.v_mixed WHERE id <= 1000;
CREATE TABLE dbo.v_page (id int PRIMARY KEY, v sql_variant NULL) WITH (DATA_COMPRESSION = PAGE);
INSERT INTO dbo.v_page SELECT id, v FROM dbo.v_edges;
WITH n AS (SELECT TOP (2000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.v_page SELECT 1000 + i, CASE i % 3 WHEN 0 THEN CAST('same text' AS sql_variant)
	WHEN 1 THEN CAST(CAST(42 AS int) AS sql_variant) ELSE CAST(CAST('2024-01-01' AS date) AS sql_variant) END FROM n;
ALTER TABLE dbo.v_page REBUILD WITH (DATA_COMPRESSION = PAGE);
GO
-- 5. Values pushed off row: a row past 8,060 bytes moves its widest variable columns to row-overflow pages.
CREATE TABLE dbo.v_ovf (id int PRIMARY KEY, pad varchar(5000) NOT NULL, v sql_variant NULL);
INSERT INTO dbo.v_ovf VALUES
	(1, REPLICATE('a', 5000), CAST(REPLICATE('b', 7000) AS varchar(7000))),
	(2, REPLICATE('a', 5000), CAST(REPLICATE(N'щ', 3900) AS nvarchar(3900))),
	(3, REPLICATE('a', 5000), CAST(REPLICATE(0xCD, 7500) AS varbinary(7500))),
	(4, REPLICATE('a', 5000), CAST(7 AS sql_variant)),
	(5, 'short', CAST(REPLICATE('c', 7000) AS varchar(7000)));
GO
-- 6. A sql_variant clustered key, not unique: the uniquifier beside it, values compared across base types.
CREATE TABLE dbo.v_key (v sql_variant NOT NULL, n int NOT NULL);
CREATE CLUSTERED INDEX cx_v_key ON dbo.v_key (v);
INSERT INTO dbo.v_key VALUES (CAST(1 AS sql_variant), 1), (CAST(1 AS sql_variant), 2), (CAST('1' AS sql_variant), 3),
	(CAST(CAST(1.0 AS float) AS sql_variant), 4), (CAST(CAST('2024-01-01' AS date) AS sql_variant), 5),
	(CAST(N'z' AS sql_variant), 6), (CAST(N'z' AS sql_variant), 7);
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_variant TO DISK = '$(DIR)/variant_full.bak' WITH FORMAT, INIT, CHECKSUM, COMPRESSION, NAME = 'variant_full';
GO
-- 7 is variant_changes.sql's, which generate.sh runs after the golden catalog of variant_full.
