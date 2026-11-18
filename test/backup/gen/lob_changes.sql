-- Spec 085 PR 4c-3 (task 24d, DQ-47): does a change to a value stored off row move its data row's page
-- (its LSN), as the row changes key on? A full, then each form of change on its own row -- the whole
-- varchar(max) replaced, .WRITE, UPDATETEXT on a text, a varbinary(max) replaced at its length, a
-- small varchar(max) grown off row --, rows left alone beside them, then the differential. No shrink:
-- pages stay where they are. For local development (and 4c-3's tests); not run by default.
SET NOCOUNT ON;
SET QUOTED_IDENTIFIER ON;
GO
IF DB_ID('bak_lob_changes') IS NOT NULL DROP DATABASE bak_lob_changes;
GO
CREATE DATABASE bak_lob_changes;
GO
ALTER DATABASE bak_lob_changes SET RECOVERY FULL;
GO
USE bak_lob_changes;
GO
CREATE TABLE dbo.lobs (id int NOT NULL PRIMARY KEY, vmax varchar(max) NULL, t text NULL, b varbinary(max) NULL);
EXEC sp_tableoption 'dbo.lobs', 'large value types out of row', 1;
CREATE TABLE dbo.inrow (id int NOT NULL PRIMARY KEY, vmax varchar(max) NULL);  -- in row until it grows
GO
WITH n AS (SELECT TOP (8) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.lobs SELECT i, REPLICATE(CAST(CONCAT('v', i) AS varchar(max)), 4000), REPLICATE(CONCAT('t', i), 2000),
	CAST(REPLICATE(CAST('b' AS varchar(max)), 9000) AS varbinary(max)) FROM n;
WITH n AS (SELECT TOP (8) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.inrow SELECT i, CONCAT('small', i) FROM n;
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_lob_changes TO DISK = '$(DIR)/lob_changes_full.bak' WITH FORMAT, INIT, CHECKSUM, NAME = 'lob_changes_full';
GO
UPDATE dbo.lobs SET vmax = REPLICATE(CAST('V1' AS varchar(max)), 4000) WHERE id = 1;
UPDATE dbo.lobs SET vmax.WRITE('appended', NULL, NULL) WHERE id = 2;
DECLARE @p varbinary(16) = (SELECT TEXTPTR(t) FROM dbo.lobs WHERE id = 3);
UPDATETEXT dbo.lobs.t @p 0 2 'XX';
GO
UPDATE dbo.lobs SET b = CAST(REPLICATE(CAST('c' AS varchar(max)), 9000) AS varbinary(max)) WHERE id = 4;
UPDATE dbo.lobs SET vmax.WRITE('Z', 0, 1) WHERE id = 5;   -- an in-place overwrite, the length kept
UPDATE dbo.inrow SET vmax = REPLICATE(CAST('g' AS varchar(max)), 20000) WHERE id = 1;  -- grown off row
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_lob_changes TO DISK = '$(DIR)/lob_changes_diff.bak' WITH FORMAT, INIT, CHECKSUM, DIFFERENTIAL, NAME = 'lob_changes_diff';
GO
