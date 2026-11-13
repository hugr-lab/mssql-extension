-- Spec 085 PR 3: values stored off row, for the oracle lane and local development only (the committed
-- samples are samples.sql's). The forms E21 measured, sized for the lane: text / ntext / image with text
-- in row off and on (values from empty to past 1 MiB, then updated to NULL -- a NULL root -- rewritten
-- small and grown), MAX columns under `large value types out of row` set after rows, MAX values in a heap
-- and a clustered table across every tree size to level 1 (8,000 / 8,040 / 40,200 / 4.1 MB), row-overflow
-- of every bounded type a sql_variant included and a row of 8,060 / 8,061 bytes, a MAX value pushed off
-- by a full row, a heap whose grown rows were forwarded beside their LOBs, .WRITE updates (a truncate
-- and edits changing a value's length mid-value), UPDATETEXT / WRITETEXT on text, xml off row, a tree of
-- level 2 (21 MB), a value a byte short of the scan's 1 MiB threshold, ghosts of
-- deleted LOB rows kept by trace flag 661, a double-byte character / a surrogate pair / a 4-byte UTF-8
-- character across a fragment's end and across the scan's 64 KiB decode block, a partitioned table with a
-- partition rebuilt, and a table of values of a mebibyte and more (the scan's path by value: copied or
-- decoded straight into the vector). Two sets: plain and compressed (MS_XPRESS: by name from 2022).
SET NOCOUNT ON;
GO
DBCC TRACEON (661, -1);	-- no ghost cleanup: deleted rows stay as ghosts
GO
IF DB_ID('bak_lob') IS NOT NULL DROP DATABASE bak_lob;
GO
CREATE DATABASE bak_lob;
GO
ALTER DATABASE bak_lob MODIFY FILE (NAME = bak_lob, SIZE = 200MB);
GO
USE bak_lob;
GO
CREATE FUNCTION dbo.s(@n int) RETURNS varchar(max) AS
BEGIN RETURN LEFT(REPLICATE(CAST('abcdefghij' AS varchar(max)), (@n + 9) / 10), @n) END;
GO
CREATE FUNCTION dbo.ns(@n int) RETURNS nvarchar(max) AS
BEGIN RETURN LEFT(REPLICATE(CAST(N'абвгдежзик' AS nvarchar(max)), (@n + 9) / 10), @n) END;
GO
CREATE FUNCTION dbo.b(@n int) RETURNS varbinary(max) AS
BEGIN RETURN CAST(dbo.s(@n) AS varbinary(max)) END;
GO
-- 1. text / ntext / image, text in row off: every value behind a 16-byte pointer to a root record.
CREATE TABLE dbo.legacy (id int NOT NULL PRIMARY KEY, t text NULL, n ntext NULL, i image NULL);
INSERT INTO dbo.legacy VALUES (1, NULL, NULL, NULL), (2, '', N'', 0x);
INSERT INTO dbo.legacy SELECT v.id, dbo.s(v.n), dbo.ns(v.n), dbo.b(v.n)
FROM (VALUES (3, 1), (4, 64), (5, 65), (6, 200), (7, 8000), (8, 8041), (9, 100000), (10, 1200000)) v (id, n);
UPDATE dbo.legacy SET t = NULL, n = NULL, i = NULL WHERE id IN (6, 9);	-- NULL roots, the null bits clear
UPDATE dbo.legacy SET t = 'x', i = 0x78 WHERE id = 4;	-- a SMALL_ROOT rewritten in place
UPDATE dbo.legacy SET t = dbo.s(9000), n = dbo.ns(9000) WHERE id = 5;	-- grown past its root
GO
-- 2. text in row on (set after rows, one updated after), then off again.
CREATE TABLE dbo.inrow (id int NOT NULL PRIMARY KEY, t text NULL, n ntext NULL, i image NULL);
INSERT INTO dbo.inrow VALUES (1, dbo.s(10), dbo.ns(10), dbo.b(10)), (2, dbo.s(200), dbo.ns(100), dbo.b(200));
EXEC sp_tableoption 'dbo.inrow', 'text in row', '256';
INSERT INTO dbo.inrow VALUES (11, dbo.s(10), dbo.ns(10), dbo.b(10)), (12, dbo.s(200), dbo.ns(100), dbo.b(200)),
	(13, dbo.s(300), dbo.ns(150), dbo.b(300)), (14, dbo.s(6000), dbo.ns(3000), dbo.b(6000)), (15, '', N'', 0x),
	(16, NULL, NULL, NULL);
UPDATE dbo.inrow SET t = dbo.s(200) WHERE id = 2;
UPDATE dbo.inrow SET t = NULL, i = NULL WHERE id = 14;	-- NULL over an in-row root
CREATE TABLE dbo.inrow_off (id int NOT NULL PRIMARY KEY, t text NULL);
EXEC sp_tableoption 'dbo.inrow_off', 'text in row', '256';
INSERT INTO dbo.inrow_off VALUES (1, dbo.s(10)), (2, dbo.s(300));
EXEC sp_tableoption 'dbo.inrow_off', 'text in row', 'OFF';
INSERT INTO dbo.inrow_off VALUES (3, dbo.s(10));
GO
-- 3. MAX columns with large value types out of row, set after rows; old rows half updated; to NULL.
CREATE TABLE dbo.maxout (id int NOT NULL PRIMARY KEY, v varchar(max) NULL, nv nvarchar(max) NULL, b varbinary(max) NULL);
INSERT INTO dbo.maxout VALUES (1, dbo.s(10), dbo.ns(10), dbo.b(10)), (2, dbo.s(9000), dbo.ns(4500), dbo.b(9000)),
	(3, dbo.s(10), dbo.ns(10), dbo.b(10)), (4, '', N'', 0x);
EXEC sp_tableoption 'dbo.maxout', 'large value types out of row', 1;
INSERT INTO dbo.maxout VALUES (11, dbo.s(10), dbo.ns(10), dbo.b(10)), (12, dbo.s(1100000), dbo.ns(600000), dbo.b(1100000)),
	(13, '', N'', 0x), (14, NULL, NULL, NULL), (15, dbo.s(70), dbo.ns(40), dbo.b(70));
UPDATE dbo.maxout SET v = dbo.s(20) WHERE id = 3;
UPDATE dbo.maxout SET v = NULL, b = NULL WHERE id = 15;
GO
-- 4. MAX values in a heap and in a clustered table: in row, at the fragment and root-link limits, deeper.
CREATE TABLE dbo.max_heap (id int NOT NULL, v varchar(max) NULL, nv nvarchar(max) NULL, b varbinary(max) NULL);
CREATE TABLE dbo.max_cl (id int NOT NULL PRIMARY KEY, v varchar(max) NULL, nv nvarchar(max) NULL, b varbinary(max) NULL);
DECLARE @sizes TABLE (id int, n int);
INSERT INTO @sizes VALUES (1, 0), (2, 10), (3, 7999), (4, 8000), (5, 8001), (6, 8040), (7, 8041), (8, 16081),
	(9, 40200), (10, 40201), (11, 200000), (12, 1048576), (13, 4100000), (15, 1048575);
INSERT INTO dbo.max_heap SELECT id, dbo.s(n), dbo.ns(n / 2), dbo.b(n) FROM @sizes;
INSERT INTO dbo.max_cl SELECT id, dbo.s(n), dbo.ns(n / 2), dbo.b(n) FROM @sizes;
INSERT INTO dbo.max_heap VALUES (14, NULL, NULL, NULL);
INSERT INTO dbo.max_cl VALUES (14, NULL, NULL, NULL);
INSERT INTO dbo.max_cl VALUES (16, dbo.s(21000000), NULL, NULL);	-- a tree of level 2
GO
-- 5. Row-overflow: 0 to 4 bounded columns pushed off (a sql_variant among them); 8,060 and 8,061 bytes.
CREATE TABLE dbo.ovf (id int NOT NULL PRIMARY KEY, a varchar(8000) NULL, b nvarchar(4000) NULL,
	c varbinary(8000) NULL, d sql_variant NULL, e varchar(5000) NULL, f char(100) NULL);
INSERT INTO dbo.ovf VALUES (1, dbo.s(10), dbo.ns(10), dbo.b(10), 1, dbo.s(10), 'f');
INSERT INTO dbo.ovf VALUES (2, dbo.s(8000), dbo.ns(10), dbo.b(10), 1, dbo.s(10), 'f');
INSERT INTO dbo.ovf VALUES (3, dbo.s(8000), dbo.ns(4000), dbo.b(8000), 1, dbo.s(10), 'f');
INSERT INTO dbo.ovf VALUES (4, dbo.s(8000), dbo.ns(4000), dbo.b(8000),
	CAST(CAST(dbo.s(7000) AS varchar(7000)) AS sql_variant), dbo.s(5000), 'f');
UPDATE dbo.ovf SET a = dbo.s(10) WHERE id = 3;	-- pulled back in row
CREATE TABLE dbo.ov_exact (id int NOT NULL PRIMARY KEY, a varchar(8000) NULL, b varchar(8000) NULL);
INSERT INTO dbo.ov_exact VALUES (1, dbo.s(8000), dbo.s(43)), (2, dbo.s(8000), dbo.s(44));
CREATE TABLE dbo.max_full_row (id int NOT NULL PRIMARY KEY, c char(6000) NOT NULL, m varchar(max) NULL);
INSERT INTO dbo.max_full_row VALUES (1, 'c', dbo.s(3000)), (2, 'c', dbo.s(1500));
GO
-- 6. A heap whose grown rows were forwarded, their LOBs untouched.
CREATE TABLE dbo.heap_fwd_lob (id int NOT NULL, v varchar(3000) NULL, m varchar(max) NULL);
WITH n AS (SELECT TOP (60) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.heap_fwd_lob SELECT i, CONCAT('s', i), dbo.s(20000 + i) FROM n;
UPDATE dbo.heap_fwd_lob SET v = REPLICATE(CAST(CONCAT('g', id) AS varchar(10)), 300) WHERE id % 3 = 0;
GO
-- 7. .WRITE: middle, append, truncate; in row to off row and back; SET m = m.
CREATE TABLE dbo.upd (id int NOT NULL PRIMARY KEY, m varchar(max) NULL);
INSERT INTO dbo.upd VALUES (1, dbo.s(20000)), (2, dbo.s(630000)), (3, dbo.s(100)), (4, dbo.s(50000)), (5, dbo.s(20000));
UPDATE dbo.upd SET m.WRITE('MIDDLE', 10000, 6) WHERE id = 1;
UPDATE dbo.upd SET m.WRITE(dbo.s(5000), NULL, 0) WHERE id = 2;	-- an append
UPDATE dbo.upd SET m.WRITE(NULL, 30000, NULL) WHERE id = 4;	-- a truncate
UPDATE dbo.upd SET m = dbo.s(9000) WHERE id = 3;
UPDATE dbo.upd SET m = dbo.s(30) WHERE id = 3;
UPDATE dbo.upd SET m = m WHERE id = 5;
INSERT INTO dbo.upd VALUES (6, dbo.s(50000)), (7, dbo.s(50000));
UPDATE dbo.upd SET m.WRITE('X', 10000, 500) WHERE id = 6;	-- 500 bytes replaced by 1, mid-value
UPDATE dbo.upd SET m.WRITE(dbo.s(3000), 20000, 10) WHERE id = 7;	-- 10 bytes replaced by 3,000, mid-value
GO
-- 7b. UPDATETEXT and WRITETEXT on legacy text: a mid-value delete and insert, a replacement.
CREATE TABLE dbo.legacy_edit (id int NOT NULL PRIMARY KEY, t text NULL, i image NULL);
INSERT INTO dbo.legacy_edit VALUES (1, dbo.s(100000), dbo.b(100000)), (2, dbo.s(30000), dbo.b(30000)),
	(3, dbo.s(50000), NULL);
DECLARE @p varbinary(16);
SELECT @p = TEXTPTR(t) FROM dbo.legacy_edit WHERE id = 1;
UPDATETEXT dbo.legacy_edit.t @p 30000 5000 'Z';	-- 5,000 bytes deleted at 30,000, 'Z' in their place
SELECT @p = TEXTPTR(i) FROM dbo.legacy_edit WHERE id = 1;
UPDATETEXT dbo.legacy_edit.i @p 8000 100 0x5A5A;
SELECT @p = TEXTPTR(t) FROM dbo.legacy_edit WHERE id = 2;
WRITETEXT dbo.legacy_edit.t @p 'replaced';
SELECT @p = TEXTPTR(t) FROM dbo.legacy_edit WHERE id = 3;
UPDATETEXT dbo.legacy_edit.t @p 49000 NULL '';	-- truncated at 49,000
GO
-- 8. Characters across a fragment's end (8,040 bytes) and across the scan's 64 KiB decode block of a value
--    past a mebibyte: a double-byte character, a UTF-16 surrogate pair, a 4-byte UTF-8 one (2019+).
CREATE TABLE dbo.dbcs_lob (id int NOT NULL PRIMARY KEY,
	j varchar(max) COLLATE Japanese_CI_AS NULL, c varchar(max) COLLATE Chinese_PRC_CI_AS NULL,
	k varchar(max) COLLATE Korean_Wansung_CI_AS NULL, tw varchar(max) COLLATE Chinese_Taiwan_Stroke_CI_AS NULL,
	tj text COLLATE Japanese_CI_AS NULL, e nvarchar(max) NULL);
INSERT INTO dbo.dbcs_lob SELECT x.id,
	REPLICATE(CAST(N'a' AS nvarchar(max)), x.p) + REPLICATE(CAST(N'漢字かな' AS nvarchar(max)), x.r),
	REPLICATE(CAST(N'a' AS nvarchar(max)), x.p) + REPLICATE(CAST(N'中文汉字' AS nvarchar(max)), x.r),
	REPLICATE(CAST(N'a' AS nvarchar(max)), x.p) + REPLICATE(CAST(N'한국어' AS nvarchar(max)), x.r),
	REPLICATE(CAST(N'a' AS nvarchar(max)), x.p) + REPLICATE(CAST(N'中文漢字' AS nvarchar(max)), x.r),
	REPLICATE(CAST(N'a' AS nvarchar(max)), x.p) + REPLICATE(CAST(N'漢字かな' AS nvarchar(max)), x.r),
	REPLICATE(CAST(N'a' AS nvarchar(max)), x.q) + REPLICATE(CAST(N'a😀' AS nvarchar(max)), x.r)
FROM (VALUES (1, 8039, 4019, 300), (2, 8038, 4018, 300), (3, 1, 1, 200000)) x (id, p, q, r);	-- q 4018: a pair cut at 8,040
IF CAST(SERVERPROPERTY('ProductMajorVersion') AS int) >= 15
	EXEC (N'CREATE TABLE dbo.utf8_lob (id int NOT NULL PRIMARY KEY, u varchar(max) COLLATE Latin1_General_100_CI_AS_SC_UTF8 NULL);
		INSERT INTO dbo.utf8_lob SELECT x.id, REPLICATE(CAST(N''a'' AS nvarchar(max)), x.p) + REPLICATE(CAST(N''a😀é'' AS nvarchar(max)), x.r)
		FROM (VALUES (1, 8037, 300), (2, 1, 200000)) x (id, p, r);');
GO
-- 8b. xml off row: its stored bytes (MS-BINXML, spec 086) are read and mapped like any other value.
CREATE TABLE dbo.xml_lob (id int NOT NULL PRIMARY KEY, x xml NULL);
INSERT INTO dbo.xml_lob SELECT 1, CAST('<r>' + dbo.s(20000) + '</r>' AS xml);
INSERT INTO dbo.xml_lob VALUES (2, '<r>small</r>'), (3, NULL);
GO
-- 9. A partitioned table, one partition rebuilt (its rowset's own in-row and row-overflow units).
CREATE PARTITION FUNCTION pf_lob (int) AS RANGE LEFT FOR VALUES (100, 200);
CREATE PARTITION SCHEME ps_lob AS PARTITION pf_lob ALL TO ([PRIMARY]);
CREATE TABLE dbo.part_lob (id int NOT NULL, m varchar(max) NULL, x varchar(6000) NULL, y varchar(6000) NULL,
	CONSTRAINT pk_part_lob PRIMARY KEY CLUSTERED (id) ON ps_lob (id)) ON ps_lob (id);
INSERT INTO dbo.part_lob VALUES (1, dbo.s(100000), dbo.s(6000), dbo.s(6000)), (2, dbo.s(20), dbo.s(10), dbo.s(10)),
	(150, dbo.s(100000), dbo.s(6000), dbo.s(6000)), (151, dbo.s(9000), dbo.s(10), dbo.s(10)),
	(250, dbo.s(100000), dbo.s(6000), dbo.s(6000)), (251, dbo.s(9000), dbo.s(10), dbo.s(10));
ALTER INDEX pk_part_lob ON dbo.part_lob REBUILD PARTITION = 2;
GO
-- 10. Ghosts of deleted LOB rows (trace flag 661): never walked.
CREATE TABLE dbo.ghost_lob (id int NOT NULL PRIMARY KEY, m varchar(max) NULL);
WITH n AS (SELECT TOP (40) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.ghost_lob SELECT i, dbo.s(10000 + i) FROM n;
DELETE FROM dbo.ghost_lob WHERE id % 4 = 0;
GO
-- 11. Values of a mebibyte and more, several to a chunk: copied (varbinary) and decoded (varchar,
--     nvarchar) straight into the vector, beside short ones.
CREATE TABLE dbo.many_lob (id int NOT NULL, b varbinary(max) NULL, v varchar(max) NULL, nv nvarchar(max) NULL);
WITH n AS (SELECT TOP (24) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.many_lob SELECT i,
	CASE WHEN i % 2 = 0 THEN dbo.b(1048576 + i) ELSE dbo.b(100 + i) END,
	CASE WHEN i % 3 = 0 THEN dbo.s(1048576 + i) ELSE CONCAT(N'é', i) END,
	CASE WHEN i % 4 = 0 THEN dbo.ns(600000 + i) ELSE CONCAT(N'ж', i) END
FROM n;
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_lob TO DISK = '$(DIR)/lob_full.bak' WITH FORMAT, INIT, CHECKSUM, NAME = 'lob_full';
GO
DECLARE @major int = CAST(SERVERPROPERTY('ProductMajorVersion') AS int);
IF @major >= 16
	EXEC ('BACKUP DATABASE bak_lob TO DISK = ''$(DIR)/lob_xpress.bak''
		WITH FORMAT, INIT, COPY_ONLY, COMPRESSION (ALGORITHM = MS_XPRESS), NAME = ''lob_xpress''');
ELSE
	BACKUP DATABASE bak_lob TO DISK = '$(DIR)/lob_xpress.bak' WITH FORMAT, INIT, COPY_ONLY, COMPRESSION, NAME = 'lob_xpress';
GO
DBCC TRACEOFF (661, -1);
GO
