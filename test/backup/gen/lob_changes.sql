-- Spec 085 PR 4c-3 (task 24d, DQ-47) and the LOB tail: values stored off row changed in every form, and
-- what each change rewrites. A full, then each form of change on its own row, rows left alone beside them,
-- then the differential (lob_changes_diff: no shrink, pages stay where they are -- a change to an off-row
-- value leaves its data row's page and LSN as they were, measured), then a shrink that moves the LOB
-- pages and rewrites the pointers to them, and a second differential over the same full
-- (lob_changes_shrink). The forms:
--   lobs      root pointers ('large value types out of row'): a varchar(max) replaced whole, .WRITE
--             appended, .WRITE in place (the length kept), UPDATETEXT on a text, a varbinary(max)
--             replaced at its length;
--   inrow     a small varchar(max) in row grown off row (an in-row root);
--   inroot    in-row roots (a varchar(max) past 8,000 bytes, in row allowed): replaced whole, appended,
--             overwritten in place, set to itself, a row deleted, one inserted;
--   overflow  row-overflow (two varchar(5000) past the 8,060-byte row): one replaced at its length, one
--             shortened back in row, the fixed column alone updated, the other replaced, one set to itself;
--   small     root pointers to values under 8,000 bytes: replaced, overwritten in place, set to itself, set
--             NULL, set from NULL;
--   texts     text: WRITETEXT, UPDATETEXT inserting, UPDATETEXT deleting;
--   dropped   a varchar(max) dropped before the full: its values stay in the records, its pointers are
--             rewritten by the shrink, and nothing changed;
--   dropped_inrow  the same with no value off row: a char(200) dropped, the records moved by the shrink;
--   renamed   a column copied into a new one and dropped between the full and the differential (the
--             same value under another column id): every row updated;
--   defaults  a bit added with a default before the full (the records hold none), the table rebuilt
--             after it (the records hold it now): nothing changed;
--   filler    dropped after the first differential, so the shrink has room to move pages into.
-- For local development (and 4c-3's tests); not run by default.
SET NOCOUNT ON;
SET QUOTED_IDENTIFIER ON;
GO
IF DB_ID('bak_lob_changes') IS NOT NULL DROP DATABASE bak_lob_changes;
GO
CREATE DATABASE bak_lob_changes;
GO
ALTER DATABASE bak_lob_changes SET RECOVERY FULL;
GO
USE bak_lob_changes;
GO
CREATE TABLE dbo.filler (id int NOT NULL PRIMARY KEY, pad char(8000) NOT NULL);
GO
WITH n AS (SELECT TOP (600) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.filler SELECT i, 'f' FROM n;
GO
CREATE TABLE dbo.lobs (id int NOT NULL PRIMARY KEY, vmax varchar(max) NULL, t text NULL, b varbinary(max) NULL);
EXEC sp_tableoption 'dbo.lobs', 'large value types out of row', 1;
CREATE TABLE dbo.inrow (id int NOT NULL PRIMARY KEY, vmax varchar(max) NULL);  -- in row until it grows
CREATE TABLE dbo.inroot (id int NOT NULL PRIMARY KEY, v varchar(max) NULL);
CREATE TABLE dbo.overflow (id int NOT NULL PRIMARY KEY, a varchar(5000) NULL, b varchar(5000) NULL, c int NULL);
CREATE TABLE dbo.small (id int NOT NULL PRIMARY KEY, v varchar(max) NULL);
EXEC sp_tableoption 'dbo.small', 'large value types out of row', 1;
CREATE TABLE dbo.texts (id int NOT NULL PRIMARY KEY, t text NULL);
CREATE TABLE dbo.dropped (id int NOT NULL PRIMARY KEY, keep varchar(max) NULL, gone varchar(max) NULL);
EXEC sp_tableoption 'dbo.dropped', 'large value types out of row', 1;
CREATE TABLE dbo.dropped_inrow (id int NOT NULL PRIMARY KEY, keep int NULL, gone char(200) NULL);
CREATE TABLE dbo.renamed (id int NOT NULL PRIMARY KEY, l varchar(max) NULL, b varchar(50) NULL);
EXEC sp_tableoption 'dbo.renamed', 'large value types out of row', 1;
CREATE TABLE dbo.defaults (id int NOT NULL PRIMARY KEY, l varchar(max) NULL);
EXEC sp_tableoption 'dbo.defaults', 'large value types out of row', 1;
GO
WITH n AS (SELECT TOP (8) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.lobs SELECT i, REPLICATE(CAST(CONCAT('v', i) AS varchar(max)), 4000), REPLICATE(CONCAT('t', i), 2000),
	CAST(REPLICATE(CAST('b' AS varchar(max)), 9000) AS varbinary(max)) FROM n;
WITH n AS (SELECT TOP (8) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.inrow SELECT i, CONCAT('small', i) FROM n;
WITH n AS (SELECT TOP (6) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.inroot SELECT i, REPLICATE(CAST(CONCAT('r', i) AS varchar(max)), 10000) FROM n;
WITH n AS (SELECT TOP (6) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.overflow SELECT i, REPLICATE(CONCAT('a', i % 10), 2500), REPLICATE(CONCAT('b', i % 10), 2500), i FROM n;
WITH n AS (SELECT TOP (6) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.small SELECT i, CASE WHEN i = 5 THEN NULL ELSE REPLICATE(CAST(CONCAT('s', i) AS varchar(max)), 100) END FROM n;
WITH n AS (SELECT TOP (4) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.texts SELECT i, REPLICATE(CONCAT('x', i), 1500) FROM n;
WITH n AS (SELECT TOP (4) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.dropped SELECT i, REPLICATE(CAST(CONCAT('k', i) AS varchar(max)), 4500), REPLICATE(CAST(CONCAT('g', i) AS varchar(max)), 4500) FROM n;
WITH n AS (SELECT TOP (200) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.dropped_inrow SELECT i, i, CONCAT('gone', i) FROM n;
WITH n AS (SELECT TOP (4) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.renamed SELECT i, REPLICATE(CAST(CONCAT('l', i) AS varchar(max)), 4500), CONCAT('b', i) FROM n;
WITH n AS (SELECT TOP (4) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.defaults SELECT i, REPLICATE(CAST(CONCAT('d', i) AS varchar(max)), 4500) FROM n;
GO
ALTER TABLE dbo.dropped DROP COLUMN gone;  -- metadata only: the values stay in the records
ALTER TABLE dbo.dropped_inrow DROP COLUMN gone;
ALTER TABLE dbo.defaults ADD flag bit NOT NULL CONSTRAINT df_defaults_flag DEFAULT 1;  -- metadata only
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_lob_changes TO DISK = '$(DIR)/lob_changes_full.bak' WITH FORMAT, INIT, CHECKSUM, NAME = 'lob_changes_full';
GO
UPDATE dbo.lobs SET vmax = REPLICATE(CAST('V1' AS varchar(max)), 4000) WHERE id = 1;
UPDATE dbo.lobs SET vmax.WRITE('appended', NULL, NULL) WHERE id = 2;
DECLARE @p varbinary(16) = (SELECT TEXTPTR(t) FROM dbo.lobs WHERE id = 3);
UPDATETEXT dbo.lobs.t @p 0 2 'XX';
GO
UPDATE dbo.lobs SET b = CAST(REPLICATE(CAST('c' AS varchar(max)), 9000) AS varbinary(max)) WHERE id = 4;
UPDATE dbo.lobs SET vmax.WRITE('Z', 0, 1) WHERE id = 5;   -- an in-place overwrite, the length kept
UPDATE dbo.inrow SET vmax = REPLICATE(CAST('g' AS varchar(max)), 20000) WHERE id = 1;  -- grown off row
GO
UPDATE dbo.inroot SET v = REPLICATE(CAST('R1' AS varchar(max)), 10000) WHERE id = 1;
UPDATE dbo.inroot SET v.WRITE('appended', NULL, NULL) WHERE id = 2;
UPDATE dbo.inroot SET v.WRITE('Z', 100, 1) WHERE id = 3;
UPDATE dbo.inroot SET v = v WHERE id = 4;
DELETE FROM dbo.inroot WHERE id = 5;
INSERT INTO dbo.inroot VALUES (7, REPLICATE(CAST('n7' AS varchar(max)), 10000));
GO
UPDATE dbo.overflow SET b = REPLICATE('B1', 2500) WHERE id = 1;
UPDATE dbo.overflow SET b = 'short' WHERE id = 2;
UPDATE dbo.overflow SET c = -c WHERE id = 3;
UPDATE dbo.overflow SET a = REPLICATE('A4', 2500) WHERE id = 4;
UPDATE dbo.overflow SET b = b WHERE id = 5;
GO
UPDATE dbo.small SET v = REPLICATE(CAST('S1' AS varchar(max)), 100) WHERE id = 1;
UPDATE dbo.small SET v.WRITE('Z', 10, 1) WHERE id = 2;
UPDATE dbo.small SET v = v WHERE id = 3;
UPDATE dbo.small SET v = NULL WHERE id = 4;
UPDATE dbo.small SET v = REPLICATE(CAST('s5' AS varchar(max)), 100) WHERE id = 5;
GO
DECLARE @w varbinary(16) = (SELECT TEXTPTR(t) FROM dbo.texts WHERE id = 1);
WRITETEXT dbo.texts.t @w 'written whole';
DECLARE @i varbinary(16) = (SELECT TEXTPTR(t) FROM dbo.texts WHERE id = 2);
UPDATETEXT dbo.texts.t @i 10 0 'abc';
DECLARE @d varbinary(16) = (SELECT TEXTPTR(t) FROM dbo.texts WHERE id = 3);
UPDATETEXT dbo.texts.t @d 10 5;
GO
UPDATE dbo.dropped SET keep = REPLICATE(CAST('K1' AS varchar(max)), 4500) WHERE id = 1;
GO
ALTER TABLE dbo.renamed ADD c varchar(50) NULL;
GO
UPDATE dbo.renamed SET c = b;
ALTER TABLE dbo.renamed DROP COLUMN b;
ALTER TABLE dbo.defaults REBUILD;
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_lob_changes TO DISK = '$(DIR)/lob_changes_diff.bak' WITH FORMAT, INIT, CHECKSUM, DIFFERENTIAL, NAME = 'lob_changes_diff';
GO
DROP TABLE dbo.filler;
GO
CHECKPOINT;
GO
DBCC SHRINKFILE (1, 1) WITH NO_INFOMSGS;
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_lob_changes TO DISK = '$(DIR)/lob_changes_shrink.bak' WITH FORMAT, INIT, CHECKSUM, DIFFERENTIAL, NAME = 'lob_changes_shrink';
GO
