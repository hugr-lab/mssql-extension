-- Spec 085 D11 (review of #591): a backup whose tail holds changes made between a page's last copy and
-- LastLSN, at known points of the copy, committed under test/data/backup/tail_window (made by its
-- generate.sh). generate.sh writes the backup into a FIFO and reads it itself, holding the reader twice:
-- in the first data pass (tail_window_pass1.sql changes the filler, so the second pass copies its
-- extents again), then inside the second pass (tail_window_pass2.sql changes the other tables, whose
-- pages that pass does not copy). Each table, and what its rows are in the backup against a RESTORE:
--   stale_update     an UPDATE committed in the second pass: the copy lacks it (stale)
--   rollback_insert  rows inserted before the backup by a transaction rolled back in the second pass:
--                    the copy holds them, a RESTORE does not (stale)
--   rollback_delete  the same for a DELETE: the copy lacks the rows (stale)
--   rollback_loaded  an INSERT ... SELECT WITH (TABLOCK) into an empty heap by the same transaction: its
--                    pages logged as page images, undone by freeing their extents (review of #619)
--   rollback_truncated  a TRUNCATE TABLE by the same transaction: the copy lacks the rows, the RESTORE
--                    has them again (review of #619)
--   savepoint        rows inserted, a savepoint, more rows, a rollback to it, a commit, in the second pass
--   rollback_grown   rows inserted into an empty table, on pages taken into use in the second pass, and
--                    rolled back there: the copy and a RESTORE both empty (consistent; review of #619)
--   parallel_open    rows inserted by a parallel plan (MAXDOP 4) in a transaction open past LastLSN: the
--                    copy holds them, a RESTORE undoes them (dirty)
--   loaded           an INSERT ... SELECT WITH (TABLOCK) into an empty heap in the second pass
--   truncated        a TRUNCATE TABLE in the second pass
--   rebuilt          an ALTER INDEX REBUILD in the second pass (the rows unchanged)
--   widened          an ALTER COLUMN int -> bigint in the second pass (every row rewritten)
--   static, src      never changed after the trial backup
--   filler           changed in the first pass: its extents copied again in the second (consistent)
-- generate.sh takes a trial backup once the open transactions have made their changes: where its second
-- data pass starts is where the real one's does. The signals between generate.sh and the sessions it runs are in database tw_control, which is not
-- backed up. Uncompressed: the holds are counted in the bytes of the data passes. Synthetic rows only (T4).
SET NOCOUNT ON;
GO
IF DB_ID('bak_tail_window') IS NOT NULL DROP DATABASE bak_tail_window;
IF DB_ID('tw_control') IS NOT NULL DROP DATABASE tw_control;
GO
CREATE DATABASE tw_control;
GO
CREATE TABLE tw_control.dbo.phase (n int NOT NULL);
INSERT INTO tw_control.dbo.phase VALUES (0);
CREATE TABLE tw_control.dbo.undone (n int NOT NULL);
INSERT INTO tw_control.dbo.undone VALUES (0);
-- Set by tail_window_open.sql's transaction once its changes are made (read uncommitted: a TRUNCATE's Sch-M
-- lock keeps any reader out of its table until the rollback).
CREATE TABLE tw_control.dbo.opened (n int NOT NULL);
INSERT INTO tw_control.dbo.opened VALUES (0);
GO
-- A small log, wrapped before the fixture's work (below): the backup's tail is then read from VLFs
-- used before, on every version the lane runs (a block of an earlier generation would break the chain
-- LogTailCursor checks; measured on 2025, a stream ends in zeros there too).
CREATE DATABASE bak_tail_window ON (NAME = bak_tail_window, FILENAME = '/var/opt/mssql/data/bak_tail_window.mdf')
	LOG ON (NAME = bak_tail_window_log, FILENAME = '/var/opt/mssql/data/bak_tail_window_log.ldf', SIZE = 1MB,
	FILEGROWTH = 1MB);
ALTER DATABASE bak_tail_window SET RECOVERY FULL;
-- Query Store (on by default from 2022) writes its own transactions into the tail: off, the log is the fixture's.
ALTER DATABASE bak_tail_window SET QUERY_STORE = OFF;
GO
USE bak_tail_window;
GO
-- The log wrapped: a full starts its chain, then rounds of changes each freed by a log backup (the VLFs
-- reused, their sequence numbers past the file's VLF count). The table is dropped, its extents freed.
BACKUP DATABASE bak_tail_window TO DISK = '$(DIR)/tail_window_chain.bak' WITH FORMAT, INIT;
CREATE TABLE dbo.churn (id int IDENTITY PRIMARY KEY, pad char(400) NOT NULL);
DECLARE @round int = 0;
WHILE @round < 12
BEGIN
	INSERT INTO dbo.churn (pad) SELECT TOP (250) 'x' FROM sys.all_objects;
	DELETE FROM dbo.churn;
	BACKUP LOG bak_tail_window TO DISK = '$(DIR)/tail_window_chain.trn' WITH FORMAT, INIT;
	SET @round += 1;
END;
DROP TABLE dbo.churn;
SELECT 'wrapped' AS what, COUNT(*) AS vlfs, MIN(vlf_sequence_number) AS first_sequence,
	MAX(vlf_sequence_number) AS last_sequence FROM sys.dm_db_log_info(DB_ID());
GO
CREATE TABLE dbo.src (id int PRIMARY KEY, v int NOT NULL, pad char(100) NOT NULL);
WITH n AS (SELECT TOP (2000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.src (id, v, pad) SELECT i, i % 97, CONCAT('src ', i) FROM n;
CREATE TABLE dbo.stale_update (id int PRIMARY KEY, v int NOT NULL);
CREATE TABLE dbo.rollback_insert (id int PRIMARY KEY, v int NOT NULL);
CREATE TABLE dbo.rollback_delete (id int PRIMARY KEY, v int NOT NULL);
CREATE TABLE dbo.rollback_loaded (id int NOT NULL, v int NOT NULL, pad char(100) NOT NULL);
CREATE TABLE dbo.rollback_truncated (id int PRIMARY KEY, v int NOT NULL);
CREATE TABLE dbo.rollback_grown (id int PRIMARY KEY, v int NOT NULL, pad char(200) NOT NULL);
CREATE TABLE dbo.savepoint (id int PRIMARY KEY, v int NOT NULL);
CREATE TABLE dbo.truncated (id int PRIMARY KEY, v int NOT NULL);
CREATE TABLE dbo.rebuilt (id int NOT NULL, v int NOT NULL, CONSTRAINT pk_rebuilt PRIMARY KEY (id));
CREATE TABLE dbo.widened (id int PRIMARY KEY, v int NOT NULL);
CREATE TABLE dbo.static (id int PRIMARY KEY, v int NOT NULL);
CREATE TABLE dbo.parallel_open (id int NOT NULL, v int NOT NULL, pad char(100) NOT NULL);
CREATE TABLE dbo.loaded (id int NOT NULL, v int NOT NULL, pad char(100) NOT NULL);
INSERT INTO dbo.stale_update SELECT id, 0 FROM dbo.src WHERE id <= 100;
INSERT INTO dbo.rollback_insert SELECT id, 0 FROM dbo.src WHERE id <= 100;
INSERT INTO dbo.rollback_delete SELECT id, 0 FROM dbo.src WHERE id <= 100;
INSERT INTO dbo.rollback_truncated SELECT id, 0 FROM dbo.src WHERE id <= 100;
INSERT INTO dbo.savepoint SELECT id, 0 FROM dbo.src WHERE id <= 100;
INSERT INTO dbo.truncated SELECT id, 0 FROM dbo.src WHERE id <= 100;
INSERT INTO dbo.rebuilt SELECT id, 0 FROM dbo.src WHERE id <= 100;
INSERT INTO dbo.widened SELECT id, id FROM dbo.src WHERE id <= 100;
INSERT INTO dbo.static SELECT id, 0 FROM dbo.src WHERE id <= 100;
GO
-- Last, so its extents are the file's highest: 512 KiB, which the second pass copies again.
CREATE TABLE dbo.filler (id int PRIMARY KEY, n int NOT NULL, b varbinary(4000) NOT NULL);
WITH n AS (SELECT TOP (128) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.filler (id, n, b) SELECT i, 0, CRYPT_GEN_RANDOM(4000) FROM n;
GO
CHECKPOINT;
GO
