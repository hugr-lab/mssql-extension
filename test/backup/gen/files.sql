-- Spec 085 PR 2b: the files fixture (oracle lane and local development). A database of two data files
-- in PRIMARY and a second filegroup: a table whose extents proportional fill spreads over both
-- PRIMARY files (its IAM chain a page per file: E19), a table on the second filegroup, a heap. Backed
-- up, then its second PRIMARY file emptied and removed and backed up again: the chains that mapped
-- the removed file leave their sequence numbers unrenumbered (E19). Synthetic rows only (T4).
SET NOCOUNT ON;
GO
IF DB_ID('bak_files') IS NOT NULL DROP DATABASE bak_files;
GO
CREATE DATABASE bak_files ON PRIMARY
	(NAME = bak_files, FILENAME = '/var/opt/mssql/data/bak_files.mdf', SIZE = 64MB),
	(NAME = bak_files_2, FILENAME = '/var/opt/mssql/data/bak_files_2.ndf', SIZE = 64MB),
	FILEGROUP fg2 (NAME = bak_files_fg2, FILENAME = '/var/opt/mssql/data/bak_files_fg2.ndf', SIZE = 16MB)
	LOG ON (NAME = bak_files_log, FILENAME = '/var/opt/mssql/data/bak_files_log.ldf', SIZE = 64MB);
GO
USE bak_files;
GO
CREATE TABLE dbo.wide (id int NOT NULL PRIMARY KEY, pad char(4000) NOT NULL);
CREATE TABLE dbo.on_fg2 (id int NOT NULL PRIMARY KEY, note nvarchar(100) NULL) ON fg2;
CREATE TABLE dbo.heap_both (id bigint NOT NULL, v varchar(200) NULL);
GO
WITH n AS (SELECT TOP (6000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.wide SELECT i, CONCAT('r', i) FROM n;
WITH n AS (SELECT TOP (500) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.on_fg2 SELECT i, CONCAT(N'fg2 ', i) FROM n;
WITH n AS (SELECT TOP (20000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.heap_both SELECT i, REPLICATE('h', 150) FROM n;
GO
BACKUP DATABASE bak_files TO DISK = '$(DIR)/files_full.bak' WITH FORMAT, INIT, CHECKSUM, NAME = 'files_full';
GO
