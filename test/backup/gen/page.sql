-- Spec 086 PR 2: PAGE-compressed tables (a page's compression-information record: its anchor record of
-- prefixes and its dictionary, E23), for the oracle lane and local development only. cd.sql's shapes under
-- PAGE, with rows enough to fill pages (a page holds a compression-information record only once it filled:
-- a page short of it is a ROW page, flag 0x80 not set): every type 085 decodes with its edge values beside
-- thousands of rows sharing prefixes and repeating values, in a PAGE table and an uncompressed twin holding
-- the same rows (acceptance 2: the two read alike, _mssql_rid included); 31, 60, 129 and 300 columns; values
-- stored off row and row-overflow; a PAGE heap with forwarded rows (made after the rebuild, which would
-- undo them); a non-unique clustered index (the uniquifier); ghosts (trace flag 661, global: this throwaway
-- server only); columns added after the rows; keys of a bit, a datetime, a decimal and an nvarchar; a table
-- with PAGE, ROW and uncompressed partitions; nullable char / binary under ANSI_PADDING OFF; rowversion,
-- sql_variant, xml, hierarchyid, geometry and text pointers; row versioning last. Synthetic rows only (T4).
SET NOCOUNT ON;
GO
DBCC TRACEON (661, -1);	-- no ghost cleanup: deleted rows stay as ghosts
GO
IF DB_ID('bak_page') IS NOT NULL DROP DATABASE bak_page;
GO
CREATE DATABASE bak_page;
GO
USE bak_page;
GO
-- 1. Every type: cd.sql's edge rows, then 3000 rows of repeating values and shared prefixes.
CREATE TABLE dbo.types_page (
	id int NOT NULL PRIMARY KEY,
	ti tinyint NULL, si smallint NULL, i int NULL, bi bigint NULL, b bit NULL,
	d52 decimal(5, 2) NULL, d184 decimal(18, 4) NULL, d190 decimal(19, 0) NULL, d286 decimal(28, 6) NULL,
	d3810 decimal(38, 10) NULL, d3838 decimal(38, 38) NULL, n105 numeric(10, 5) NULL,
	m money NULL, sm smallmoney NULL, r real NULL, f float NULL,
	dt date NULL, t0 time(0) NULL, t3 time(3) NULL, t7 time(7) NULL,
	dt20 datetime2(0) NULL, dt23 datetime2(3) NULL, dt27 datetime2(7) NULL,
	dto0 datetimeoffset(0) NULL, dto7 datetimeoffset(7) NULL, dtm datetime NULL, sdt smalldatetime NULL,
	c10 char(10) NULL, vc20 varchar(20) NULL, nc10 nchar(10) NULL, nv200 nvarchar(200) NULL,
	bn8 binary(8) NULL, vb20 varbinary(20) NULL, g uniqueidentifier NULL,
	vcm varchar(max) NULL, nvm nvarchar(max) NULL, vbm varbinary(max) NULL
) WITH (DATA_COMPRESSION = PAGE);
GO
-- One row a statement: a VALUES list would unify each column's literals into one type first.
INSERT INTO dbo.types_page VALUES (1, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
 NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
INSERT INTO dbo.types_page VALUES (2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '0001-01-01', '00:00:00', '00:00:00', '00:00:00',
 '0001-01-01', '0001-01-01', '1700-01-01', '0001-01-01 00:00:00 +00:00', '0001-01-01 00:00:00 +00:00', '1900-01-01',
 '1900-01-01', '', '', N'', N'', 0x, 0x, '00000000-0000-0000-0000-000000000000', '', N'', 0x);
INSERT INTO dbo.types_page VALUES (3, 255, 32767, 2147483647, 9223372036854775807, 1, 999.99, 99999999999999.9999, 9999999999999999999,
 9999999999999999999999.999999, 9999999999999999999999999999.9999999999, 0.99999999999999999999999999999999999999,
 99999.99999, 922337203685477.5807, 214748.3647, 3.4E+38, 1.7976931348623157E+308, '9999-12-31', '23:59:59',
 '23:59:59.999', '23:59:59.9999999', '9999-12-31 23:59:59', '9999-12-31 23:59:59.999', '2262-01-01 00:00:00.0000001',
 '9999-12-31 23:59:59 +14:00', '9999-12-31 23:59:59.9999999 +14:00', '9999-12-31 23:59:59.997', '2079-06-06 23:59',
 '0123456789', '12345678901234567890', N'日本語テキストです', REPLICATE(N'ж', 200), 0xFFFFFFFFFFFFFFFF,
 0x0102030405060708090A0B0C0D0E0F1011121314, 'FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF',
 REPLICATE(CAST('x' AS varchar(max)), 9000), REPLICATE(CAST(N'ж' AS nvarchar(max)), 5000),
 CAST(REPLICATE(CAST('z' AS varchar(max)), 9000) AS varbinary(max)));
INSERT INTO dbo.types_page VALUES (4, 1, -32768, -2147483648, -9223372036854775808, 0, -999.99, -0.0001, -1, -0.000001, -1.5,
 -0.00000000000000000000000000000000000001, -2.5, -922337203685477.5808, -214748.3648, -1.18E-38,
 2.2250738585072014E-308, '1900-01-01', '12:00:00', '00:00:00.001', '00:00:00.0000001', '2026-11-16 10:11:12',
 '2026-11-16 10:11:12.123', '1900-01-01 00:00:00.0000001', '2026-11-16 10:11:12 +03:00',
 '2026-11-16 10:11:12.1234567 +05:30', '1753-01-01', '1900-01-01 00:01', 'a', 'abc   ', N'ab', N'abc',
 0x01, 0x00, '6F9619FF-8B86-D011-B42D-00C04FC964FF', 'short', N'короткий', 0x0102);
INSERT INTO dbo.types_page VALUES (5, 128, 128, -24, 4294967296, 1, 0.01, 1, 1000000000000000000, 1234567890123456789012.123456, 123456789,
 0.12345678901234567890123456789012345678, 3.14159, 12.34, 0.5, 0.5, 0.1, '2000-02-29', '00:00:01', '12:34:56.789',
 '12:34:56.7890123', '2000-02-29 12:00:00', '1999-12-31 23:59:59.999', '2026-11-16 10:11:12.1234567',
 '2026-11-16 10:11:12 -14:00', '0001-01-01 00:00:00 -00:01', '1899-12-31 23:59:59.997', '1900-01-02',
 '  lead', 'q', N'жук', N'Ελληνικά, עברית, العربية, ภาษาไทย, 한국어', 0x00000000000000FF, 0x0000,
 NULL, NULL, NULL, NULL);
INSERT INTO dbo.types_page VALUES (6, 127, -129, 65536, -1099511627777, NULL, 10.50, 1000000, 10, 10.000001, 0.0000000001, 0.1, 12345.00001,
 0.0001, -1, 100, 100, '0001-01-02', NULL, NULL, NULL, NULL, NULL, '1900-01-01 00:00:00.0000001', NULL, NULL,
 '1900-01-01 00:00:00.003', '2026-11-16 10:11', ' ', 'abcdefghij', N'𝄞x', N'mixedЖ日𝄞end', NULL, 0x01020304,
 NULL, REPLICATE('q', 300), N'𝄞𝄞', NULL);
INSERT INTO dbo.types_page VALUES (7, 2, 256, -16777217, 1099511627776, 1, 123.45, 12345678901234.5678, 1234567890123456789, 0, -9999999999999999999999999999.9999999999,
 0.00000000000000000000000000000000000001, 0, 1, 1, 1.401298E-45, 4.9E-324, '1753-01-01', '09:30:00', '09:30:00.500',
 '09:30:00.5000000', '1753-01-01 00:00:00', '1753-01-01 00:00:00.000', '1753-01-01 00:00:00.0000000',
 '1753-01-01 00:00:00 +00:00', '1753-01-01 00:00:00.0000000 +00:00', '1753-01-01 00:00:00', '1900-01-01 00:02',
 'ab        ', REPLICATE('v', 20), N'ÀÁÂ', N'line1' + NCHAR(13) + NCHAR(10) + N'line2', 0x0000000000000001,
 0x00, '11111111-2222-3333-4444-555555555555', NULL, N'abc' + NCHAR(0) + N'def', 0x);
GO
-- Repeating values (the dictionary) and shared prefixes (the anchor), NULLs every so often.
WITH n AS (SELECT TOP (3000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.types_page (id, ti, si, i, bi, b, d52, d184, d286, m, sm, r, f, dt, t3, dt23, dto7, dtm, sdt, c10, vc20, nc10, nv200, bn8,
	vb20, g, vcm)
SELECT 100 + i, i % 7, CASE WHEN i % 13 = 0 THEN NULL ELSE i % 50 - 25 END, 1000000 + i, 5000000000 + i % 100, i % 2, i % 9 * 1.25,
	   100000 + i * 0.0137, 1234567890.123456 + i % 10, (i % 4) * 2.5, i % 3, (i % 5) * 0.5, 1.0E10 + i, DATEADD(day, i % 30, '2026-01-01'),
	   DATEADD(ms, i * 1000, CAST('08:00:00' AS time(3))), DATEADD(second, i, '2026-11-16 00:00:00'),
	   DATEADD(minute, i % 60, CAST('2026-11-16 10:00:00 +03:00' AS datetimeoffset(7))), DATEADD(minute, i, '2026-11-16'),
	   DATEADD(minute, i % 90, '2026-11-16'), CHOOSE(i % 4 + 1, 'alpha', 'bravo', 'charlie', NULL),
	   CONCAT('customer_', RIGHT(CONCAT('0000', i), 5)), CHOOSE(i % 3 + 1, N'жук', N'日本', N'zebra'),
	   CONCAT(N'name_prefix_', NCHAR(1072 + i % 32), i % 97), CAST(i % 8 AS binary(8)), CAST(CONCAT('ab', i % 5) AS varbinary(20)),
	   CAST(CONCAT('6F9619FF-8B86-D011-B42D-00C04FC9', RIGHT(CONCAT('0000', i % 6), 4)) AS uniqueidentifier),
	   CASE WHEN i % 100 = 0 THEN REPLICATE(CAST('long' AS varchar(max)), 3000) ELSE CONCAT('short_', i % 20) END
FROM n;
GO
ALTER INDEX ALL ON dbo.types_page REBUILD WITH (DATA_COMPRESSION = PAGE);
SELECT * INTO dbo.types_plain FROM dbo.types_page;	-- the same rows, uncompressed
ALTER TABLE dbo.types_plain ADD CONSTRAINT pk_types_plain PRIMARY KEY (id);
GO
-- 2. Wide tables: clusters of 30 columns, the two-byte column count past 127; 400 rows of repeating values.
DECLARE @n int, @sql nvarchar(max), @cols nvarchar(max), @i int;
DECLARE wide CURSOR LOCAL FOR SELECT v FROM (VALUES (31), (60), (129), (300)) AS w (v);
OPEN wide;
FETCH NEXT FROM wide INTO @n;
WHILE @@FETCH_STATUS = 0
BEGIN
	SET @sql = N'CREATE TABLE dbo.wide_' + CAST(@n AS nvarchar(10)) + N' (id int NOT NULL PRIMARY KEY';
	SET @cols = N'';
	SET @i = 1;
	WHILE @i <= @n
	BEGIN
		SET @sql += N', c' + CAST(@i AS nvarchar(10)) + N' int NULL';
		SET @cols += N', CASE WHEN x.i % ' + CAST(@i % 5 + 2 AS nvarchar(10)) + N' = 0 THEN NULL ELSE ' +
					 CAST(@i * 1000 AS nvarchar(20)) + N' + x.i % ' + CAST(@i % 7 + 1 AS nvarchar(10)) + N' END';
		SET @i += 1;
	END;
	SET @sql += N') WITH (DATA_COMPRESSION = PAGE);';
	EXEC (@sql);
	SET @sql = N'WITH x AS (SELECT TOP (400) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects) '
			 + N'INSERT INTO dbo.wide_' + CAST(@n AS nvarchar(10)) + N' SELECT x.i' + @cols + N' FROM x;'
			 + N'ALTER INDEX ALL ON dbo.wide_' + CAST(@n AS nvarchar(10)) + N' REBUILD WITH (DATA_COMPRESSION = PAGE);';
	EXEC (@sql);
	FETCH NEXT FROM wide INTO @n;
END;
CLOSE wide;
DEALLOCATE wide;
GO
-- 3. Off row in compressed rows: row-overflow and MAX values beside in-row ones sharing prefixes.
CREATE TABLE dbo.overflow_page (id int NOT NULL PRIMARY KEY, a varchar(6000) NULL, b varchar(6000) NULL, n nvarchar(4000) NULL)
	WITH (DATA_COMPRESSION = PAGE);
WITH x AS (SELECT TOP (300) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.overflow_page SELECT i, CONCAT('overflow_prefix_', i % 11),
	CASE WHEN i % 50 = 0 THEN REPLICATE('b', 5000) ELSE CONCAT('b_', i) END,
	CASE WHEN i % 75 = 0 THEN REPLICATE(N'ж', 3000) ELSE CONCAT(N'н_', i % 9) END FROM x;
ALTER INDEX ALL ON dbo.overflow_page REBUILD WITH (DATA_COMPRESSION = PAGE);
GO
-- 4. A PAGE heap, rebuilt compressed, then rows grown past their page: forwarded records and stubs.
CREATE TABLE dbo.heap_page (id int NOT NULL, v varchar(3000) NULL) WITH (DATA_COMPRESSION = PAGE);
WITH n AS (SELECT TOP (2000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.heap_page SELECT i, CONCAT('heap_value_', i % 40) FROM n;
ALTER TABLE dbo.heap_page REBUILD WITH (DATA_COMPRESSION = PAGE);
UPDATE dbo.heap_page SET v = REPLICATE(CAST(CONCAT('g', id) AS varchar(10)), 300) WHERE id % 97 = 0;
GO
-- 5. A non-unique clustered index: the uniquifier, in a PAGE record.
CREATE TABLE dbo.uniq_page (k int NOT NULL, v varchar(20) NOT NULL, n int NULL);
CREATE CLUSTERED INDEX cx_uniq_page ON dbo.uniq_page (k) WITH (DATA_COMPRESSION = PAGE);
WITH n AS (SELECT TOP (2000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.uniq_page SELECT i % 10, CONCAT('u_', i % 30), CASE WHEN i % 4 = 0 THEN NULL ELSE i % 13 END FROM n;
ALTER INDEX ALL ON dbo.uniq_page REBUILD WITH (DATA_COMPRESSION = PAGE);
GO
-- 6. Ghosts, kept by trace flag 661, deleted after the rebuild.
CREATE TABLE dbo.ghost_page (id int NOT NULL PRIMARY KEY, v varchar(20) NOT NULL) WITH (DATA_COMPRESSION = PAGE);
WITH n AS (SELECT TOP (2000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.ghost_page SELECT i, CONCAT('ghost_', i % 25) FROM n;
ALTER INDEX ALL ON dbo.ghost_page REBUILD WITH (DATA_COMPRESSION = PAGE);
DELETE FROM dbo.ghost_page WHERE id % 4 = 0;
GO
-- 7. Columns added after rows were written: metadata-only, with defaults and without.
CREATE TABLE dbo.added_page (id int NOT NULL PRIMARY KEY, v int NULL) WITH (DATA_COMPRESSION = PAGE);
WITH n AS (SELECT TOP (1500) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.added_page SELECT i, CASE WHEN i % 3 = 0 THEN NULL ELSE i % 10 END FROM n;
ALTER INDEX ALL ON dbo.added_page REBUILD WITH (DATA_COMPRESSION = PAGE);
ALTER TABLE dbo.added_page ADD n1 int NULL;
ALTER TABLE dbo.added_page ADD d1 int NOT NULL CONSTRAINT df_added_page DEFAULT 77;
ALTER TABLE dbo.added_page ADD s1 varchar(10) NOT NULL CONSTRAINT df_added_page_s DEFAULT 'zz';
ALTER TABLE dbo.added_page ADD m1 money NOT NULL CONSTRAINT df_added_page_m DEFAULT 1.25;
INSERT INTO dbo.added_page (id, v, n1, d1, s1, m1) VALUES (100001, 30, 3, 33, 'yy', 3.5);
GO
-- 8. Keys that are not the first column, of the types whose key bytes _mssql_rid carries.
CREATE TABLE dbo.keys_page (a int NULL, bk bit NOT NULL, dk datetime NOT NULL, ck decimal(12, 3) NOT NULL, nk nvarchar(20) NOT NULL,
	CONSTRAINT pk_keys_page PRIMARY KEY (bk, dk, ck, nk)) WITH (DATA_COMPRESSION = PAGE);
WITH n AS (SELECT TOP (2000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.keys_page SELECT CASE WHEN i % 5 = 0 THEN NULL ELSE i END, i % 2, DATEADD(minute, i % 100, '2026-11-16'), i % 37 * 1.5,
	CONCAT(N'key_жук_', i) FROM n;
INSERT INTO dbo.keys_page VALUES (1, 0, '1900-01-01', -2, N'a'), (NULL, 1, '1753-01-01', 0, N'𝄞'),
	(4, 0, '9999-12-31 23:59:59.997', 999999999.999, N'mixedЖ日');
ALTER INDEX ALL ON dbo.keys_page REBUILD WITH (DATA_COMPRESSION = PAGE);
SELECT * INTO dbo.keys_plain FROM dbo.keys_page;
ALTER TABLE dbo.keys_plain ADD CONSTRAINT pk_keys_plain PRIMARY KEY (bk, dk, ck, nk);
GO
-- 9. Per-partition compression: PAGE, ROW and uncompressed partitions of one table.
CREATE PARTITION FUNCTION pf_page (int) AS RANGE RIGHT FOR VALUES (1000, 2000);
CREATE PARTITION SCHEME ps_page AS PARTITION pf_page ALL TO ([PRIMARY]);
CREATE TABLE dbo.part_mix (k int NOT NULL, v varchar(20) NULL, n decimal(9, 2) NULL) ON ps_page (k);
CREATE CLUSTERED INDEX cx_part_mix ON dbo.part_mix (k)
	WITH (DATA_COMPRESSION = PAGE ON PARTITIONS (1), DATA_COMPRESSION = ROW ON PARTITIONS (2)) ON ps_page (k);
WITH n AS (SELECT TOP (3000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.part_mix SELECT i, CONCAT('part_', i % 17), i % 20 * 1.5 FROM n;
ALTER INDEX ALL ON dbo.part_mix REBUILD PARTITION = 1 WITH (DATA_COMPRESSION = PAGE);
GO
-- 10. Nullable char / binary under ANSI_PADDING OFF, PAGE and uncompressed alike.
SET ANSI_PADDING OFF;
GO
CREATE TABLE dbo.padoff_page (id int NOT NULL PRIMARY KEY, c char(10) NULL, b binary(8) NULL, n nchar(10) NULL,
	cn char(10) NOT NULL, bn binary(8) NOT NULL) WITH (DATA_COMPRESSION = PAGE);
CREATE TABLE dbo.padoff_plain (id int NOT NULL PRIMARY KEY, c char(10) NULL, b binary(8) NULL, n nchar(10) NULL,
	cn char(10) NOT NULL, bn binary(8) NOT NULL);
GO
SET ANSI_PADDING ON;
GO
WITH n AS (SELECT TOP (1500) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.padoff_page SELECT i, CHOOSE(i % 3 + 1, 'abc', 'abc   ', NULL), CHOOSE(i % 3 + 1, 0x01, 0x0100, NULL),
	CHOOSE(i % 3 + 1, N'жук', N'ab  ', NULL), CHOOSE(i % 3 + 1, 'x', 'yy  ', ''), CHOOSE(i % 3 + 1, 0x02, 0x0300, 0x) FROM n;
ALTER INDEX ALL ON dbo.padoff_page REBUILD WITH (DATA_COMPRESSION = PAGE);
INSERT INTO dbo.padoff_plain SELECT * FROM dbo.padoff_page;
GO
-- 11. rowversion, and the types a compressed record keeps as stored, PAGE and uncompressed alike.
CREATE TABLE dbo.rv_page (id int NOT NULL PRIMARY KEY, v int NULL, rv rowversion) WITH (DATA_COMPRESSION = PAGE);
WITH n AS (SELECT TOP (1500) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.rv_page (id, v) SELECT i, i % 10 FROM n;
ALTER INDEX ALL ON dbo.rv_page REBUILD WITH (DATA_COMPRESSION = PAGE);
UPDATE dbo.rv_page SET v = v + 10 WHERE id % 50 = 2;
CREATE TABLE dbo.stored_page (id int NOT NULL PRIMARY KEY, sv sql_variant NULL, x xml NULL, h hierarchyid NULL, g geometry NULL)
	WITH (DATA_COMPRESSION = PAGE);
WITH n AS (SELECT TOP (1500) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.stored_page SELECT i, CASE i % 3 WHEN 0 THEN CAST(i % 5 AS sql_variant) WHEN 1 THEN CAST(N'жук' AS sql_variant)
	ELSE CAST(CAST(123.45 AS decimal(9, 2)) AS sql_variant) END, CASE WHEN i % 4 = 0 THEN '<a>x</a>' END,
	CASE WHEN i % 2 = 0 THEN CAST('/1/' AS hierarchyid) ELSE CAST('/1/2/' AS hierarchyid) END,
	CASE WHEN i % 6 = 0 THEN geometry::STGeomFromText('POINT (1 2)', 0) END FROM n;
ALTER INDEX ALL ON dbo.stored_page REBUILD WITH (DATA_COMPRESSION = PAGE);
SELECT * INTO dbo.stored_plain FROM dbo.stored_page;
ALTER TABLE dbo.stored_plain ADD CONSTRAINT pk_stored_plain PRIMARY KEY (id);
CREATE TABLE dbo.textptr_page (id int NOT NULL PRIMARY KEY, tx text NULL, ntx ntext NULL, im image NULL)
	WITH (DATA_COMPRESSION = PAGE);
WITH n AS (SELECT TOP (1500) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.textptr_page SELECT i, CASE WHEN i % 3 = 0 THEN NULL ELSE CONCAT('t', i % 4) END, CONCAT(N'ж', i % 4),
	CASE WHEN i % 500 = 0 THEN CAST(REPLICATE(CAST('i' AS varchar(max)), 9000) AS varbinary(max)) ELSE 0x0102 END FROM n;
ALTER INDEX ALL ON dbo.textptr_page REBUILD WITH (DATA_COMPRESSION = PAGE);
GO
-- 12. Row versioning (RCSI): version tags on PAGE records written from here on; ghosts and forwarded rows.
ALTER DATABASE bak_page SET ALLOW_SNAPSHOT_ISOLATION ON;
ALTER DATABASE bak_page SET READ_COMMITTED_SNAPSHOT ON WITH ROLLBACK IMMEDIATE;
GO
CREATE TABLE dbo.ver_page (id int NOT NULL PRIMARY KEY, v int NOT NULL, s nvarchar(20) NULL) WITH (DATA_COMPRESSION = PAGE);
WITH n AS (SELECT TOP (2000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.ver_page SELECT i, i % 10, CONCAT(N'в_', i % 30) FROM n;
ALTER INDEX ALL ON dbo.ver_page REBUILD WITH (DATA_COMPRESSION = PAGE);
UPDATE dbo.ver_page SET v = v + 1000 WHERE id % 2 = 0;
DELETE FROM dbo.ver_page WHERE id % 7 = 0;
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_page TO DISK = '$(DIR)/page_full.bak' WITH FORMAT, INIT, CHECKSUM, NAME = 'page_full';
GO
BACKUP DATABASE bak_page TO DISK = '$(DIR)/page_xpress.bak' WITH FORMAT, INIT, CHECKSUM, COMPRESSION, COPY_ONLY, NAME = 'page_xpress';
GO
-- 13. Changes after the full, then a differential over it (page_diff): rows updated, inserted (pages that split)
-- and deleted under anchors and dictionaries of their own; a table rebuilt whole, every page's anchor and
-- dictionary made anew, its rows the same (no change: compared by their values); and a nullable column added
-- to a PAGE table after the full, then the table rebuilt, its records rewritten with the column and with the
-- defaults section 7 added (stored now, metadata-only in the full) -- its rows unchanged too (review of spec
-- 086 PR 2: compared by the layout's positions, every row was an update). Not here: a column added with a
-- default between the states and then rebuilt, whose every row is an update (the rebuild clears the
-- column's default from sysrscols: nothing says the stored value is the one the older rows read).
UPDATE dbo.types_page SET vc20 = CONCAT('changed_', id % 7), i = i + 1 WHERE id % 50 = 3 AND id > 100;	-- not the edge rows
WITH n AS (SELECT TOP (500) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.types_page (id, vc20, i, c10) SELECT 10000 + i, CONCAT('customer_', RIGHT(CONCAT('0000', i), 5)), i, 'bravo' FROM n;
DELETE FROM dbo.types_page WHERE id BETWEEN 200 AND 260;
UPDATE dbo.uniq_page SET v = 'u_updated' WHERE k = 3 AND n = 7;
DELETE FROM dbo.heap_page WHERE id % 211 = 0;
ALTER INDEX ALL ON dbo.keys_page REBUILD WITH (DATA_COMPRESSION = PAGE, FILLFACTOR = 70);
ALTER TABLE dbo.added_page ADD grown_n int NULL;
ALTER INDEX ALL ON dbo.added_page REBUILD WITH (DATA_COMPRESSION = PAGE);
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_page TO DISK = '$(DIR)/page_diff.bak' WITH FORMAT, INIT, CHECKSUM, DIFFERENTIAL, NAME = 'page_diff';
GO
DBCC TRACEOFF (661, -1);
GO
