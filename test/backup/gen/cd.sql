-- Spec 086 PR 1: ROW-compressed tables (CD records, E22), for the oracle lane and local development only.
-- Every type 085 decodes with its edge values, in a ROW table and in an uncompressed twin holding the
-- same rows (acceptance 2: the two read alike, _mssql_rid included); 31, 60, 129 and 300 columns
-- (clusters of the CD array, the two-byte column count); values stored off row and row-overflow in
-- compressed rows; a ROW heap with forwarded rows and stubs; a non-unique clustered index (the
-- uniquifier); ghosts (trace flag 661, global: this throwaway server only, turned off at the end); columns
-- added after the rows; a key that is not the first column, of a bit, a datetime, a decimal and an
-- nvarchar; a partitioned table with one partition ROW-compressed beside uncompressed ones, and one
-- with a PAGE-compressed partition (refused by name until spec 086 PR 2); Unicode compression (SCSU)
-- over several scripts; nullable char / binary under ANSI_PADDING OFF; rowversion, sql_variant, xml,
-- hierarchyid and geometry in a ROW record; text / ntext / image (their text pointers) in a ROW record; and,
-- last, row versioning (version tags, version ghosts).
SET NOCOUNT ON;
GO
DBCC TRACEON (661, -1);	-- no ghost cleanup: deleted rows stay as ghosts
GO
IF DB_ID('bak_cd') IS NOT NULL DROP DATABASE bak_cd;
GO
CREATE DATABASE bak_cd;
GO
USE bak_cd;
GO
-- 1. Every type, edge values, ROW and uncompressed alike. datetime2(7) within 1677-2262, what
-- TIMESTAMP_NS holds (a value past it reads NULL on both sides, with a warning: spec 093 T9's).
CREATE TABLE dbo.types_row (
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
) WITH (DATA_COMPRESSION = ROW);
GO
-- One row a statement: a VALUES list would unify each column's literals into one type first.
INSERT INTO dbo.types_row VALUES (1, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
 NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
INSERT INTO dbo.types_row VALUES (2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '0001-01-01', '00:00:00', '00:00:00', '00:00:00',
 '0001-01-01', '0001-01-01', '1700-01-01', '0001-01-01 00:00:00 +00:00', '0001-01-01 00:00:00 +00:00', '1900-01-01',
 '1900-01-01', '', '', N'', N'', 0x, 0x, '00000000-0000-0000-0000-000000000000', '', N'', 0x);
INSERT INTO dbo.types_row VALUES (3, 255, 32767, 2147483647, 9223372036854775807, 1, 999.99, 99999999999999.9999, 9999999999999999999,
 9999999999999999999999.999999, 9999999999999999999999999999.9999999999, 0.99999999999999999999999999999999999999,
 99999.99999, 922337203685477.5807, 214748.3647, 3.4E+38, 1.7976931348623157E+308, '9999-12-31', '23:59:59',
 '23:59:59.999', '23:59:59.9999999', '9999-12-31 23:59:59', '9999-12-31 23:59:59.999', '2262-01-01 00:00:00.0000001',
 '9999-12-31 23:59:59 +14:00', '9999-12-31 23:59:59.9999999 +14:00', '9999-12-31 23:59:59.997', '2079-06-06 23:59',
 '0123456789', '12345678901234567890', N'日本語テキストです', REPLICATE(N'ж', 200), 0xFFFFFFFFFFFFFFFF,
 0x0102030405060708090A0B0C0D0E0F1011121314, 'FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF',
 REPLICATE(CAST('x' AS varchar(max)), 9000), REPLICATE(CAST(N'ж' AS nvarchar(max)), 5000),
 CAST(REPLICATE(CAST('z' AS varchar(max)), 9000) AS varbinary(max)));
INSERT INTO dbo.types_row VALUES (4, 1, -32768, -2147483648, -9223372036854775808, 0, -999.99, -0.0001, -1, -0.000001, -1.5,
 -0.00000000000000000000000000000000000001, -2.5, -922337203685477.5808, -214748.3648, -1.18E-38,
 2.2250738585072014E-308, '1900-01-01', '12:00:00', '00:00:00.001', '00:00:00.0000001', '2026-11-16 10:11:12',
 '2026-11-16 10:11:12.123', '1900-01-01 00:00:00.0000001', '2026-11-16 10:11:12 +03:00',
 '2026-11-16 10:11:12.1234567 +05:30', '1753-01-01', '1900-01-01 00:01', 'a', 'abc   ', N'ab', N'abc',
 0x01, 0x00, '6F9619FF-8B86-D011-B42D-00C04FC964FF', 'short', N'короткий', 0x0102);
INSERT INTO dbo.types_row VALUES (5, 128, 128, -24, 4294967296, 1, 0.01, 1, 1000000000000000000, 1234567890123456789012.123456, 123456789,
 0.12345678901234567890123456789012345678, 3.14159, 12.34, 0.5, 0.5, 0.1, '2000-02-29', '00:00:01', '12:34:56.789',
 '12:34:56.7890123', '2000-02-29 12:00:00', '1999-12-31 23:59:59.999', '2026-11-16 10:11:12.1234567',
 '2026-11-16 10:11:12 -14:00', '0001-01-01 00:00:00 -00:01', '1899-12-31 23:59:59.997', '1900-01-02',
 '  lead', 'q', N'жук', N'Ελληνικά, עברית, العربية, ภาษาไทย, 한국어', 0x00000000000000FF, 0x0000,
 NULL, NULL, NULL, NULL);
INSERT INTO dbo.types_row VALUES (6, 127, -129, 65536, -1099511627777, NULL, 10.50, 1000000, 10, 10.000001, 0.0000000001, 0.1, 12345.00001,
 0.0001, -1, 100, 100, '0001-01-02', NULL, NULL, NULL, NULL, NULL, '1900-01-01 00:00:00.0000001', NULL, NULL,
 '1900-01-01 00:00:00.003', '2026-11-16 10:11', ' ', 'abcdefghij', N'𝄞x', N'mixedЖ日𝄞end', NULL, 0x01020304,
 NULL, REPLICATE('q', 300), N'𝄞𝄞', NULL);
INSERT INTO dbo.types_row VALUES (7, 2, 256, -16777217, 1099511627776, 1, 123.45, 12345678901234.5678, 1234567890123456789, 0, -9999999999999999999999999999.9999999999,
 0.00000000000000000000000000000000000001, 0, 1, 1, 1.401298E-45, 4.9E-324, '1753-01-01', '09:30:00', '09:30:00.500',
 '09:30:00.5000000', '1753-01-01 00:00:00', '1753-01-01 00:00:00.000', '1753-01-01 00:00:00.0000000',
 '1753-01-01 00:00:00 +00:00', '1753-01-01 00:00:00.0000000 +00:00', '1753-01-01 00:00:00', '1900-01-01 00:02',
 'ab        ', REPLICATE('v', 20), N'ÀÁÂ', N'line1' + NCHAR(13) + NCHAR(10) + N'line2', 0x0000000000000001,
 0x00, '11111111-2222-3333-4444-555555555555', NULL, N'abc' + NCHAR(0) + N'def', 0x);
GO
WITH n AS (SELECT TOP (200) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.types_row (id, ti, si, i, bi, b, d52, d184, m, f, dt, t7, dtm, sdt, c10, vc20, nc10, nv200, bn8, g)
SELECT 100 + i, i % 256, i - 100, i * 37 - 4000, CAST(i AS bigint) * 1000003 - 99999999, i % 2, i * 1.25 - 100,
	   i * 0.0137, i * 3.3333, i * 1.5E10 - 1.0E12, DATEADD(day, i * 97, '1800-01-01'),
	   DATEADD(ms, i * 123457, CAST('00:00:00' AS time(7))), DATEADD(minute, i * 997, '1890-01-01'),
	   DATEADD(minute, i * 1997, '1900-01-01'), REPLICATE(CHAR(65 + i % 26), i % 11),
	   CONCAT('v', i, REPLICATE(' ', i % 3)), REPLICATE(NCHAR(1072 + i % 32), i % 6),
	   REPLICATE(NCHAR(945 + i % 24) + N'x' + NCHAR(19968 + i), i % 7), CAST(i AS binary(8)), NEWID()
FROM n;
GO
SELECT * INTO dbo.types_plain FROM dbo.types_row;	-- the same rows, uncompressed
ALTER TABLE dbo.types_plain ADD CONSTRAINT pk_types_plain PRIMARY KEY (id);
GO
-- 2. Wide tables: clusters of 30 columns, the two-byte column count past 127.
DECLARE @n int, @sql nvarchar(max), @vals nvarchar(max), @i int;
DECLARE wide CURSOR LOCAL FOR SELECT v FROM (VALUES (31), (60), (129), (300)) AS w (v);
OPEN wide;
FETCH NEXT FROM wide INTO @n;
WHILE @@FETCH_STATUS = 0
BEGIN
	SET @sql = N'CREATE TABLE dbo.wide_' + CAST(@n AS nvarchar(10)) + N' (id int NOT NULL PRIMARY KEY';
	SET @vals = N'';
	SET @i = 1;
	WHILE @i <= @n
	BEGIN
		SET @sql += N', c' + CAST(@i AS nvarchar(10)) + N' int NULL';
		SET @vals += N', ' + CASE WHEN @i % 5 = 0 THEN N'NULL' WHEN @i % 7 = 0 THEN CAST(-@i * 1000 AS nvarchar(20))
							   ELSE CAST(@i AS nvarchar(20)) END;
		SET @i += 1;
	END;
	SET @sql += N') WITH (DATA_COMPRESSION = ROW);';
	EXEC (@sql);
	SET @sql = N'INSERT INTO dbo.wide_' + CAST(@n AS nvarchar(10)) + N' VALUES (1' + @vals + N');'
			 + N'INSERT INTO dbo.wide_' + CAST(@n AS nvarchar(10)) + N' (id) VALUES (2);';
	EXEC (@sql);
	FETCH NEXT FROM wide INTO @n;
END;
CLOSE wide;
DEALLOCATE wide;
GO
-- 3. Off row in compressed rows: row-overflow and MAX values beside in-row ones.
CREATE TABLE dbo.overflow_row (id int NOT NULL PRIMARY KEY, a varchar(6000) NULL, b varchar(6000) NULL, n nvarchar(4000) NULL)
	WITH (DATA_COMPRESSION = ROW);
INSERT INTO dbo.overflow_row VALUES (1, 'a', 'b', N'n'), (2, REPLICATE('a', 5000), REPLICATE('b', 5000), REPLICATE(N'ж', 3000)),
	(3, REPLICATE('c', 100), NULL, REPLICATE(N'n', 4000));
GO
-- 4. A ROW heap whose rows grew past their page: forwarded records and stubs.
CREATE TABLE dbo.heap_row (id int NOT NULL, v varchar(3000) NULL) WITH (DATA_COMPRESSION = ROW);
WITH n AS (SELECT TOP (300) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.heap_row SELECT i, CONCAT('s', i) FROM n;
UPDATE dbo.heap_row SET v = REPLICATE(CAST(CONCAT('g', id) AS varchar(10)), 300) WHERE id % 3 = 0;
GO
-- 5. A non-unique clustered index: the uniquifier, in a CD record.
CREATE TABLE dbo.uniq_row (k int NOT NULL, v varchar(20) NOT NULL, n int NULL);
CREATE CLUSTERED INDEX cx_uniq_row ON dbo.uniq_row (k) WITH (DATA_COMPRESSION = ROW);
WITH n AS (SELECT TOP (300) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.uniq_row SELECT i % 10, CONCAT('u', i), CASE WHEN i % 4 = 0 THEN NULL ELSE i END FROM n;
GO
-- 6. Ghosts, kept by trace flag 661.
CREATE TABLE dbo.ghost_row (id int NOT NULL PRIMARY KEY, v varchar(20) NOT NULL) WITH (DATA_COMPRESSION = ROW);
WITH n AS (SELECT TOP (100) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.ghost_row SELECT i, CONCAT('g', i) FROM n;
DELETE FROM dbo.ghost_row WHERE id % 4 = 0;
GO
-- 7. Columns added after rows were written: metadata-only, with defaults and without.
CREATE TABLE dbo.added_row (id int NOT NULL PRIMARY KEY, v int NULL) WITH (DATA_COMPRESSION = ROW);
INSERT INTO dbo.added_row VALUES (1, 10), (2, NULL);
ALTER TABLE dbo.added_row ADD n1 int NULL;
ALTER TABLE dbo.added_row ADD d1 int NOT NULL CONSTRAINT df_added_row DEFAULT 77;
ALTER TABLE dbo.added_row ADD s1 varchar(10) NOT NULL CONSTRAINT df_added_row_s DEFAULT 'zz';
ALTER TABLE dbo.added_row ADD m1 money NOT NULL CONSTRAINT df_added_row_m DEFAULT 1.25;
INSERT INTO dbo.added_row (id, v, n1, d1, s1, m1) VALUES (3, 30, 3, 33, 'yy', 3.5);
GO
-- 8. Keys that are not the first column, of the types whose key bytes _mssql_rid carries.
CREATE TABLE dbo.keys_row (a int NULL, bk bit NOT NULL, dk datetime NOT NULL, ck decimal(12, 3) NOT NULL, nk nvarchar(20) NOT NULL,
	CONSTRAINT pk_keys_row PRIMARY KEY (bk, dk, ck, nk)) WITH (DATA_COMPRESSION = ROW);
INSERT INTO dbo.keys_row VALUES (1, 0, '2026-11-16 10:00', 1.5, N'жук'), (2, 1, '1900-01-01', -2, N'a'),
	(NULL, 1, '1753-01-01', 0, N'𝄞'), (4, 0, '9999-12-31 23:59:59.997', 999999999.999, N'mixedЖ日');
SELECT * INTO dbo.keys_plain FROM dbo.keys_row;
ALTER TABLE dbo.keys_plain ADD CONSTRAINT pk_keys_plain PRIMARY KEY (bk, dk, ck, nk);
GO
-- 9. Per-partition compression: one partition ROW beside uncompressed ones; one table with a PAGE partition.
CREATE PARTITION FUNCTION pf_cd (int) AS RANGE RIGHT FOR VALUES (100, 200);
CREATE PARTITION SCHEME ps_cd AS PARTITION pf_cd ALL TO ([PRIMARY]);
CREATE TABLE dbo.part_row (k int NOT NULL, v varchar(20) NULL, n decimal(9, 2) NULL) ON ps_cd (k);
CREATE CLUSTERED INDEX cx_part_row ON dbo.part_row (k) WITH (DATA_COMPRESSION = ROW ON PARTITIONS (2)) ON ps_cd (k);
WITH n AS (SELECT TOP (300) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.part_row SELECT i, CONCAT('p', i), i * 1.5 FROM n;
CREATE TABLE dbo.part_page (k int NOT NULL, v varchar(20) NULL) ON ps_cd (k);
CREATE CLUSTERED INDEX cx_part_page ON dbo.part_page (k) WITH (DATA_COMPRESSION = PAGE ON PARTITIONS (3)) ON ps_cd (k);
INSERT INTO dbo.part_page SELECT k, v FROM dbo.part_row;
GO
-- 10. Nullable char / binary under ANSI_PADDING OFF: stored trimmed like any, read as given (not padded),
-- in a ROW table and an uncompressed twin; NOT NULL ones and nchar are padded whatever the setting.
SET ANSI_PADDING OFF;
GO
CREATE TABLE dbo.padoff_row (id int NOT NULL PRIMARY KEY, c char(10) NULL, b binary(8) NULL, n nchar(10) NULL,
	cn char(10) NOT NULL, bn binary(8) NOT NULL) WITH (DATA_COMPRESSION = ROW);
CREATE TABLE dbo.padoff_plain (id int NOT NULL PRIMARY KEY, c char(10) NULL, b binary(8) NULL, n nchar(10) NULL,
	cn char(10) NOT NULL, bn binary(8) NOT NULL);
GO
SET ANSI_PADDING ON;
GO
INSERT INTO dbo.padoff_row VALUES (1, 'abc', 0x01, N'жук', 'x', 0x02), (2, 'abc   ', 0x0100, N'ab  ', 'yy  ', 0x0300),
	(3, NULL, NULL, NULL, '', 0x),
	-- '' and 0x in the nullable ones: the server reads them as one space / one zero byte (DATALENGTH 1),
	-- from either record (review of #626).
	(4, '', 0x, N'', 'z', 0x04);
INSERT INTO dbo.padoff_plain SELECT * FROM dbo.padoff_row;
GO
-- 11. rowversion (big-endian, its leading zero bytes left out), and the types a ROW record keeps as stored --
-- sql_variant, xml, hierarchyid, geometry -- in a ROW table and an uncompressed twin (their stored bytes alike).
CREATE TABLE dbo.rv_row (id int NOT NULL PRIMARY KEY, v int NULL, rv rowversion) WITH (DATA_COMPRESSION = ROW);
INSERT INTO dbo.rv_row (id, v) VALUES (1, 1), (2, 2), (3, 3);
UPDATE dbo.rv_row SET v = v + 10 WHERE id = 2;
CREATE TABLE dbo.stored_row (id int NOT NULL PRIMARY KEY, sv sql_variant NULL, x xml NULL, h hierarchyid NULL, g geometry NULL)
	WITH (DATA_COMPRESSION = ROW);
-- One row a statement: a VALUES list would convert every sql_variant value to the list's one type.
INSERT INTO dbo.stored_row VALUES (1, CAST(1 AS int), '<a>x</a>', '/1/', geometry::STGeomFromText('POINT (1 2)', 0));
INSERT INTO dbo.stored_row VALUES (2, CAST(N'жук' AS nvarchar(10)), '<b/>', '/', NULL);
INSERT INTO dbo.stored_row VALUES (3, CAST(123.45 AS decimal(9, 2)), NULL, '/1/2/', geometry::STGeomFromText('LINESTRING (0 0, 3 4)', 0));
INSERT INTO dbo.stored_row VALUES (4, CAST('2026-11-16' AS datetime), NULL, NULL, NULL);
SELECT * INTO dbo.stored_plain FROM dbo.stored_row;
ALTER TABLE dbo.stored_plain ADD CONSTRAINT pk_stored_plain PRIMARY KEY (id);
-- text / ntext / image in a ROW record (review of #569): the value's 16-byte text pointer stands in the
-- row, a NULL, an empty, a short and a value past a page each, beside the uncompressed twin.
CREATE TABLE dbo.textptr_row (id int NOT NULL PRIMARY KEY, tx text NULL, ntx ntext NULL, im image NULL)
	WITH (DATA_COMPRESSION = ROW);
INSERT INTO dbo.textptr_row VALUES (1, NULL, NULL, NULL);
INSERT INTO dbo.textptr_row VALUES (2, '', N'', 0x);
INSERT INTO dbo.textptr_row VALUES (3, 'abc', N'жук', 0x0102);
INSERT INTO dbo.textptr_row VALUES (4, REPLICATE(CAST('t' AS varchar(max)), 9000), REPLICATE(CAST(N'ж' AS nvarchar(max)), 5000),
 CAST(REPLICATE(CAST('i' AS varchar(max)), 9000) AS varbinary(max)));
SELECT * INTO dbo.textptr_plain FROM dbo.textptr_row;
ALTER TABLE dbo.textptr_plain ADD CONSTRAINT pk_textptr_plain PRIMARY KEY (id);
GO
-- 12. Row versioning (RCSI): every record written from here on carries a 14-byte version tag; updated rows,
-- ghosts of a clustered table, version ghosts of a heap, forwarded rows of a heap.
ALTER DATABASE bak_cd SET ALLOW_SNAPSHOT_ISOLATION ON;
ALTER DATABASE bak_cd SET READ_COMMITTED_SNAPSHOT ON WITH ROLLBACK IMMEDIATE;
GO
CREATE TABLE dbo.ver_row (id int NOT NULL PRIMARY KEY, v int NOT NULL, s nvarchar(20) NULL) WITH (DATA_COMPRESSION = ROW);
WITH n AS (SELECT TOP (200) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.ver_row SELECT i, i, CONCAT(N'в', i) FROM n;
UPDATE dbo.ver_row SET v = v + 1000 WHERE id % 2 = 0;
DELETE FROM dbo.ver_row WHERE id % 7 = 0;
CREATE TABLE dbo.ver_heap_row (id int NOT NULL, v varchar(3000) NULL) WITH (DATA_COMPRESSION = ROW);
WITH n AS (SELECT TOP (200) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.ver_heap_row SELECT i, CONCAT('s', i) FROM n;
UPDATE dbo.ver_heap_row SET v = REPLICATE(CAST(CONCAT('g', id) AS varchar(10)), 300) WHERE id % 3 = 0;
DELETE FROM dbo.ver_heap_row WHERE id % 5 = 0;
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_cd TO DISK = '$(DIR)/cd_full.bak' WITH FORMAT, INIT, CHECKSUM, NAME = 'cd_full';
GO
BACKUP DATABASE bak_cd TO DISK = '$(DIR)/cd_xpress.bak' WITH FORMAT, INIT, CHECKSUM, COMPRESSION, COPY_ONLY, NAME = 'cd_xpress';
GO
DBCC TRACEOFF (661, -1);
GO
