-- Spec 085 PR 2d: the record forms of user tables E20 measured, for the oracle lane and local
-- development only. Row versioning on (ALLOW_SNAPSHOT_ISOLATION, READ_COMMITTED_SNAPSHOT), so every
-- record written carries a version tag; trace flag 661 (no ghost cleanup, global: this throwaway
-- server only, turned off at the end) keeps deleted rows as ghosts. A heap with forwarded records and
-- stubs; a non-unique clustered index (the uniquifier); updated rows; ghosts of a clustered table and
-- of a heap (a version ghost); row-overflow and off-row MAX values beside in-row ones; a partitioned
-- table with columns added after rows were written (a default per partition rowset); bits sharing a
-- byte; NOT NULL fixed columns only.
SET NOCOUNT ON;
GO
DBCC TRACEON (661, -1);	-- no ghost cleanup: deleted rows stay as ghosts
GO
IF DB_ID('bak_rows') IS NOT NULL DROP DATABASE bak_rows;
GO
CREATE DATABASE bak_rows;
GO
ALTER DATABASE bak_rows SET ALLOW_SNAPSHOT_ISOLATION ON;
ALTER DATABASE bak_rows SET READ_COMMITTED_SNAPSHOT ON WITH ROLLBACK IMMEDIATE;
GO
USE bak_rows;
GO
-- 1. A heap whose rows grew past their page: forwarded records and their stubs.
CREATE TABLE dbo.fwd (id int NOT NULL, v varchar(3000) NULL);
WITH n AS (SELECT TOP (400) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.fwd SELECT i, CONCAT('s', i) FROM n;
UPDATE dbo.fwd SET v = REPLICATE(CAST(CONCAT('g', id) AS varchar(10)), 300) WHERE id % 3 = 0;
-- 2. A non-unique clustered index: the uniquifier.
CREATE TABLE dbo.uniq (k int NOT NULL, v varchar(20) NOT NULL, n int NULL);
CREATE CLUSTERED INDEX cx_uniq ON dbo.uniq (k);
WITH n AS (SELECT TOP (300) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.uniq SELECT i % 10, CONCAT('u', i), CASE WHEN i % 4 = 0 THEN NULL ELSE i END FROM n;
-- 3. Version tags: rows updated (and inserted) under row versioning.
CREATE TABLE dbo.ver (id int NOT NULL PRIMARY KEY, v int NOT NULL, s varchar(20) NULL);
WITH n AS (SELECT TOP (200) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.ver SELECT i, i, CONCAT('v', i) FROM n;
UPDATE dbo.ver SET v = v + 1000 WHERE id % 2 = 0;
CREATE TABLE dbo.ver_heap (id int NOT NULL, s char(10) NOT NULL);
INSERT INTO dbo.ver_heap SELECT TOP (50) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)), 'h' FROM sys.all_objects;
UPDATE dbo.ver_heap SET s = 'x' WHERE id % 5 = 0;
-- 4. Ghosts: deleted rows, kept by trace flag 661.
CREATE TABLE dbo.ghost (id int NOT NULL PRIMARY KEY, v varchar(20) NOT NULL);
WITH n AS (SELECT TOP (100) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.ghost SELECT i, CONCAT('g', i) FROM n;
DELETE FROM dbo.ghost WHERE id % 4 = 0;
CREATE TABLE dbo.ghost_heap (id int NOT NULL, v varchar(20) NOT NULL);
INSERT INTO dbo.ghost_heap SELECT id, v FROM dbo.ghost;
DELETE FROM dbo.ghost_heap WHERE id % 3 = 0;
-- 5. Complex columns: row overflow and an off-row MAX value beside in-row ones.
CREATE TABLE dbo.wide (id int NOT NULL PRIMARY KEY, a varchar(6000) NULL, b varchar(6000) NULL, m varchar(max) NULL, nm nvarchar(max) NULL);
INSERT INTO dbo.wide VALUES (1, 'a', 'b', 'short', N'короткий');
INSERT INTO dbo.wide VALUES (2, REPLICATE('a', 5000), REPLICATE('b', 5000), REPLICATE(CAST('m' AS varchar(max)), 20000), NULL);
INSERT INTO dbo.wide VALUES (3, REPLICATE('a', 100), NULL, REPLICATE(CAST('n' AS varchar(max)), 9000), REPLICATE(CAST(N'ж' AS nvarchar(max)), 5000));
-- 6. A column added with a default to a partitioned table that has rows (2c's open question).
CREATE PARTITION FUNCTION pf_rows (int) AS RANGE RIGHT FOR VALUES (100, 200);
CREATE PARTITION SCHEME ps_rows AS PARTITION pf_rows ALL TO ([PRIMARY]);
CREATE TABLE dbo.part_add (k int NOT NULL, v varchar(20) NULL) ON ps_rows (k);
WITH n AS (SELECT TOP (300) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.part_add SELECT i, CONCAT('p', i) FROM n;
ALTER TABLE dbo.part_add ADD added int NOT NULL CONSTRAINT df_part_add DEFAULT 7;
ALTER TABLE dbo.part_add ADD added_s varchar(10) NOT NULL CONSTRAINT df_part_add_s DEFAULT 'zz';
INSERT INTO dbo.part_add (k, v, added) VALUES (150, 'after', 8);
-- 7. A table of only NOT NULL fixed columns (a null bitmap anyway?) and one of a single bit.
CREATE TABLE dbo.notnull (a int NOT NULL, b bigint NOT NULL);
INSERT INTO dbo.notnull VALUES (1, 2), (3, 4);
CREATE TABLE dbo.bits (b1 bit NULL, b2 bit NOT NULL, i int NULL, b3 bit NULL, b4 bit NULL, b5 bit NULL, b6 bit NULL, b7 bit NULL, b8 bit NULL, b9 bit NULL);
INSERT INTO dbo.bits VALUES (1, 0, 5, 1, 0, 1, 0, 1, 0, 1), (NULL, 1, NULL, 0, 1, 0, 1, 0, 1, 0);
-- 8. A column dropped and others added after rows were written, and one widened: the catalog column ids
-- and the rowset's physical columns (sysrscols' rscolid / hbcolid) diverge, and each column must still
-- read its own bytes (review of #552). Then a clustered table with the same history.
CREATE TABLE dbo.dropadd (a int NOT NULL, b int NULL, c varchar(10) NULL, w char(4) NULL);
INSERT INTO dbo.dropadd VALUES (1, 10, 'c1', 'w1'), (2, 20, 'c2', 'w2'), (3, NULL, NULL, NULL);
ALTER TABLE dbo.dropadd DROP COLUMN b;
ALTER TABLE dbo.dropadd ADD d int NULL;
ALTER TABLE dbo.dropadd ADD e int NOT NULL CONSTRAINT df_dropadd_e DEFAULT 5;
ALTER TABLE dbo.dropadd ALTER COLUMN w char(8) NULL;
-- Added after the widening, so its physical id (hbcolid) is past its column id (rscolid): which one
-- sysseobjvalues keys its default by is what this row tells.
ALTER TABLE dbo.dropadd ADD f int NOT NULL CONSTRAINT df_dropadd_f DEFAULT 9;
INSERT INTO dbo.dropadd (a, c, w, d, e, f) VALUES (4, 'c4', 'w4', 40, 41, 42);
UPDATE dbo.dropadd SET d = 30 WHERE a = 3;
CREATE TABLE dbo.dropadd_cl (a int NOT NULL PRIMARY KEY, b int NULL, c varchar(10) NULL);
INSERT INTO dbo.dropadd_cl VALUES (1, 10, 'c1'), (2, 20, 'c2');
ALTER TABLE dbo.dropadd_cl DROP COLUMN b;
ALTER TABLE dbo.dropadd_cl ADD d int NULL;
INSERT INTO dbo.dropadd_cl (a, c, d) VALUES (3, 'c3', 30);
-- A fixed char narrowed after rows were written: does its physical width follow the declaration?
CREATE TABLE dbo.narrowed (id int NOT NULL, s char(8) NULL, t int NULL);
INSERT INTO dbo.narrowed VALUES (1, 'ab', 10), (2, 'cdef', 20), (3, NULL, 30);
ALTER TABLE dbo.narrowed ALTER COLUMN s char(4) NULL;
INSERT INTO dbo.narrowed VALUES (4, 'gh', 40);
-- 9. A partitioned table with a non-unique clustered index on another column than the partitioning one:
-- rows of one key in two partitions, each partition numbering its uniquifiers apart (review of #552).
CREATE TABLE dbo.part_nu (k int NOT NULL, v int NOT NULL, x varchar(10) NULL) ON ps_rows (k);
CREATE CLUSTERED INDEX cx_part_nu ON dbo.part_nu (v) ON ps_rows (k);
INSERT INTO dbo.part_nu VALUES (5, 7, 'a'), (150, 7, 'b'), (250, 7, 'c'), (6, 7, 'd'), (151, 7, 'e');
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_rows TO DISK = '$(DIR)/rows_full.bak' WITH FORMAT, INIT, CHECKSUM, NAME = 'rows_full';
GO
DBCC TRACEOFF (661, -1);
GO
