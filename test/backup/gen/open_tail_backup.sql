-- Spec 085 PR 5: the open_tail fixture's backups (open_tail.sql), taken while open_tail_open.sql holds its
-- transaction open: a full (copy-only, so the differential's base stays open_tail_base) and a differential.
-- Then the open transaction is told to end.
SET NOCOUNT ON;
GO
BACKUP DATABASE bak_open_tail TO DISK = '$(DIR)/open_tail_full.bak' WITH FORMAT, INIT, COPY_ONLY, NAME = 'open_tail_full';
GO
BACKUP DATABASE bak_open_tail TO DISK = '$(DIR)/open_tail_diff.bak' WITH FORMAT, INIT, DIFFERENTIAL, NAME = 'open_tail_diff';
GO
UPDATE bak_open_tail.dbo.control SET backups_done = 1;
GO
