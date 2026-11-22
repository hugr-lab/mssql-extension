-- Spec 085 PR 5: the open_tail fixture's transaction (open_tail.sql), run in the background by generate.sh: it
-- updates ten rows of dbo.changed, deletes three, inserts three, then waits -- open -- until
-- open_tail_backup.sql has set dbo.control.backups_done, and rolls back.
SET NOCOUNT ON;
SET XACT_ABORT ON;
USE bak_open_tail;
BEGIN TRANSACTION open_tail;
UPDATE dbo.changed SET v = v + 1000 WHERE id <= 10;
DELETE FROM dbo.changed WHERE id BETWEEN 50 AND 52;
INSERT INTO dbo.changed (id, v, note) VALUES (201, 201, 'new 201'), (202, 202, 'new 202'), (203, 203, 'new 203');
WHILE (SELECT backups_done FROM dbo.control WITH (READUNCOMMITTED)) = 0
	WAITFOR DELAY '00:00:00.200';
ROLLBACK;
GO
