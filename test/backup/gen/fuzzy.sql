-- Spec 085 PR 5 (task 27): the fuzzy fixture's database, E3's controls (research/experiments/E3-fuzzy-backups.md
-- § 1) on every version. generate.sh runs this (the tables and a quiet base backup), then fuzzy_workload.sql in
-- the background (short transactions committing, one in twenty rolled back), then fuzzy_open.sql (one transaction
-- left open across the backups), then a committed change of pre_commit, then fuzzy_backup.sql under all of it.
-- What each table is for:
--   busy        clustered: updated, appended, deleted by the workload (stale); rows the open transaction changes
--               too, in a key range the workload never reads (dirty)
--   heap        a heap with a nonclustered index: updated (rows grow: forwarding), appended
--   lob         values stored off row, rewritten whole
--   control     the workload's counter, the generator's flags
--   static      never changed after the base backup: consistent
--   open_only   changed only by the open transaction, before the backups: dirty
--   pre_commit  changed and committed after the open transaction began, before the backups: its records are in
--               the tail, older than its pages' copies, so consistent (flag (iv) must not fire on them)
--   rolled_only changed only by the workload's rolled-back transactions: stale only where a page was copied
--               between a change and its undo, else equal to the RESTORE (review of #591: it read stale)
-- Synthetic rows only (T4).
SET NOCOUNT ON;
GO
IF DB_ID('bak_fuzzy') IS NOT NULL DROP DATABASE bak_fuzzy;
GO
CREATE DATABASE bak_fuzzy ON PRIMARY (NAME = bak_fuzzy, FILENAME = '/var/opt/mssql/data/bak_fuzzy.mdf', SIZE = 256MB,
	FILEGROWTH = 64MB)
	LOG ON (NAME = bak_fuzzy_log, FILENAME = '/var/opt/mssql/data/bak_fuzzy_log.ldf', SIZE = 256MB, FILEGROWTH = 64MB);
ALTER DATABASE bak_fuzzy SET RECOVERY FULL;
GO
USE bak_fuzzy;
GO
CREATE TABLE dbo.control (stop_requested bit NOT NULL, ticks bigint NOT NULL, backups_done bit NOT NULL);
INSERT INTO dbo.control VALUES (0, 0, 0);
CREATE TABLE dbo.busy (id bigint IDENTITY(1, 1) PRIMARY KEY, v int NOT NULL, pad char(500) NOT NULL);
CREATE TABLE dbo.heap (id bigint NOT NULL, v int NOT NULL, pad varchar(900) NOT NULL);
CREATE UNIQUE NONCLUSTERED INDEX heap_id ON dbo.heap (id);
CREATE TABLE dbo.lob (id int PRIMARY KEY, doc varchar(max) NOT NULL);
CREATE TABLE dbo.static (id int PRIMARY KEY, v int NOT NULL);
CREATE TABLE dbo.open_only (id int PRIMARY KEY, v int NOT NULL);
CREATE TABLE dbo.pre_commit (id int PRIMARY KEY, v int NOT NULL);
CREATE TABLE dbo.rolled_only (id int PRIMARY KEY, v int NOT NULL, pad char(200) NOT NULL);
GO
-- About 400 MB, so a backup throttled to one 64 KB buffer runs long enough for commits to land after its
-- checkpoint.
WITH n AS (SELECT TOP (300000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.busy (v, pad) SELECT i % 1000, CONCAT('b', i) FROM n;
WITH n AS (SELECT TOP (240000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.heap (id, v, pad) SELECT i, i % 1000, CONCAT('h', i) FROM n;
WITH n AS (SELECT TOP (3000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.lob (id, doc) SELECT i, REPLICATE(CONVERT(varchar(max), CONCAT('L', i % 10)), 9000 + i * 7) FROM n;
WITH n AS (SELECT TOP (5000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.static (id, v) SELECT i, i FROM n;
WITH n AS (SELECT TOP (5000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.open_only (id, v) SELECT i, i FROM n;
WITH n AS (SELECT TOP (5000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.pre_commit (id, v) SELECT i, i FROM n;
WITH n AS (SELECT TOP (30000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.rolled_only (id, v, pad) SELECT i, i, CONCAT('r', i) FROM n;
GO
-- The base of the differential, taken quiet (the log chain starts here too).
BACKUP DATABASE bak_fuzzy TO DISK = '$(DIR)/fuzzy_base.bak' WITH FORMAT, INIT, CHECKSUM, NAME = 'fuzzy_base';
GO
