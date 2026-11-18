-- Spec 085 PR 4c-3: a partition switched in with no shrink after it, for local development only. A
-- partitioned table and a staging table of its shape, both in the full; then the staging table's rows
-- switched into the table's empty partition and a row of another partition updated, then the
-- differential. The switched-in pages keep their LSNs (older than the full's checkpoint): only the
-- catalog tells they are new to the table, so mssql_backup_table_changes must take it whole (a reload).
SET NOCOUNT ON;
SET QUOTED_IDENTIFIER ON;
GO
IF DB_ID('bak_switch') IS NOT NULL DROP DATABASE bak_switch;
GO
CREATE DATABASE bak_switch;
GO
ALTER DATABASE bak_switch SET RECOVERY FULL;
GO
USE bak_switch;
GO
CREATE PARTITION FUNCTION pf_switch (int) AS RANGE RIGHT FOR VALUES (1000, 2000);
CREATE PARTITION SCHEME ps_switch AS PARTITION pf_switch ALL TO ([PRIMARY]);
GO
CREATE TABLE dbo.facts (id int NOT NULL, v int NULL, CONSTRAINT pk_facts PRIMARY KEY (id)) ON ps_switch (id);
CREATE TABLE dbo.staging (id int NOT NULL, v int NULL, CONSTRAINT pk_staging PRIMARY KEY (id),
	CONSTRAINT ck_staging CHECK (id >= 2000 AND id IS NOT NULL));
GO
WITH n AS (SELECT TOP (1500) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.facts SELECT i, i FROM n;
WITH n AS (SELECT TOP (700) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.staging SELECT 2000 + i, i FROM n;
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_switch TO DISK = '$(DIR)/switch_full.bak' WITH FORMAT, INIT, CHECKSUM, NAME = 'switch_full';
GO
ALTER TABLE dbo.staging SWITCH TO dbo.facts PARTITION 3;
UPDATE dbo.facts SET v = -v WHERE id = 10;
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_switch TO DISK = '$(DIR)/switch_diff.bak' WITH FORMAT, INIT, CHECKSUM, DIFFERENTIAL, NAME = 'switch_diff';
GO
