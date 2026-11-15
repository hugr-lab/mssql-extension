-- Spec 085 acceptance 13 (task 16b): a backup much larger than memory, about 20 GB at HUGE_SCALE 1, for
-- the manual lane and local runs (test/backup/acceptance13.sh); not in generate.sh's default FIXTURES.
-- Four tables of the shapes the scan reads, synthetic: a wide heap (nvarchar, a code-page varchar,
-- uniqueidentifier, varbinary, NULLs; ~8 GB), a clustered table (~6 GB), a partitioned one with a
-- non-unique clustered index (12 partitions, the uniquifier; ~4 GB) and a heap of one row a page (~2 GB).
-- Loaded minimally logged (SIMPLE, TABLOCK), a million rows a batch. Backed up plain and MS_XPRESS.
-- HUGE_SCALE (sqlcmd variable) scales every table's rows: 0.05 for a quick check of the pipeline.
SET NOCOUNT ON;
GO
IF DB_ID('bak_huge') IS NOT NULL DROP DATABASE bak_huge;
GO
CREATE DATABASE bak_huge ON PRIMARY (NAME = bak_huge, FILENAME = '/var/opt/mssql/data/bak_huge.mdf', SIZE = 1GB,
	FILEGROWTH = 2GB)
	LOG ON (NAME = bak_huge_log, FILENAME = '/var/opt/mssql/data/bak_huge_log.ldf', SIZE = 1GB, FILEGROWTH = 1GB);
ALTER DATABASE bak_huge SET RECOVERY SIMPLE;
GO
USE bak_huge;
GO
CREATE TABLE #n (i int NOT NULL PRIMARY KEY);
INSERT INTO #n SELECT TOP (1000000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) FROM sys.all_objects a
	CROSS JOIN sys.all_objects b CROSS JOIN sys.all_objects c;
GO
CREATE TABLE dbo.h_wide (id int NOT NULL, b bigint NOT NULL, d decimal(18, 4) NOT NULL, t datetime2(3) NOT NULL,
	nv nvarchar(200) NULL, cp varchar(100) COLLATE Cyrillic_General_CI_AS NOT NULL, g uniqueidentifier NOT NULL,
	f bit NOT NULL, vb varbinary(64) NOT NULL, n int NULL);
CREATE TABLE dbo.c_orders (id bigint NOT NULL PRIMARY KEY, cust int NOT NULL, amt money NOT NULL,
	ts datetime NOT NULL, status char(2) NOT NULL, note varchar(60) NULL, qty smallint NOT NULL,
	price decimal(10, 2) NOT NULL, dt date NOT NULL);
CREATE PARTITION FUNCTION pf_huge (datetime2(0)) AS RANGE RIGHT FOR VALUES ('2026-02-01', '2026-03-01', '2026-04-01',
	'2026-05-01', '2026-06-01', '2026-07-01', '2026-08-01', '2026-09-01', '2026-10-01', '2026-11-01', '2026-12-01');
CREATE PARTITION SCHEME ps_huge AS PARTITION pf_huge ALL TO ([PRIMARY]);
CREATE TABLE dbo.p_events (id int NOT NULL, dev int NOT NULL, ts datetime2(0) NOT NULL, val float NOT NULL,
	tag nvarchar(30) NOT NULL, payload varchar(200) NULL) ON ps_huge (ts);
CREATE TABLE dbo.pages (id int NOT NULL, fill char(7900) NOT NULL);
GO
DECLARE @wide int = CAST(28000000 * $(HUGE_SCALE) AS int);
DECLARE @orders int = CAST(60000000 * $(HUGE_SCALE) AS int);
DECLARE @events int = CAST(26000000 * $(HUGE_SCALE) AS int);
DECLARE @pages int = CAST(262000 * $(HUGE_SCALE) AS int);
DECLARE @at int = 0;
WHILE @at < @wide
BEGIN
	INSERT INTO dbo.h_wide WITH (TABLOCK)
	SELECT k, CAST(k AS bigint) * 1000003, k / 7.0, DATEADD(SECOND, k % 86400000, '2020-01-01'),
		CASE WHEN k % 11 = 0 THEN NULL ELSE LEFT(REPLICATE(N'жёлтый wolf ', 17), 20 + k % 180) END,
		LEFT(REPLICATE(N'съешь ещё этих ', 7), 10 + k % 90), NEWID(), k % 2,
		CAST(k AS binary(4)) + CAST(k * 7 AS binary(4)) + CAST(REPLICATE('x', k % 40) AS varbinary(40)),
		CASE WHEN k % 7 = 0 THEN NULL ELSE k END
	FROM (SELECT @at + i AS k FROM #n WHERE i <= @wide - @at) s;
	SET @at += 1000000;
	CHECKPOINT;
END;
SET @at = 0;
WHILE @at < @orders
BEGIN
	INSERT INTO dbo.c_orders WITH (TABLOCK)
	SELECT k, k % 100003, k % 100000 / 100.0, DATEADD(MINUTE, k % 5000000, '2015-01-01'),
		CHAR(65 + k % 26) + CHAR(65 + k % 7), CASE WHEN k % 5 = 0 THEN NULL ELSE CONCAT('note ', k) END,
		k % 30000, k % 1000000 / 100.0, DATEADD(DAY, k % 3650, '2016-01-01')
	FROM (SELECT CAST(@at AS bigint) + i AS k FROM #n WHERE i <= @orders - @at) s ORDER BY k;
	SET @at += 1000000;
	CHECKPOINT;
END;
SET @at = 0;
WHILE @at < @events
BEGIN
	INSERT INTO dbo.p_events WITH (TABLOCK)
	SELECT k, k % 5000, DATEADD(SECOND, CAST(CAST(k AS bigint) * 31000000 / @events AS int), '2026-01-01'), k * 0.25,
		CONCAT(N'ü-', k % 997),
		CASE WHEN k % 3 = 0 THEN NULL ELSE REPLICATE('p', k % 150) END
	FROM (SELECT @at + i AS k FROM #n WHERE i <= @events - @at) s;
	SET @at += 1000000;
	CHECKPOINT;
END;
SET @at = 0;
WHILE @at < @pages
BEGIN
	INSERT INTO dbo.pages WITH (TABLOCK)
	SELECT k, CONCAT('page ', k) FROM (SELECT @at + i AS k FROM #n WHERE i <= @pages - @at) s;
	SET @at += 1000000;
	CHECKPOINT;
END;
-- The clustered index after the load: one sort, rather than a split per batch.
CREATE CLUSTERED INDEX cx_p_events ON dbo.p_events (dev) ON ps_huge (ts);
GO
DROP TABLE #n;
CHECKPOINT;
GO
BACKUP DATABASE bak_huge TO DISK = '$(DIR)/huge_full.bak' WITH FORMAT, INIT, CHECKSUM, NAME = 'huge_full';
GO
BACKUP DATABASE bak_huge TO DISK = '$(DIR)/huge_xpress.bak' WITH FORMAT, INIT, CHECKSUM,
	COMPRESSION (ALGORITHM = MS_XPRESS), NAME = 'huge_xpress';
GO
