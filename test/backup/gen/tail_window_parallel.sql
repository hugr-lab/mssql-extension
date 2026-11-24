-- Spec 085 D11: tail_window.sql's transaction left open past LastLSN, its rows inserted by a parallel
-- plan (MAXDOP 4, forced by trace flag 8649 for this query: the table is too small to be worth one). Run by generate.sh in the background
-- before the backup; rolled back once tw_control says the backup is done (phase 3).
SET NOCOUNT ON;
SET XACT_ABORT ON;
GO
USE bak_tail_window;
GO
BEGIN TRANSACTION tw_parallel;
INSERT INTO dbo.parallel_open WITH (TABLOCK) (id, v, pad)
SELECT id, v, pad FROM dbo.src OPTION (MAXDOP 4, QUERYTRACEON 8649, QUERYTRACEON 8757);
WHILE (SELECT n FROM tw_control.dbo.phase) < 3
	WAITFOR DELAY '00:00:00.200';
ROLLBACK TRANSACTION tw_parallel;
GO
