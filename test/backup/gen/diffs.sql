-- Spec 085 PR 4c (task 24, D12): a differential over its full, for the oracle lane and local development
-- only. A full, then the changes E2 found a naive overlay gets wrong -- updates, deletes and inserts
-- that split pages, a heap whose rows grow past their page (forwarded records), values stored off row
-- rewritten, a table dropped, one truncated, columns added and dropped, a table created, an index
-- rebuilt (new allocation units), and most of a table deleted then the file shrunk (pages moved: a
-- naive overlay doubled every row after a shrink) -- then the differential, plain and compressed (both
-- cumulative since the full, so both read alike). compare_rows restores the full WITH NORECOVERY and
-- each differential WITH RECOVERY and compares mssql_backup_scan(full, diff := …) with it.
SET NOCOUNT ON;
SET QUOTED_IDENTIFIER ON;
GO
IF DB_ID('bak_diffs') IS NOT NULL DROP DATABASE bak_diffs;
GO
CREATE DATABASE bak_diffs;
GO
ALTER DATABASE bak_diffs SET RECOVERY FULL;
GO
USE bak_diffs;
GO
CREATE TABLE dbo.keyed (id int NOT NULL PRIMARY KEY, v int NULL, s nvarchar(200) NULL);
CREATE TABLE dbo.heap (id int NOT NULL, s varchar(3000) NULL);
CREATE TABLE dbo.lob (id int NOT NULL PRIMARY KEY, t varchar(max) NULL, b varbinary(max) NULL);
CREATE TABLE dbo.dropped (id int NOT NULL PRIMARY KEY, v int NULL);
CREATE TABLE dbo.truncated (id int NOT NULL PRIMARY KEY, v int NULL);
CREATE TABLE dbo.altered (id int NOT NULL PRIMARY KEY, old_col int NULL, keep_col varchar(20) NULL);
CREATE TABLE dbo.rebuilt (id int NOT NULL PRIMARY KEY, v int NULL, pad char(200) NULL);
CREATE TABLE dbo.shrunk (id int NOT NULL PRIMARY KEY, pad char(2000) NULL);
GO
WITH n AS (SELECT TOP (5000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.keyed SELECT i, i, CONCAT(N'k', i) FROM n;
WITH n AS (SELECT TOP (2000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.heap SELECT i, CONCAT('h', i) FROM n;
WITH n AS (SELECT TOP (50) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.lob SELECT i, REPLICATE(CAST(CONCAT('l', i) AS varchar(max)), 3000), CAST(REPLICATE(CAST('b' AS varchar(max)), 9000) AS varbinary(max)) FROM n;
WITH n AS (SELECT TOP (300) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.dropped SELECT i, i FROM n;
WITH n AS (SELECT TOP (300) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.truncated SELECT i, i FROM n;
WITH n AS (SELECT TOP (300) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.altered SELECT i, i, CONCAT('a', i) FROM n;
WITH n AS (SELECT TOP (2000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.rebuilt SELECT i, i, 'r' FROM n;
WITH n AS (SELECT TOP (3000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.shrunk SELECT i, 's' FROM n;
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_diffs TO DISK = '$(DIR)/diffs_full.bak' WITH FORMAT, INIT, CHECKSUM, NAME = 'diffs_full';
GO
-- The changes after the full.
UPDATE dbo.keyed SET v = v + 1000000, s = CONCAT(s, REPLICATE(N'u', 150)) WHERE id % 7 = 0;	-- rows grow: page splits
DELETE FROM dbo.keyed WHERE id % 11 = 0;
WITH n AS (SELECT TOP (1500) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.keyed SELECT 100000 + i, -i, N'new' FROM n;
UPDATE dbo.heap SET s = REPLICATE(CAST(CONCAT('g', id) AS varchar(10)), 280) WHERE id % 3 = 0;  -- forwarded records
DELETE FROM dbo.heap WHERE id % 5 = 0;
UPDATE dbo.lob SET t = REPLICATE(CAST(CONCAT('L', id) AS varchar(max)), 5000) WHERE id % 2 = 0;
UPDATE dbo.lob SET b = NULL WHERE id % 5 = 0;
DROP TABLE dbo.dropped;
TRUNCATE TABLE dbo.truncated;
INSERT INTO dbo.truncated VALUES (1, -1), (2, -2), (3, -3);
ALTER TABLE dbo.altered ADD added_col int NULL;
ALTER TABLE dbo.altered DROP COLUMN old_col;
GO
UPDATE dbo.altered SET added_col = id * 10 WHERE id % 4 = 0;
CREATE TABLE dbo.created (id int NOT NULL PRIMARY KEY, s nvarchar(20) NULL);
INSERT INTO dbo.created VALUES (1, N'c1'), (2, N'c2');
ALTER INDEX ALL ON dbo.rebuilt REBUILD;
UPDATE dbo.rebuilt SET v = -v WHERE id % 10 = 0;
DELETE FROM dbo.shrunk WHERE id > 100;
GO
CHECKPOINT;
GO
DBCC SHRINKFILE (1, 1) WITH NO_INFOMSGS;
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_diffs TO DISK = '$(DIR)/diffs_diff.bak' WITH FORMAT, INIT, CHECKSUM, DIFFERENTIAL, NAME = 'diffs_diff';
GO
DECLARE @major int = CAST(SERVERPROPERTY('ProductMajorVersion') AS int);
IF @major >= 16
	EXEC ('BACKUP DATABASE bak_diffs TO DISK = ''$(DIR)/diffs_diff_xpress.bak''
		WITH FORMAT, INIT, CHECKSUM, DIFFERENTIAL, COMPRESSION (ALGORITHM = MS_XPRESS), NAME = ''diffs_diff_xpress''');
ELSE
	BACKUP DATABASE bak_diffs TO DISK = '$(DIR)/diffs_diff_xpress.bak' WITH FORMAT, INIT, CHECKSUM, DIFFERENTIAL, COMPRESSION, NAME = 'diffs_diff_xpress';
GO
