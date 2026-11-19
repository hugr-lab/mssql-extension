-- Spec 085 PR 4c-3: a benchmark of mssql_backup_table_changes against a whole scan, for local
-- measurement only (not run by default; about 700 MB of data). A table of 10 million rows in a full
-- backup, then three rounds of changes, each followed by a differential (cumulative since the full):
-- about 10 thousand rows changed, then 110 thousand, then 1.1 million (updates, with a few inserts and
-- deletes beside them). Beside it, `lobs`: a million rows with a varchar(max) stored off row (about
-- 1 KB each), its values changed in place in the same rounds (1 thousand, then 10 thousand, then
-- 100 thousand rows) -- what a change to a value stored off row costs to find (LOB pages rewritten,
-- the data pages not). Last, `big`'s clustered index rebuilt and a thousand rows updated: every row on
-- a new page, so each is paired across the table (what an index rebuild costs to compare).
SET NOCOUNT ON;
SET QUOTED_IDENTIFIER ON;
GO
IF DB_ID('bak_bench') IS NOT NULL DROP DATABASE bak_bench;
GO
CREATE DATABASE bak_bench;
GO
ALTER DATABASE bak_bench SET RECOVERY SIMPLE;
ALTER DATABASE bak_bench MODIFY FILE (NAME = 'bak_bench', SIZE = 4GB);
GO
USE bak_bench;
GO
CREATE TABLE dbo.big (id int NOT NULL PRIMARY KEY, a int NOT NULL, b varchar(40) NULL, c datetime2(3) NULL,
	d decimal(12, 2) NULL);
CREATE TABLE dbo.lobs (id int NOT NULL PRIMARY KEY, a int NOT NULL, v varchar(max) NULL);
EXEC sp_tableoption 'dbo.lobs', 'large value types out of row', 1;
GO
WITH d AS (SELECT TOP (10) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) - 1 AS n FROM sys.all_objects),
	n AS (SELECT d1.n + 10 * d2.n + 100 * d3.n + 1000 * d4.n + 10000 * d5.n + 100000 * d6.n + 1000000 * d7.n + 1 AS i
		  FROM d d1, d d2, d d3, d d4, d d5, d d6, d d7)
INSERT INTO dbo.big WITH (TABLOCK)
SELECT i, i % 9973, CONCAT('row ', i, ' of the benchmark'), DATEADD(second, i, '2026-01-01'), i / 100.0 FROM n;
GO
WITH d AS (SELECT TOP (10) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) - 1 AS n FROM sys.all_objects),
	n AS (SELECT d1.n + 10 * d2.n + 100 * d3.n + 1000 * d4.n + 10000 * d5.n + 100000 * d6.n + 1 AS i
		  FROM d d1, d d2, d d3, d d4, d d5, d d6)
INSERT INTO dbo.lobs WITH (TABLOCK) SELECT i, i, REPLICATE(CAST(CONCAT('lob ', i, ' ') AS varchar(max)), 70) FROM n;
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_bench TO DISK = '$(DIR)/bench_full.bak' WITH FORMAT, INIT, NAME = 'bench_full';
-- Its compressed twin, copy-only so the differentials' base stays bench_full.
BACKUP DATABASE bak_bench TO DISK = '$(DIR)/bench_full_x.bak' WITH FORMAT, INIT, COPY_ONLY, COMPRESSION, NAME = 'bench_full_x';
GO
UPDATE dbo.big SET a = a + 1, b = CONCAT(b, ' u1') WHERE id % 1000 = 0;
UPDATE dbo.lobs SET v.WRITE('X', 0, 1) WHERE id % 1000 = 0;
DELETE FROM dbo.big WHERE id % 100000 = 7;
INSERT INTO dbo.big SELECT 10000000 + id, 1, 'new', NULL, NULL FROM dbo.big WHERE id <= 100;
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_bench TO DISK = '$(DIR)/bench_diff_10k.bak' WITH FORMAT, INIT, DIFFERENTIAL, NAME = 'bench_diff_10k';
BACKUP DATABASE bak_bench TO DISK = '$(DIR)/bench_diff_10k_x.bak' WITH FORMAT, INIT, DIFFERENTIAL, COMPRESSION, NAME = 'bench_diff_10k_x';
GO
UPDATE dbo.big SET a = a + 1 WHERE id % 100 = 1;
UPDATE dbo.lobs SET v.WRITE('Y', 0, 1) WHERE id % 100 = 1;
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_bench TO DISK = '$(DIR)/bench_diff_110k.bak' WITH FORMAT, INIT, DIFFERENTIAL, NAME = 'bench_diff_110k';
BACKUP DATABASE bak_bench TO DISK = '$(DIR)/bench_diff_110k_x.bak' WITH FORMAT, INIT, DIFFERENTIAL, COMPRESSION, NAME = 'bench_diff_110k_x';
GO
UPDATE dbo.big SET a = a + 1 WHERE id % 10 = 2;
UPDATE dbo.lobs SET v.WRITE('Z', 0, 1) WHERE id % 10 = 2;
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_bench TO DISK = '$(DIR)/bench_diff_1m.bak' WITH FORMAT, INIT, DIFFERENTIAL, NAME = 'bench_diff_1m';
BACKUP DATABASE bak_bench TO DISK = '$(DIR)/bench_diff_1m_x.bak' WITH FORMAT, INIT, DIFFERENTIAL, COMPRESSION, NAME = 'bench_diff_1m_x';
GO
ALTER INDEX ALL ON dbo.big REBUILD;
GO
UPDATE dbo.big SET a = a + 1 WHERE id % 10000 = 3;
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_bench TO DISK = '$(DIR)/bench_diff_rebuild.bak' WITH FORMAT, INIT, DIFFERENTIAL, NAME = 'bench_diff_rebuild';
BACKUP DATABASE bak_bench TO DISK = '$(DIR)/bench_diff_rebuild_x.bak' WITH FORMAT, INIT, DIFFERENTIAL, COMPRESSION, NAME = 'bench_diff_rebuild_x';
GO
