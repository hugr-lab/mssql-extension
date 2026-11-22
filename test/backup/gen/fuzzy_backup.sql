-- Spec 085 PR 5: the fuzzy fixture's backups (fuzzy.sql), taken while fuzzy_workload.sql commits and
-- fuzzy_open.sql holds its transaction open: a full with CHECKSUM, a compressed full (both copy-only, so the
-- differential's base stays fuzzy_base) and a differential. One 64 KB buffer each (E3 § 1), so a backup runs for
-- seconds and commits land between its checkpoint and its end. Then the open transaction is told to end.
SET NOCOUNT ON;
GO
BACKUP DATABASE bak_fuzzy TO DISK = '$(DIR)/fuzzy_full.bak'
	WITH FORMAT, INIT, CHECKSUM, COPY_ONLY, BUFFERCOUNT = 1, MAXTRANSFERSIZE = 65536, NAME = 'fuzzy_full';
GO
BACKUP DATABASE bak_fuzzy TO DISK = '$(DIR)/fuzzy_xpress.bak'
	WITH FORMAT, INIT, COPY_ONLY, COMPRESSION, BUFFERCOUNT = 1, MAXTRANSFERSIZE = 65536, NAME = 'fuzzy_xpress';
GO
BACKUP DATABASE bak_fuzzy TO DISK = '$(DIR)/fuzzy_diff.bak'
	WITH FORMAT, INIT, DIFFERENTIAL, BUFFERCOUNT = 1, MAXTRANSFERSIZE = 65536, NAME = 'fuzzy_diff';
GO
UPDATE bak_fuzzy.dbo.control SET backups_done = 1;
GO
