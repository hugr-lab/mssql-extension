-- Spec 087 task 9: the committed samples (test/data/backup/encryption, fixture `encryption_samples`): one small
-- encrypted backup and one small TDE database's backups, made with TEST-ONLY certificates created for these
-- samples alone and committed with their keys and the keys' password -- nothing else ever uses them, so the
-- password below is public by design (the samples' secrets name it). $(DIR) is the container's backup
-- directory; $(MKPASS) the master key's password (generate.sh's, for this run alone).
--
-- bak_enc_sample: dbo.t (id, v) rows (1, 'one'), (2, 'two'), (3, 'three'), and dbo.big, 2000 rows (id, the
-- name 'row <id>'), backed up WITH ENCRYPTION (AES_256, sample_enc_cert) and MS_XPRESS -> enc_sample.
-- bak_tde_sample: the same tables, its DEK AES_256 under sample_tde_cert -> tde_sample (uncompressed: its pages
-- each under the DEK), tde_sample_xpress (compressed: E4c's form, the blocks under the DEK), tde_sample_enc
-- (compressed and WITH ENCRYPTION by sample_enc_cert: both keys).
SET NOCOUNT ON;
GO
USE master;
GO
IF NOT EXISTS (SELECT 1 FROM sys.symmetric_keys WHERE name = '##MS_DatabaseMasterKey##')
	CREATE MASTER KEY ENCRYPTION BY PASSWORD = '$(MKPASS)';
GO
CREATE CERTIFICATE sample_enc_cert WITH SUBJECT = 'mssql-extension committed sample only: backup encryption';
CREATE CERTIFICATE sample_tde_cert WITH SUBJECT = 'mssql-extension committed sample only: TDE';
GO
BACKUP CERTIFICATE sample_enc_cert TO FILE = '$(DIR)/sample_enc_cert.cer'
	WITH PRIVATE KEY (FILE = '$(DIR)/sample_enc_cert.pvk', ENCRYPTION BY PASSWORD = 'test-only sample key 087!');
BACKUP CERTIFICATE sample_tde_cert TO FILE = '$(DIR)/sample_tde_cert.cer'
	WITH PRIVATE KEY (FILE = '$(DIR)/sample_tde_cert.pvk', ENCRYPTION BY PASSWORD = 'test-only sample key 087!');
GO
DECLARE @db sysname;
DECLARE dbs CURSOR FOR SELECT v FROM (VALUES ('bak_enc_sample'), ('bak_tde_sample')) t(v);
OPEN dbs; FETCH NEXT FROM dbs INTO @db;
WHILE @@FETCH_STATUS = 0
BEGIN
	EXEC (N'IF DB_ID(''' + @db + N''') IS NOT NULL DROP DATABASE ' + @db + N'; CREATE DATABASE ' + @db + N';');
	EXEC (N'USE ' + @db + N';
CREATE TABLE dbo.t (id int PRIMARY KEY, v varchar(20) NOT NULL);
INSERT INTO dbo.t VALUES (1, ''one''), (2, ''two''), (3, ''three'');
CREATE TABLE dbo.big (id int PRIMARY KEY, name nvarchar(40) NOT NULL);
WITH n AS (SELECT TOP (2000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.big SELECT i, CONCAT(N''row '', i) FROM n;');
	FETCH NEXT FROM dbs INTO @db;
END
CLOSE dbs; DEALLOCATE dbs;
GO
USE bak_tde_sample;
CREATE DATABASE ENCRYPTION KEY WITH ALGORITHM = AES_256 ENCRYPTION BY SERVER CERTIFICATE sample_tde_cert;
ALTER DATABASE bak_tde_sample SET ENCRYPTION ON;
GO
USE master;
DECLARE @tries int = 0;
WHILE EXISTS (SELECT 1 FROM sys.dm_database_encryption_keys k WHERE k.database_id = DB_ID('bak_tde_sample') AND k.encryption_state <> 3) AND @tries < 120
BEGIN WAITFOR DELAY '00:00:01'; SET @tries += 1; END
IF EXISTS (SELECT 1 FROM sys.dm_database_encryption_keys k WHERE k.database_id = DB_ID('bak_tde_sample') AND k.encryption_state <> 3)
	THROW 50000, 'encryption_samples.sql: the TDE encryption scan did not end within 120 s', 1;
CHECKPOINT;
GO
BACKUP DATABASE bak_enc_sample TO DISK = '$(DIR)/enc_sample.bak'
	WITH FORMAT, INIT, CHECKSUM, COMPRESSION, NAME = 'enc_sample', ENCRYPTION (ALGORITHM = AES_256, SERVER CERTIFICATE = sample_enc_cert);
BACKUP DATABASE bak_tde_sample TO DISK = '$(DIR)/tde_sample.bak' WITH FORMAT, INIT, CHECKSUM, NO_COMPRESSION, NAME = 'tde_sample';
BACKUP DATABASE bak_tde_sample TO DISK = '$(DIR)/tde_sample_xpress.bak' WITH FORMAT, INIT, CHECKSUM, COMPRESSION, NAME = 'tde_sample_xpress';
BACKUP DATABASE bak_tde_sample TO DISK = '$(DIR)/tde_sample_enc.bak'
	WITH FORMAT, INIT, CHECKSUM, COMPRESSION, NAME = 'tde_sample_enc', ENCRYPTION (ALGORITHM = AES_256, SERVER CERTIFICATE = sample_enc_cert);
GO
SELECT name, CONVERT(varchar(64), thumbprint, 2) AS thumbprint FROM sys.certificates WHERE name LIKE 'sample%' ORDER BY name;
GO
