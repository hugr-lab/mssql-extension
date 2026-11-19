-- Spec 085 (DQ-41 revisited): differentials of other layouts than one plain file, for local development
-- only. A database of two PRIMARY files and a second filegroup; a full in two stripes; changes in every
-- file; then differentials over that full -- to one file, in two stripes, in two stripes compressed,
-- and of the filegroup fg2 alone, to one file and in two stripes (a size cap, as a maintenance script's
-- @MaxFileSize makes one: more stripes). What the DCM-computed map of a differential must match: its extents are those marked in
-- each file's DCM, allocated by its GAM or holding an allocation page, within the file's size.
SET NOCOUNT ON;
SET QUOTED_IDENTIFIER ON;
GO
IF DB_ID('bak_dlayout') IS NOT NULL DROP DATABASE bak_dlayout;
GO
CREATE DATABASE bak_dlayout ON PRIMARY
	(NAME = bak_dlayout, FILENAME = '/var/opt/mssql/data/bak_dlayout.mdf', SIZE = 32MB),
	(NAME = bak_dlayout_2, FILENAME = '/var/opt/mssql/data/bak_dlayout_2.ndf', SIZE = 32MB),
	FILEGROUP fg2 (NAME = bak_dlayout_fg2, FILENAME = '/var/opt/mssql/data/bak_dlayout_fg2.ndf', SIZE = 16MB)
	LOG ON (NAME = bak_dlayout_log, FILENAME = '/var/opt/mssql/data/bak_dlayout_log.ldf', SIZE = 64MB);
GO
ALTER DATABASE bak_dlayout SET RECOVERY FULL;
GO
USE bak_dlayout;
GO
CREATE TABLE dbo.wide (id int NOT NULL PRIMARY KEY, pad char(2000) NOT NULL);
CREATE TABLE dbo.on_fg2 (id int NOT NULL PRIMARY KEY, note nvarchar(200) NULL) ON fg2;
CREATE TABLE dbo.heap_both (id bigint NOT NULL, v varchar(200) NULL);
GO
WITH n AS (SELECT TOP (8000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.wide SELECT i, CONCAT('r', i) FROM n;
WITH n AS (SELECT TOP (3000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.on_fg2 SELECT i, CONCAT(N'fg2 ', i) FROM n;
WITH n AS (SELECT TOP (20000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.heap_both SELECT i, REPLICATE('h', 150) FROM n;
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_dlayout TO DISK = '$(DIR)/dlayout_full_1.bak', DISK = '$(DIR)/dlayout_full_2.bak'
	WITH FORMAT, INIT, CHECKSUM, NAME = 'dlayout_full';
GO
UPDATE dbo.wide SET pad = CONCAT('u', id) WHERE id % 7 = 0;
DELETE FROM dbo.on_fg2 WHERE id % 5 = 0;
UPDATE dbo.on_fg2 SET note = CONCAT(N'changed ', id) WHERE id % 11 = 0;
UPDATE dbo.heap_both SET v = REPLICATE('x', 190) WHERE id % 13 = 0;
WITH n AS (SELECT TOP (2000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.wide SELECT 100000 + i, 'new' FROM n;
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_dlayout TO DISK = '$(DIR)/dlayout_diff.bak' WITH FORMAT, INIT, CHECKSUM, DIFFERENTIAL, NAME = 'dlayout_diff';
GO
BACKUP DATABASE bak_dlayout TO DISK = '$(DIR)/dlayout_diff_s1.bak', DISK = '$(DIR)/dlayout_diff_s2.bak'
	WITH FORMAT, INIT, CHECKSUM, DIFFERENTIAL, NAME = 'dlayout_diff_striped';
GO
BACKUP DATABASE bak_dlayout TO DISK = '$(DIR)/dlayout_diff_x1.bak', DISK = '$(DIR)/dlayout_diff_x2.bak'
	WITH FORMAT, INIT, CHECKSUM, DIFFERENTIAL, COMPRESSION, NAME = 'dlayout_diff_xpress';
GO
BACKUP DATABASE bak_dlayout FILEGROUP = 'fg2' TO DISK = '$(DIR)/dlayout_fg2_diff.bak'
	WITH FORMAT, INIT, CHECKSUM, DIFFERENTIAL, NAME = 'dlayout_fg2_diff';
GO
BACKUP DATABASE bak_dlayout FILEGROUP = 'fg2' TO DISK = '$(DIR)/dlayout_fg2_s1.bak', DISK = '$(DIR)/dlayout_fg2_s2.bak'
	WITH FORMAT, INIT, CHECKSUM, DIFFERENTIAL, NAME = 'dlayout_fg2_diff_striped';
GO
