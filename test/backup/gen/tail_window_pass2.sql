-- Spec 085 D11: run while generate.sh holds tail_window's backup inside its second data pass, which does
-- not copy these tables' pages: each change below is in the tail and not in the copies. Then the
-- transaction of tail_window_open.sql is told to roll back (phase 2), and waited for: the reader goes on
-- only once its undo is in the log.
SET NOCOUNT ON;
SET XACT_ABORT ON;
GO
USE bak_tail_window;
GO
UPDATE dbo.stale_update SET v = 1 WHERE id <= 10;
GO
BEGIN TRANSACTION;
INSERT INTO dbo.savepoint SELECT id, 1 FROM dbo.src WHERE id BETWEEN 101 AND 110;
SAVE TRANSACTION tw_save;
INSERT INTO dbo.savepoint SELECT id, 2 FROM dbo.src WHERE id BETWEEN 111 AND 120;
UPDATE dbo.savepoint SET v = 2 WHERE id <= 10;
ROLLBACK TRANSACTION tw_save;
COMMIT TRANSACTION;
GO
INSERT INTO dbo.loaded WITH (TABLOCK) (id, v, pad) SELECT id, v, pad FROM dbo.src WHERE id <= 1000;
GO
-- Pages and extents taken into use after the copy, by rows rolled back: nothing a RESTORE keeps. The flush
-- puts the undo before LastLSN (a ROLLBACK does not harden the log: tail_window_open.sql).
BEGIN TRANSACTION;
INSERT INTO dbo.rollback_grown (id, v, pad) SELECT id, v, pad FROM dbo.src;
ROLLBACK TRANSACTION;
EXEC sys.sp_flush_log;
GO
TRUNCATE TABLE dbo.truncated;
GO
ALTER INDEX pk_rebuilt ON dbo.rebuilt REBUILD;
GO
ALTER TABLE dbo.widened ALTER COLUMN v bigint NOT NULL;
GO
UPDATE tw_control.dbo.phase SET n = 2;
GO
DECLARE @waited int = 0;
WHILE (SELECT n FROM tw_control.dbo.undone) = 0 AND @waited < 300
BEGIN
	WAITFOR DELAY '00:00:00.200';
	SET @waited += 1;
END;
IF @waited >= 300
	RAISERROR('tail_window: the transaction tw_undo did not roll back in 60 s', 16, 1);
GO
