-- Spec 085: the backup samples, made on a CLEAN instance (spec 085 T4: only synthetic data from an
-- instance of our own). Run by generate.sh, which passes the backup directory as the sqlcmd variable
-- DIR; every row below is generated, nothing comes from anyone's database. One script for every
-- version the oracle lane runs (2017-2025): what a version lacks is skipped by its major version
-- (MS_XPRESS by name from 2022, before that plain WITH COMPRESSION, which is MS_XPRESS; ZSTD from
-- 2025), and generate.sh lists the sets the same way.
SET NOCOUNT ON;
GO
IF DB_ID('bak_sample') IS NOT NULL DROP DATABASE bak_sample;
GO
CREATE DATABASE bak_sample COLLATE SQL_Latin1_General_CP1_CI_AS;
ALTER DATABASE bak_sample SET RECOVERY FULL;
GO
USE bak_sample;
GO
CREATE TABLE dbo.items (
	id int NOT NULL PRIMARY KEY,
	name nvarchar(40) NOT NULL,
	code varchar(12) NULL,
	price decimal(12, 2) NOT NULL,
	created datetime2(3) NOT NULL,
	flag bit NOT NULL,
	blob varbinary(64) NULL
);
CREATE TABLE dbo.events (	-- a heap
	event_id bigint NOT NULL,
	item_id int NOT NULL,
	note varchar(100) NULL,
	at datetime NOT NULL
);
GO
-- Deterministic synthetic rows.
WITH n AS (SELECT TOP (2000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.items (id, name, code, price, created, flag, blob)
SELECT i, CONCAT(N'item ', i, N' ', NCHAR(0x00E9 + i % 7)), CASE WHEN i % 5 = 0 THEN NULL ELSE CONCAT('C', i % 997) END,
	   CAST(i * 1.25 AS decimal(12, 2)), DATEADD(SECOND, i * 37, CAST('2026-01-01T00:00:00' AS datetime2(3))),
	   CAST(i % 2 AS bit), CASE WHEN i % 3 = 0 THEN CAST(HASHBYTES('SHA2_256', CAST(i AS varchar(10))) AS varbinary(64)) END
FROM n;
WITH n AS (SELECT TOP (3000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.events (event_id, item_id, note, at)
SELECT i, 1 + i % 2000, CASE WHEN i % 4 = 0 THEN NULL ELSE CONCAT('event ', i) END, DATEADD(MINUTE, i, '2026-02-01')
FROM n;
GO
-- The full backups: plain (WITH CHECKSUM), MS_XPRESS, ZSTD (2025), and striped in two, plain and ZSTD.
DECLARE @major int = CAST(SERVERPROPERTY('ProductMajorVersion') AS int);
BACKUP DATABASE bak_sample TO DISK = '$(DIR)/full.bak' WITH FORMAT, INIT, CHECKSUM, NAME = 'full';
IF @major >= 16
	EXEC (N'BACKUP DATABASE bak_sample TO DISK = ''$(DIR)/full_xpress.bak''
		WITH FORMAT, INIT, COPY_ONLY, COMPRESSION (ALGORITHM = MS_XPRESS), NAME = ''full_xpress''');
ELSE
	BACKUP DATABASE bak_sample TO DISK = '$(DIR)/full_xpress.bak' WITH FORMAT, INIT, COPY_ONLY, COMPRESSION, NAME = 'full_xpress';
IF @major >= 17
	EXEC (N'BACKUP DATABASE bak_sample TO DISK = ''$(DIR)/full_zstd.bak''
		WITH FORMAT, INIT, COPY_ONLY, COMPRESSION (ALGORITHM = ZSTD), NAME = ''full_zstd''');
BACKUP DATABASE bak_sample TO DISK = '$(DIR)/stripe_1.bak', DISK = '$(DIR)/stripe_2.bak'
	WITH FORMAT, INIT, COPY_ONLY, NAME = 'stripe';
IF @major >= 17
	EXEC (N'BACKUP DATABASE bak_sample TO DISK = ''$(DIR)/stripe_zstd_1.bak'', DISK = ''$(DIR)/stripe_zstd_2.bak''
		WITH FORMAT, INIT, COPY_ONLY, COMPRESSION (ALGORITHM = ZSTD), NAME = ''stripe_zstd''');
GO
-- Changes after the full: a differential, then a log backup.
UPDATE dbo.items SET price = price + 1, name = CONCAT(name, N' (updated)') WHERE id % 10 = 0;
DELETE FROM dbo.events WHERE event_id % 7 = 0;
WITH n AS (SELECT TOP (500) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.items (id, name, code, price, created, flag, blob)
SELECT 10000 + i, CONCAT(N'new ', i), 'NEW', 9.99, '2026-03-01', 1, NULL FROM n;
GO
BACKUP DATABASE bak_sample TO DISK = '$(DIR)/diff.bak' WITH FORMAT, INIT, DIFFERENTIAL, NAME = 'diff';
BACKUP LOG bak_sample TO DISK = '$(DIR)/log.trn' WITH FORMAT, INIT, NAME = 'log';
GO
