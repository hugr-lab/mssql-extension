-- Spec 085 PR 4c-3 (task 24c, DQ-47 / DQ-49): what mssql_backup_schema_changes says of the changes the
-- catalog alone shows, for the oracle lane and local development only. A full, then: a table of more
-- than 128 extents truncated (a deferred drop) and nothing inserted; a small one truncated and nothing
-- inserted; a clustered columnstore truncated; every row of a table deleted (no TRUNCATE: no event); a
-- table allocated late, its pages moved by a shrink once an early one is dropped (no event); a
-- partitioned table's sliding window -- its oldest partition switched out to an archive table the full
-- already has, the low boundary merged, a new one split at the top (a switch out, the archive's switch
-- in, nothing else) -- then the differential. Not in overlays.txt: a columnstore is not read yet
-- (spec 088), so compare_rows does not take this pair.
SET NOCOUNT ON;
SET QUOTED_IDENTIFIER ON;
GO
IF DB_ID('bak_schema') IS NOT NULL DROP DATABASE bak_schema;
GO
CREATE DATABASE bak_schema;
GO
ALTER DATABASE bak_schema SET RECOVERY FULL;
GO
USE bak_schema;
GO
-- Allocated first, dropped later: the space a shrink moves `late` into.
CREATE TABLE dbo.filler (id int NOT NULL PRIMARY KEY, pad char(7000) NULL);
GO
WITH n AS (SELECT TOP (1500) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.filler SELECT i, 'f' FROM n;
GO
CREATE TABLE dbo.big_truncated (id int NOT NULL PRIMARY KEY, pad char(7000) NULL);   -- a page a row
CREATE TABLE dbo.small_truncated (id int NOT NULL PRIMARY KEY, v int NULL);
CREATE TABLE dbo.columnstore_truncated (id int NOT NULL, v int NULL, INDEX cci CLUSTERED COLUMNSTORE);
CREATE TABLE dbo.deleted_all (id int NOT NULL PRIMARY KEY, v int NULL);
CREATE TABLE dbo.late (id int NOT NULL PRIMARY KEY, pad char(3000) NULL);
GO
WITH n AS (SELECT TOP (2000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.big_truncated SELECT i, 'b' FROM n;
WITH n AS (SELECT TOP (300) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.small_truncated SELECT i, i FROM n;
WITH n AS (SELECT TOP (2000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.columnstore_truncated SELECT i, i FROM n;
WITH n AS (SELECT TOP (300) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.deleted_all SELECT i, i FROM n;
WITH n AS (SELECT TOP (600) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.late SELECT i, 'l' FROM n;
GO
-- The sliding window: monthly partitions, Jan..Apr, and an archive table of the same shape.
CREATE PARTITION FUNCTION pf_month (date) AS RANGE RIGHT FOR VALUES ('2026-02-01', '2026-03-01', '2026-04-01');
CREATE PARTITION SCHEME ps_month AS PARTITION pf_month ALL TO ([PRIMARY]);
GO
CREATE TABLE dbo.events (d date NOT NULL, id int NOT NULL, v int NULL, CONSTRAINT pk_events PRIMARY KEY (d, id))
	ON ps_month (d);
CREATE TABLE dbo.events_archive (d date NOT NULL, id int NOT NULL, v int NULL,
	CONSTRAINT pk_events_archive PRIMARY KEY (d, id));
GO
WITH n AS (SELECT TOP (400) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.events SELECT DATEADD(day, (i - 1) % 110, CAST('2026-01-01' AS date)), i, i FROM n;
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_schema TO DISK = '$(DIR)/schema_full.bak' WITH FORMAT, INIT, CHECKSUM, NAME = 'schema_full';
GO
-- The changes after the full.
TRUNCATE TABLE dbo.big_truncated;
TRUNCATE TABLE dbo.small_truncated;
TRUNCATE TABLE dbo.columnstore_truncated;
DELETE FROM dbo.deleted_all;
GO
-- January out to the archive (a constraint on the archive's date range is not needed for a SWITCH TO an
-- unpartitioned table), its boundary merged away, May split in at the top.
ALTER TABLE dbo.events SWITCH PARTITION 1 TO dbo.events_archive;
ALTER PARTITION FUNCTION pf_month() MERGE RANGE ('2026-02-01');
ALTER PARTITION SCHEME ps_month NEXT USED [PRIMARY];
ALTER PARTITION FUNCTION pf_month() SPLIT RANGE ('2026-05-01');
GO
DROP TABLE dbo.filler;
GO
CHECKPOINT;
GO
DBCC SHRINKFILE (1, 1) WITH NO_INFOMSGS;
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_schema TO DISK = '$(DIR)/schema_diff.bak' WITH FORMAT, INIT, CHECKSUM, DIFFERENTIAL, NAME = 'schema_diff';
GO
