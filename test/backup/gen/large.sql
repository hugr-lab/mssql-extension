-- Spec 085 PR 2b: the large fixture (oracle lane): a data file whose data passes one GAM interval
-- (511,232 pages, E17), so its pages lie in two and the second interval's GAM / SGAM / PFS are read
-- with data behind them. A heap of one row per page, loaded minimally logged (SIMPLE, TABLOCK); the
-- rows compress well, so its compressed backup is small. LARGE_ROWS (sqlcmd variable) sets the rows:
-- 530000 (about 4.1 GB) by default, fewer for a quick local run that stays in one interval.
SET NOCOUNT ON;
GO
IF DB_ID('bak_large') IS NOT NULL DROP DATABASE bak_large;
GO
CREATE DATABASE bak_large ON PRIMARY (NAME = bak_large, FILENAME = '/var/opt/mssql/data/bak_large.mdf', SIZE = 4400MB,
	FILEGROWTH = 256MB)
	LOG ON (NAME = bak_large_log, FILENAME = '/var/opt/mssql/data/bak_large_log.ldf', SIZE = 256MB);
ALTER DATABASE bak_large SET RECOVERY SIMPLE;
GO
USE bak_large;
GO
CREATE TABLE dbo.pages (id int NOT NULL, fill char(7900) NOT NULL);
GO
WITH n AS (SELECT TOP ($(LARGE_ROWS)) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b CROSS JOIN sys.all_objects c)
INSERT INTO dbo.pages WITH (TABLOCK) (id, fill) SELECT i, CONCAT('p', i) FROM n;
GO
CREATE TABLE dbo.tail (id int NOT NULL PRIMARY KEY, v varchar(50) NOT NULL);	-- allocated after the heap
INSERT INTO dbo.tail SELECT TOP (1000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)), 't' FROM sys.all_objects;
GO
BACKUP DATABASE bak_large TO DISK = '$(DIR)/large_xpress.bak' WITH FORMAT, INIT, CHECKSUM, COMPRESSION, NAME = 'large_xpress';
GO
