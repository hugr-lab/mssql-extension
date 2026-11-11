-- Spec 085 PR 2b: the files fixture's second stage (generate.sh runs it after files.sql and the
-- golden catalog of files_full): the second data file emptied and removed, then backed up again
-- (E19: IAM chains that keep the removed file's sequence numbers; an SFIN record flagged 0x20).
SET NOCOUNT ON;
GO
USE bak_files;
GO
DBCC SHRINKFILE (bak_files_2, EMPTYFILE) WITH NO_INFOMSGS;
GO
ALTER DATABASE bak_files REMOVE FILE bak_files_2;
GO
BACKUP DATABASE bak_files TO DISK = '$(DIR)/files_removed.bak' WITH FORMAT, INIT, CHECKSUM, NAME = 'files_removed';
GO
