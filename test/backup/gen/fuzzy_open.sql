-- Spec 085 PR 5: the fuzzy fixture's open transaction (fuzzy.sql), run in the background by generate.sh once the
-- workload commits: it changes open_only and a range of busy the workload never reads, then waits -- open --
-- until fuzzy_backup.sql has set dbo.control.backups_done, and rolls back. Every backup taken meanwhile holds
-- its changes (the backup's checkpoints write them) and a RESTORE undoes them: the tables it touched are dirty.
SET NOCOUNT ON;
SET XACT_ABORT ON;
USE bak_fuzzy;
BEGIN TRANSACTION;
UPDATE dbo.open_only SET v = v + 1000 WHERE id % 3 = 0;
DELETE FROM dbo.open_only WHERE id % 7 = 0;
INSERT INTO dbo.open_only (id, v) SELECT id + 100000, 1000 FROM dbo.open_only WHERE id <= 200;
UPDATE dbo.busy SET v = v + 1000 WHERE id BETWEEN 290000 AND 291000;
-- The flag is read without locks: the workload updates that row in every transaction.
WHILE (SELECT backups_done FROM dbo.control WITH (READUNCOMMITTED)) = 0
	WAITFOR DELAY '00:00:00.200';
ROLLBACK;
GO
