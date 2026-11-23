-- Spec 085 PR 4c-3, review of #612: a history break, for local development only. One table, a full; then
-- that full restored over the database (WITH REPLACE: a new recovery fork), a row inserted, and a second
-- full. mssql_backup_snapshots marks the second version's history_break, and mssql_backup_schema_changes
-- reloads every table across it -- the user's and, since every table is in the catalog model, those of
-- schema sys, told apart by type / is_ms_shipped. Compressed (MS_XPRESS), so the committed pair under
-- test/data/backup/history_break stays small.
SET NOCOUNT ON;
SET QUOTED_IDENTIFIER ON;
GO
IF DB_ID('bak_break') IS NOT NULL DROP DATABASE bak_break;
GO
CREATE DATABASE bak_break;
GO
ALTER DATABASE bak_break SET RECOVERY FULL;
GO
USE bak_break;
GO
CREATE TABLE dbo.t (id int NOT NULL, v int NULL, CONSTRAINT pk_t PRIMARY KEY (id));
INSERT INTO dbo.t VALUES (1, 1);
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_break TO DISK = '$(DIR)/break_before.bak' WITH FORMAT, INIT, CHECKSUM, COMPRESSION, NAME = 'break_before';
GO
USE master;
GO
RESTORE DATABASE bak_break FROM DISK = '$(DIR)/break_before.bak' WITH REPLACE, RECOVERY;
GO
USE bak_break;
GO
INSERT INTO dbo.t VALUES (2, 2);
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_break TO DISK = '$(DIR)/break_after.bak' WITH FORMAT, INIT, CHECKSUM, COMPRESSION, NAME = 'break_after';
GO
