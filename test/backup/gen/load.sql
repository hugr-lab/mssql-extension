-- Spec 085 PR 2b: the load fixture's database (oracle lane and local development). generate.sh runs
-- this, then load_workload.sql in the background (inserts, updates and deletes committing in a loop),
-- then its backups while the workload runs -- a backup of a live database: pages changed while the
-- backup ran (a data pass 2), transactions open at its end (a fuzzy tail, spec 090), and a data file
-- that autogrows while it is copied, so the file header's recorded Size falls behind the data (the
-- Size a lower bound: BusinessCentral.DbReader's PROVENANCE). Synthetic rows only (T4).
SET NOCOUNT ON;
GO
IF DB_ID('bak_load') IS NOT NULL DROP DATABASE bak_load;
GO
-- A small growth step, so the file grows many times while the backup runs.
CREATE DATABASE bak_load ON PRIMARY (NAME = bak_load, FILENAME = '/var/opt/mssql/data/bak_load.mdf', SIZE = 256MB,
	FILEGROWTH = 8MB)
	LOG ON (NAME = bak_load_log, FILENAME = '/var/opt/mssql/data/bak_load_log.ldf', SIZE = 256MB, FILEGROWTH = 64MB);
ALTER DATABASE bak_load SET RECOVERY FULL;
GO
USE bak_load;
GO
CREATE TABLE dbo.control (stop_requested bit NOT NULL, ticks bigint NOT NULL);
INSERT INTO dbo.control VALUES (0, 0);
CREATE TABLE dbo.events (id bigint IDENTITY(1, 1) PRIMARY KEY, at datetime2(3) NOT NULL, payload char(2000) NOT NULL,
	status tinyint NOT NULL);
CREATE TABLE dbo.journal (id bigint NOT NULL, note varchar(500) NULL);	-- a heap
GO
-- About 300 MB before the backups start, so they take long enough to overlap the workload.
WITH n AS (SELECT TOP (120000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.events (at, payload, status) SELECT DATEADD(SECOND, i, '2026-01-01'), CONCAT('e', i), i % 4 FROM n;
GO
-- The log chain starts here (a full backup the workload's log backup can follow).
BACKUP DATABASE bak_load TO DISK = '$(DIR)/load_base.bak' WITH FORMAT, INIT, CHECKSUM, NAME = 'load_base';
GO
