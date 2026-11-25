-- Spec 085 D11: tail_window.sql's transaction rolled back in the second pass. Run by generate.sh in the
-- background before the backup: an insert, a delete, a bulk load (TABLOCK, page images) and a TRUNCATE held
-- open, rolled back once tw_control says the reader is held inside the second data pass (phase 2); then says
-- so in tw_control.dbo.undone.
SET NOCOUNT ON;
SET XACT_ABORT ON;
GO
USE bak_tail_window;
GO
BEGIN TRANSACTION tw_undo;
INSERT INTO dbo.rollback_insert SELECT id, 1 FROM dbo.src WHERE id BETWEEN 101 AND 150;
DELETE FROM dbo.rollback_delete WHERE id <= 50;
INSERT INTO dbo.rollback_loaded WITH (TABLOCK) (id, v, pad) SELECT id, v, pad FROM dbo.src WHERE id <= 1000;
TRUNCATE TABLE dbo.rollback_truncated;
UPDATE tw_control.dbo.opened SET n = 1;
WHILE (SELECT n FROM tw_control.dbo.phase) < 2
	WAITFOR DELAY '00:00:00.200';
ROLLBACK TRANSACTION tw_undo;
-- A ROLLBACK does not harden the log (a COMMIT does), and a backup copies the log up to what is hardened:
-- without the flush its undo and ABORT stayed past LastLSN (measured on 2025), the transaction open.
EXEC sys.sp_flush_log;
UPDATE tw_control.dbo.undone SET n = 1;
GO
