-- Spec 085 PR 5 (task 27): a small backup whose tail holds one transaction left open, committed under
-- test/data/backup/open_tail (made by its generate.sh), so the SQL tests read D11's status and the log
-- functions with no server. generate.sh runs this (the tables, a full the differential lies over), then
-- open_tail_open.sql in the background -- deletes and inserts, then updates, deletes and inserts, in one
-- transaction, held open -- then open_tail_backup.sql while it waits, then lets it roll back. What the
-- backups hold:
--   changed  its rows as the open transaction left them before the backups' checkpoint: dirty
--   static   never changed after the first full: consistent
--   variant_rows  a sql_variant column, a row deleted and three inserted by the open transaction, its log
--            records' row images holding VARIANTs (review of #631): dirty
-- Synthetic rows only (T4).
SET NOCOUNT ON;
GO
IF DB_ID('bak_open_tail') IS NOT NULL DROP DATABASE bak_open_tail;
GO
CREATE DATABASE bak_open_tail;
ALTER DATABASE bak_open_tail SET RECOVERY FULL;
GO
USE bak_open_tail;
GO
CREATE TABLE dbo.control (backups_done bit NOT NULL);
INSERT INTO dbo.control VALUES (0);
CREATE TABLE dbo.changed (id int PRIMARY KEY, v int NOT NULL, note varchar(20) NOT NULL);
CREATE TABLE dbo.static (id int PRIMARY KEY, v int NOT NULL);
CREATE TABLE dbo.variant_rows (id int PRIMARY KEY, sv sql_variant NULL);
-- One row a statement: a VALUES list would convert every sql_variant value to the list's one type.
INSERT INTO dbo.variant_rows VALUES (1, CAST(7 AS int));
INSERT INTO dbo.variant_rows VALUES (2, CAST(N'жук' AS nvarchar(10)));
WITH n AS (SELECT TOP (100) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.changed (id, v, note) SELECT i, i, CONCAT('row ', i) FROM n;
WITH n AS (SELECT TOP (100) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.static (id, v) SELECT i, i FROM n;
GO
BACKUP DATABASE bak_open_tail TO DISK = '$(DIR)/open_tail_base.bak' WITH FORMAT, INIT, NAME = 'open_tail_base';
GO
