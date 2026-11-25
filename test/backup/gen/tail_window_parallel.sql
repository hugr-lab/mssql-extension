-- Spec 085 D11: tail_window.sql's transaction left open past LastLSN: two TRUNCATEs (review of #619), then rows
-- inserted by a parallel plan (MAXDOP 4, forced by trace flag 8649 for this query: the table is too small to be
-- worth one). The TRUNCATEs first, so generate.sh's wait for the inserted rows says all are made (a Sch-M lock
-- keeps every reader out of its own table). Before it, once tail_window_open.sql's transaction is open (the
-- tail then starts at or before its BEGIN), rows committed into the empty open_grown: its IAM page formatted in
-- the tail by a transaction of its own. Run by generate.sh in the background before the backup; rolled back once
-- tw_control says the backup is done (phase 3).
SET NOCOUNT ON;
SET XACT_ABORT ON;
GO
USE bak_tail_window;
GO
WHILE (SELECT n FROM tw_control.dbo.opened WITH (READUNCOMMITTED)) < 1
	WAITFOR DELAY '00:00:00.200';
INSERT INTO dbo.open_grown SELECT id, 0 FROM dbo.src WHERE id <= 100;
BEGIN TRANSACTION tw_parallel;
TRUNCATE TABLE dbo.open_grown;
TRUNCATE TABLE dbo.open_truncated;
INSERT INTO dbo.parallel_open WITH (TABLOCK) (id, v, pad)
SELECT id, v, pad FROM dbo.src OPTION (MAXDOP 4, QUERYTRACEON 8649, QUERYTRACEON 8757);
WHILE (SELECT n FROM tw_control.dbo.phase) < 3
	WAITFOR DELAY '00:00:00.200';
ROLLBACK TRANSACTION tw_parallel;
GO
