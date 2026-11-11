-- Spec 085 PR 2b: the load fixture's backups, taken while load_workload.sql runs (generate.sh waits
-- for its first ticks): a full with CHECKSUM, a compressed full, a differential and a log backup.
SET NOCOUNT ON;
GO
BACKUP DATABASE bak_load TO DISK = '$(DIR)/load_full.bak' WITH FORMAT, INIT, CHECKSUM, COPY_ONLY, NAME = 'load_full';
GO
BACKUP DATABASE bak_load TO DISK = '$(DIR)/load_xpress.bak' WITH FORMAT, INIT, COPY_ONLY, COMPRESSION, NAME = 'load_xpress';
GO
BACKUP DATABASE bak_load TO DISK = '$(DIR)/load_diff.bak' WITH FORMAT, INIT, DIFFERENTIAL, NAME = 'load_diff';
GO
BACKUP LOG bak_load TO DISK = '$(DIR)/load_log.trn' WITH FORMAT, INIT, NAME = 'load_log';
GO
