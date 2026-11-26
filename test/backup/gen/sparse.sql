-- Spec 086 PR 3: sparse columns (the record's sparse vector, E24), for the oracle lane, local development and the
-- committed test/data/backup/sparse. Every type a sparse column can have, at its edges and over rows that set a
-- few of them; a regular column off row before the vector; 1000 sparse columns; a column set; sparseness
-- changed by ALTER (rows rewritten, the old entries kept under a dropped hbcolid); a sparse column dropped and
-- one added; a heap whose rows grow and are forwarded; a non-unique clustered index
-- (the uniquifier beside the vector); values stored off row and row-overflow inside the vector; a partitioned
-- table; nullable char / binary under ANSI_PADDING OFF; a filtered index on a sparse column; row versioning
-- last. Synthetic rows only (T4).
SET NOCOUNT ON;
SET QUOTED_IDENTIFIER ON;	-- a filtered index needs it (sqlcmd's default is off)
GO
IF DB_ID('bak_sparse') IS NOT NULL DROP DATABASE bak_sparse;
GO
CREATE DATABASE bak_sparse;
GO
USE bak_sparse;
GO
-- 1. Every type, between regular columns (a varchar(max) of them off row now and then).
CREATE TABLE dbo.types_sparse (
	id int NOT NULL PRIMARY KEY,
	r1 varchar(10) NULL,
	s_bit bit SPARSE NULL, s_tiny tinyint SPARSE NULL, s_small smallint SPARSE NULL, s_int int SPARSE NULL,
	s_big bigint SPARSE NULL, s_dec decimal(9, 2) SPARSE NULL, s_dec38 decimal(38, 10) SPARSE NULL,
	s_num numeric(18, 0) SPARSE NULL, s_money money SPARSE NULL, s_smoney smallmoney SPARSE NULL,
	s_real real SPARSE NULL, s_float float SPARSE NULL, s_date date SPARSE NULL, s_time time(7) SPARSE NULL,
	s_time0 time(0) SPARSE NULL, s_dt datetime SPARSE NULL, s_sdt smalldatetime SPARSE NULL,
	s_dt2 datetime2(3) SPARSE NULL, s_dto datetimeoffset(3) SPARSE NULL, s_guid uniqueidentifier SPARSE NULL,
	s_char char(6) SPARSE NULL, s_vc varchar(20) SPARSE NULL, s_nchar nchar(4) SPARSE NULL,
	s_nvc nvarchar(20) SPARSE NULL, s_bin binary(5) SPARSE NULL, s_vbin varbinary(20) SPARSE NULL,
	s_vcmax varchar(max) SPARSE NULL, s_nvcmax nvarchar(max) SPARSE NULL, s_vbmax varbinary(max) SPARSE NULL,
	s_var sql_variant SPARSE NULL, s_xml xml SPARSE NULL,
	r2 nvarchar(10) NULL,
	rmax varchar(max) NULL);
INSERT dbo.types_sparse (id, r1, r2) VALUES (1, 'allnull', N'z');
INSERT dbo.types_sparse VALUES (2, 'max', 1, 255, 32767, 2147483647, 9223372036854775807, 9999999.99,
	9999999999999999999999999999.9999999999, 999999999999999999, 922337203685477.5807, 214748.3647, 3.4e38,
	1.79e308, '9999-12-31', '23:59:59.9999999', '23:59:59', '9999-12-31T23:59:59.997', '2079-06-06T23:59:00',
	'9999-12-31T23:59:59.999', '9999-12-31T23:59:59.999+14:00', 'FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF',
	'zzzzzz', REPLICATE('z', 20), N'яяяя', REPLICATE(N'я', 20), 0xFFFFFFFFFF, CAST(REPLICATE('z', 20) AS varbinary(20)),
	'max in row', N'макс', 0xCAFE, CAST(42 AS int), '<a b="1">t</a>', N'end', 'r');
INSERT dbo.types_sparse VALUES (3, 'min', 0, 0, -32768, -2147483648, -9223372036854775808, -9999999.99,
	-9999999999999999999999999999.9999999999, -999999999999999999, -922337203685477.5808, -214748.3648, -3.4e38,
	-1.79e308, '0001-01-01', '00:00:00', '00:00:00', '1753-01-01T00:00:00', '1900-01-01T00:00:00',
	'0001-01-01T00:00:00', '0001-01-01T00:00:00-14:00', '00000000-0000-0000-0000-000000000000',
	'a', 'a', N'ж', N'ж', 0x00, 0x00, 'm', N'м', 0x00, CAST(N'вариант' AS nvarchar(20)), '<x/>', N'a', NULL);
INSERT dbo.types_sparse (id, s_vc, s_nvc, s_vbin, s_char, s_bin, s_vcmax, s_nvcmax, s_vbmax) VALUES
	(4, '', N'', 0x, '', 0x, '', N'', 0x);	-- empty values, not NULL
INSERT dbo.types_sparse (id, s_vcmax, rmax) VALUES (5, REPLICATE(CAST('x' AS varchar(max)), 10000),
	REPLICATE(CAST('r' AS varchar(max)), 9000));	-- off row in the vector and before it
INSERT dbo.types_sparse (id, s_vbmax, s_nvcmax) VALUES (6, CAST(REPLICATE(CAST('y' AS varchar(max)), 90000) AS varbinary(max)),
	REPLICATE(CAST(N'я' AS nvarchar(max)), 5000));
INSERT dbo.types_sparse (id, r1, rmax) VALUES (7, 'rmax only', REPLICATE(CAST('q' AS varchar(max)), 9000));
-- An empty value, not NULL, in the last regular variable column before the vector (review of #626).
INSERT dbo.types_sparse (id, rmax, s_int) VALUES (8, '', 8);
INSERT dbo.types_sparse (id, r2, s_int, s_vc) VALUES (9, N'', 9, 'nine');
INSERT dbo.types_sparse (id, r1, s_big) VALUES (10, '', 10);
INSERT dbo.types_sparse (id, r1, r2, rmax, s_bit) VALUES (11, '', N'', '', 1);
WITH n AS (SELECT TOP (3000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT dbo.types_sparse (id, r1, s_bit, s_int, s_big, s_dec, s_float, s_date, s_dt2, s_guid, s_char, s_vc, s_nvc, s_vbin,
	s_vcmax, s_var, r2)
SELECT 100 + i, CASE WHEN i % 4 = 0 THEN NULL ELSE CONCAT('r', i % 9) END,
	CASE WHEN i % 3 = 0 THEN i % 2 END, CASE WHEN i % 5 = 0 THEN i END, CASE WHEN i % 7 = 0 THEN CAST(i AS bigint) * 1000003 END,
	CASE WHEN i % 11 = 0 THEN i / 7.0 END, CASE WHEN i % 13 = 0 THEN i / 3.0 END,
	CASE WHEN i % 6 = 0 THEN DATEADD(day, i, '2000-01-01') END, CASE WHEN i % 8 = 0 THEN DATEADD(second, i, '2020-02-29T00:00:00.123') END,
	CASE WHEN i % 17 = 0 THEN NEWID() END, CASE WHEN i % 19 = 0 THEN CONCAT('c', i % 10) END,
	CASE WHEN i % 2 = 0 THEN CONCAT('v_', i) END, CASE WHEN i % 23 = 0 THEN CONCAT(N'н_', i) END,
	CASE WHEN i % 29 = 0 THEN CAST(i AS varbinary(4)) END,
	CASE WHEN i % 500 = 0 THEN REPLICATE(CAST('L' AS varchar(max)), 8500) WHEN i % 31 = 0 THEN CONCAT('m', i) END,
	CASE WHEN i % 37 = 0 THEN CAST(i AS int) END, CASE WHEN i % 3 = 1 THEN CONCAT(N'z', i % 5) END
FROM n;
GO
-- 2. A thousand sparse columns, a few set per row: hbcolids past 255, vectors of every length.
DECLARE @sql nvarchar(max) = N'CREATE TABLE dbo.wide_sparse (id int NOT NULL PRIMARY KEY';
DECLARE @i int = 1;
WHILE @i <= 1000 BEGIN SET @sql += N', w' + CAST(@i AS nvarchar(10)) + N' int SPARSE NULL'; SET @i += 1; END
SET @sql += N', tail varchar(10) NULL)';
EXEC (@sql);
SET @sql = N'INSERT dbo.wide_sparse (id, tail) SELECT TOP (400) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)), ''t'' FROM sys.all_objects';
EXEC (@sql);
SET @i = 1;
WHILE @i <= 1000 BEGIN
	SET @sql = N'UPDATE dbo.wide_sparse SET w' + CAST(@i AS nvarchar(10)) + N' = id * 1000 + ' + CAST(@i AS nvarchar(10)) +
		N' WHERE (id * 7 + ' + CAST(@i AS nvarchar(10)) + N') % 97 = 0';
	EXEC (@sql);
	SET @i += 1;
END
UPDATE dbo.wide_sparse SET w1 = 1, w1000 = 1000, w500 = 500 WHERE id = 1;
GO
-- 3. A column set: rows written through it and through the columns. The column set itself is not read (not stored).
CREATE TABLE dbo.colset_sparse (id int NOT NULL PRIMARY KEY, a int SPARSE NULL, b varchar(10) SPARSE NULL,
	c datetime2(3) SPARSE NULL, d float SPARSE NULL, e varbinary(8) SPARSE NULL, f nvarchar(10) SPARSE NULL,
	g decimal(9, 3) SPARSE NULL, h bit SPARSE NULL, cs xml COLUMN_SET FOR ALL_SPARSE_COLUMNS);
INSERT dbo.colset_sparse (id, a, b, c, d, e, f, g, h) VALUES (1, 5, 'x<&>', '2024-01-02T03:04:05.678', 0.1, 0x0A0B, N'юни', -1.5, 1);
INSERT dbo.colset_sparse (id) VALUES (2);
INSERT dbo.colset_sparse (id, cs) VALUES (3, '<a>9</a><b>q</b><h>0</h>');
WITH n AS (SELECT TOP (200) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT dbo.colset_sparse (id, a, f) SELECT 10 + i, CASE WHEN i % 2 = 0 THEN i END, CASE WHEN i % 3 = 0 THEN CONCAT(N'ф', i) END FROM n;
GO
-- 4. Sparseness changed after rows were written: ADD SPARSE / DROP SPARSE rewrite them (E24), a sparse column
-- added, one dropped (a sparse column takes no DEFAULT: 1791).
CREATE TABLE dbo.alter_sparse (id int NOT NULL PRIMARY KEY, x int NULL, y int SPARSE NULL, v varchar(10) NULL,
	gone int SPARSE NULL, keep varchar(10) SPARSE NULL);
WITH n AS (SELECT TOP (300) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT dbo.alter_sparse SELECT i, CASE WHEN i % 3 > 0 THEN i END, CASE WHEN i % 4 > 0 THEN -i END, CONCAT('v', i),
	CASE WHEN i % 2 = 0 THEN i END, CASE WHEN i % 5 = 0 THEN CONCAT('k', i) END FROM n;
ALTER TABLE dbo.alter_sparse ALTER COLUMN x ADD SPARSE;
ALTER TABLE dbo.alter_sparse ALTER COLUMN y DROP SPARSE;
ALTER TABLE dbo.alter_sparse DROP COLUMN gone;
ALTER TABLE dbo.alter_sparse ADD added int SPARSE NULL;
INSERT dbo.alter_sparse (id, x, y, v, keep, added) VALUES (1001, 1, 2, 'after', 'k', 3);
GO
-- 5. A heap whose rows grow: forwarded records holding the vector before their back pointer.
CREATE TABLE dbo.heap_sparse (id int NOT NULL, pad varchar(100) NULL, s varchar(4000) SPARSE NULL, n int SPARSE NULL);
WITH n AS (SELECT TOP (400) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT dbo.heap_sparse SELECT i, REPLICATE('p', 100), NULL, CASE WHEN i % 2 = 0 THEN i END FROM n;
UPDATE dbo.heap_sparse SET s = REPLICATE('s', 3000) WHERE id % 40 = 3;
-- An empty pad before the vector, and those rows forwarded too (review of #626).
WITH n AS (SELECT TOP (40) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT dbo.heap_sparse SELECT 1000 + i, '', NULL, 1000 + i FROM n;
UPDATE dbo.heap_sparse SET s = REPLICATE('e', 4000) WHERE id > 1000 AND id % 4 = 1;
GO
-- 6. A non-unique clustered index: the uniquifier beside the vector.
CREATE TABLE dbo.uniq_sparse (k int NOT NULL, s1 int SPARSE NULL, s2 nvarchar(20) SPARSE NULL, v varchar(10) NULL);
CREATE CLUSTERED INDEX cx_uniq_sparse ON dbo.uniq_sparse (k);
WITH n AS (SELECT TOP (600) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT dbo.uniq_sparse SELECT i % 20, CASE WHEN i % 3 = 0 THEN i END, CASE WHEN i % 5 = 0 THEN CONCAT(N'ю', i) END,
	CASE WHEN i % 2 = 0 THEN 'v' END FROM n;
INSERT dbo.uniq_sparse VALUES (5, 55, N'empty v', ''), (5, 56, NULL, '');	-- an empty v before the vector (review of #626)
GO
-- 7. A vector past the row: row-overflow and LOB roots inside it.
CREATE TABLE dbo.ovf_sparse (id int NOT NULL PRIMARY KEY, a varchar(4000) SPARSE NULL, b varchar(4000) SPARSE NULL,
	c nvarchar(4000) SPARSE NULL, m nvarchar(max) SPARSE NULL, d int SPARSE NULL);
INSERT dbo.ovf_sparse VALUES (1, REPLICATE('a', 4000), REPLICATE('b', 4000), REPLICATE(N'c', 3000), REPLICATE(CAST(N'м' AS nvarchar(max)), 6000), 4);
INSERT dbo.ovf_sparse VALUES (2, 'x', NULL, NULL, NULL, 5);
INSERT dbo.ovf_sparse VALUES (3, REPLICATE('a', 3000), REPLICATE('b', 3000), REPLICATE(N'c', 1500), N'm', NULL);
UPDATE dbo.ovf_sparse SET d = NULL WHERE id = 2;
GO
-- 8. A partitioned table.
CREATE PARTITION FUNCTION pf_sparse (int) AS RANGE LEFT FOR VALUES (100);
CREATE PARTITION SCHEME ps_sparse AS PARTITION pf_sparse ALL TO ([PRIMARY]);
CREATE TABLE dbo.part_sparse (id int NOT NULL PRIMARY KEY, s int SPARSE NULL, t varchar(20) SPARSE NULL) ON ps_sparse (id);
WITH n AS (SELECT TOP (300) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT dbo.part_sparse SELECT i, CASE WHEN i % 2 = 0 THEN i END, CASE WHEN i % 3 = 0 THEN CONCAT('t', i) END FROM n;
GO
-- 9. Nullable char / binary under ANSI_PADDING OFF: read without their trailing spaces / zero bytes.
SET ANSI_PADDING OFF;
GO
CREATE TABLE dbo.ansi_sparse (id int NOT NULL PRIMARY KEY, c char(8) SPARSE NULL, b binary(6) SPARSE NULL);
GO
SET ANSI_PADDING ON;
GO
INSERT dbo.ansi_sparse VALUES (1, 'ab', 0x0102), (2, '', 0x), (3, 'abcdefgh', 0x010203040506), (4, NULL, NULL), (5, 'a  ', 0x0100);
GO
-- 10. A filtered index on a sparse column (a secondary index: the base table is what is read).
CREATE INDEX ix_types_sparse_int ON dbo.types_sparse (s_int) WHERE s_int IS NOT NULL;
GO
-- 10b. Review of #626: a text column (its 16-byte root pointer, the regular complex form that can begin with the
-- vector's tag) as the last regular variable column, every combination of it and the sparse column set or NULL;
-- and an empty last regular column ('') beside a vector holding the entry of a sparse column dropped, or made
-- not sparse by ALTER COLUMN ... DROP SPARSE.
CREATE TABLE dbo.text_sparse (id int NOT NULL PRIMARY KEY, t text NULL, s bigint SPARSE NULL);
INSERT dbo.text_sparse VALUES (1, 'some text', 11), (2, 'more text', NULL), (3, NULL, 33), (4, NULL, NULL), (5, '', 55);
GO
CREATE TABLE dbo.dropped_sparse (id int NOT NULL PRIMARY KEY, v varchar(10) NULL, a int SPARSE NULL, b int SPARSE NULL);
INSERT dbo.dropped_sparse VALUES (1, '', 1, 10), (2, 'x', 2, 20), (3, '', 3, NULL), (4, NULL, 4, 40);
GO
ALTER TABLE dbo.dropped_sparse DROP COLUMN a;
GO
CREATE TABLE dbo.unsparsed (id int NOT NULL PRIMARY KEY, v varchar(10) NULL, y int SPARSE NULL, z int SPARSE NULL);
INSERT dbo.unsparsed VALUES (1, '', 1, 10), (2, 'x', 2, 20), (3, '', NULL, 30), (4, '', 4, NULL);
GO
ALTER TABLE dbo.unsparsed ALTER COLUMN y DROP SPARSE;
GO
-- 11. Row versioning (RCSI) last: version tags on records with a vector.
ALTER DATABASE bak_sparse SET ALLOW_SNAPSHOT_ISOLATION ON;
ALTER DATABASE bak_sparse SET READ_COMMITTED_SNAPSHOT ON WITH ROLLBACK IMMEDIATE;
GO
CREATE TABLE dbo.ver_sparse (id int NOT NULL PRIMARY KEY, v int SPARSE NULL, s nvarchar(20) SPARSE NULL);
WITH n AS (SELECT TOP (500) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT dbo.ver_sparse SELECT i, CASE WHEN i % 2 = 0 THEN i END, CASE WHEN i % 3 = 0 THEN CONCAT(N'в', i) END FROM n;
UPDATE dbo.ver_sparse SET v = -v, s = N'u' WHERE id % 5 = 0;
DELETE FROM dbo.ver_sparse WHERE id % 7 = 0;
GO
CHECKPOINT;
GO
-- Compressed, the full and the differential alike: committed under test/data/backup/sparse, they stay small
-- (6.5 MB plain); the plain container is every other fixture's.
BACKUP DATABASE bak_sparse TO DISK = '$(DIR)/sparse_full.bak' WITH FORMAT, INIT, CHECKSUM, COMPRESSION, NAME = 'sparse_full';
GO
-- 12 is sparse_changes.sql's, which generate.sh runs after the golden catalog of sparse_full.
