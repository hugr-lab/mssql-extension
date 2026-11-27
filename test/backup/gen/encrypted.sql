-- Spec 087 task 9: encrypted and TDE backups of synthetic databases, with their certificates exported (DER and
-- PVK) for the reader's secrets: the oracle lane's matrix (fixture `encrypted`, not run by default). $(DIR) is
-- the container's backup directory. $(PVKPASS), the PVK password, and $(MKPASS), the master key's, are
-- environment variables generate.sh makes for this run alone, never on a command line.
--
-- E4 / E4b / E4c (specs/087-backup-encryption): backup encryption for every algorithm, x {none, MS_XPRESS,
-- ZSTD} x {CHECKSUM, none}, striped, a second certificate and a differential; TDE under AES_256 and AES_128,
-- each uncompressed and compressed (E4c's container form: MS_XPRESS with and without CHECKSUM, ZSTD), an
-- encrypted backup of a TDE database (two keys), the DEK rotated to a second certificate, a full and
-- differentials after it; and a tiny TDE database backed up three ways (bak_tde_small: the same pages plain,
-- MS_XPRESS and ZSTD). A combination the version refuses (ZSTD before 2025) is printed, not an error.
SET NOCOUNT ON;
GO
USE master;
GO
IF NOT EXISTS (SELECT 1 FROM sys.symmetric_keys WHERE name = '##MS_DatabaseMasterKey##')
	CREATE MASTER KEY ENCRYPTION BY PASSWORD = '$(MKPASS)';
GO
CREATE CERTIFICATE enc_cert WITH SUBJECT = 'mssql-extension test-only backup encryption';
CREATE CERTIFICATE enc_cert2 WITH SUBJECT = 'mssql-extension test-only backup encryption second';
CREATE CERTIFICATE tde_cert WITH SUBJECT = 'mssql-extension test-only TDE';
CREATE CERTIFICATE tde_cert2 WITH SUBJECT = 'mssql-extension test-only TDE rotated';
GO
BACKUP CERTIFICATE enc_cert TO FILE = '$(DIR)/enc_cert.cer'
	WITH PRIVATE KEY (FILE = '$(DIR)/enc_cert.pvk', ENCRYPTION BY PASSWORD = '$(PVKPASS)');
BACKUP CERTIFICATE enc_cert2 TO FILE = '$(DIR)/enc_cert2.cer'
	WITH PRIVATE KEY (FILE = '$(DIR)/enc_cert2.pvk', ENCRYPTION BY PASSWORD = '$(PVKPASS)');
BACKUP CERTIFICATE tde_cert TO FILE = '$(DIR)/tde_cert.cer'
	WITH PRIVATE KEY (FILE = '$(DIR)/tde_cert.pvk', ENCRYPTION BY PASSWORD = '$(PVKPASS)');
BACKUP CERTIFICATE tde_cert2 TO FILE = '$(DIR)/tde_cert2.cer'
	WITH PRIVATE KEY (FILE = '$(DIR)/tde_cert2.pvk', ENCRYPTION BY PASSWORD = '$(PVKPASS)');
GO
-- The PFX form of the first (E4b: accepted on 2025; refused on E4's build -- an error here is the answer).
BEGIN TRY
	EXEC (N'BACKUP CERTIFICATE enc_cert TO FILE = ''$(DIR)/enc_cert.pfx'' WITH FORMAT = ''PFX'', PRIVATE KEY (ENCRYPTION BY PASSWORD = ''$(PVKPASS)'', ALGORITHM = ''AES_256'')');
	PRINT 'PFX: written';
END TRY
BEGIN CATCH
	PRINT CONCAT('PFX: refused: ', ERROR_MESSAGE());
END CATCH
GO
-- The databases: one plain, two under TDE; the same tables in each (ints, strings, a decimal, a date, a LOB).
DECLARE @db sysname;
DECLARE dbs CURSOR FOR SELECT v FROM (VALUES ('bak_enc'), ('bak_tde256'), ('bak_tde128')) t(v);
OPEN dbs; FETCH NEXT FROM dbs INTO @db;
WHILE @@FETCH_STATUS = 0
BEGIN
	EXEC (N'IF DB_ID(''' + @db + N''') IS NOT NULL DROP DATABASE ' + @db + N'; CREATE DATABASE ' + @db + N';');
	EXEC (N'USE ' + @db + N';
CREATE TABLE dbo.t_rows (id int PRIMARY KEY, name nvarchar(40) NOT NULL, amount decimal(12, 2) NULL, d date NULL, note varchar(200) NULL);
WITH n AS (SELECT TOP (5000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.t_rows SELECT i, CONCAT(N''имя '', i), i / 7.0, DATEADD(day, i, ''2000-01-01''), REPLICATE(''n'', i % 200) FROM n;
CREATE TABLE dbo.t_lob (id int PRIMARY KEY, body nvarchar(max) NOT NULL);
INSERT INTO dbo.t_lob VALUES (1, REPLICATE(CAST(N''lob '' AS nvarchar(max)), 20000)), (2, N''short'');
CREATE TABLE dbo.t_heap (id int NOT NULL, v varbinary(100) NULL);
WITH n AS (SELECT TOP (800) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.t_heap SELECT i, CAST(i AS varbinary(4)) FROM n;');
	FETCH NEXT FROM dbs INTO @db;
END
CLOSE dbs; DEALLOCATE dbs;
GO
USE bak_tde256;
CREATE DATABASE ENCRYPTION KEY WITH ALGORITHM = AES_256 ENCRYPTION BY SERVER CERTIFICATE tde_cert;
ALTER DATABASE bak_tde256 SET ENCRYPTION ON;
GO
USE bak_tde128;
CREATE DATABASE ENCRYPTION KEY WITH ALGORITHM = AES_128 ENCRYPTION BY SERVER CERTIFICATE tde_cert;
ALTER DATABASE bak_tde128 SET ENCRYPTION ON;
GO
USE master;
GO
-- The encryption scans' end (state 3 = encrypted).
DECLARE @tries int = 0;
WHILE EXISTS (SELECT 1 FROM sys.dm_database_encryption_keys k JOIN sys.databases d ON d.database_id = k.database_id
			  WHERE d.name IN ('bak_tde256', 'bak_tde128') AND k.encryption_state <> 3) AND @tries < 120
BEGIN
	WAITFOR DELAY '00:00:01'; SET @tries += 1;
END
IF EXISTS (SELECT 1 FROM sys.dm_database_encryption_keys k JOIN sys.databases d ON d.database_id = k.database_id
		   WHERE d.name IN ('bak_tde256', 'bak_tde128') AND k.encryption_state <> 3)
	THROW 50000, 'encrypted.sql: the TDE encryption scans did not end within 120 s', 1;
GO
CHECKPOINT;
GO
-- The plaintext reference of bak_enc.
BACKUP DATABASE bak_enc TO DISK = '$(DIR)/plain_full.bak' WITH FORMAT, INIT, CHECKSUM, NAME = 'plain_full';
GO
-- Encrypted: every algorithm x {none, MS_XPRESS, ZSTD} x {CHECKSUM, none}.
DECLARE @alg sysname, @comp nvarchar(80), @ck nvarchar(20), @name nvarchar(80), @sql nvarchar(max);
DECLARE algs CURSOR FOR SELECT a FROM (VALUES ('AES_128'), ('AES_192'), ('AES_256'), ('TRIPLE_DES_3KEY')) t(a);
OPEN algs; FETCH NEXT FROM algs INTO @alg;
WHILE @@FETCH_STATUS = 0
BEGIN
	DECLARE comps CURSOR FOR SELECT c FROM (VALUES ('none'), ('xpress'), ('zstd')) t(c);
	OPEN comps; FETCH NEXT FROM comps INTO @comp;
	WHILE @@FETCH_STATUS = 0
	BEGIN
		DECLARE cks CURSOR FOR SELECT c FROM (VALUES ('ck'), ('nock')) t(c);
		OPEN cks; FETCH NEXT FROM cks INTO @ck;
		WHILE @@FETCH_STATUS = 0
		BEGIN
			SET @name = LOWER(CONCAT('enc_', @alg, '_', @comp, '_', @ck));
			SET @sql = CONCAT(N'BACKUP DATABASE bak_enc TO DISK = ''$(DIR)/', @name, N'.bak'' WITH FORMAT, INIT, NAME = ''', @name, N''', ',
				CASE @ck WHEN 'ck' THEN N'CHECKSUM, ' ELSE N'NO_CHECKSUM, ' END,
				CASE @comp WHEN 'none' THEN N'NO_COMPRESSION, ' WHEN 'xpress' THEN N'COMPRESSION, ' ELSE N'COMPRESSION (ALGORITHM = ZSTD), ' END,
				N'ENCRYPTION (ALGORITHM = ', @alg, N', SERVER CERTIFICATE = enc_cert)');
			BEGIN TRY
				EXEC (@sql);
				PRINT CONCAT('written: ', @name);
			END TRY
			BEGIN CATCH
				PRINT CONCAT('refused: ', @name, ': ', ERROR_MESSAGE());
			END CATCH
			FETCH NEXT FROM cks INTO @ck;
		END
		CLOSE cks; DEALLOCATE cks;
		FETCH NEXT FROM comps INTO @comp;
	END
	CLOSE comps; DEALLOCATE comps;
	FETCH NEXT FROM algs INTO @alg;
END
CLOSE algs; DEALLOCATE algs;
GO
-- Striped, and by the second certificate (another thumbprint); then a change and an encrypted differential.
BACKUP DATABASE bak_enc TO DISK = '$(DIR)/enc_striped_1.bak', DISK = '$(DIR)/enc_striped_2.bak'
	WITH FORMAT, INIT, CHECKSUM, COMPRESSION, NAME = 'enc_striped', ENCRYPTION (ALGORITHM = AES_256, SERVER CERTIFICATE = enc_cert);
BACKUP DATABASE bak_enc TO DISK = '$(DIR)/enc_cert2_full.bak'
	WITH FORMAT, INIT, CHECKSUM, COMPRESSION, NAME = 'enc_cert2_full', ENCRYPTION (ALGORITHM = AES_256, SERVER CERTIFICATE = enc_cert2);
GO
UPDATE bak_enc.dbo.t_rows SET amount = -amount WHERE id % 10 = 0;
DELETE FROM bak_enc.dbo.t_heap WHERE id % 3 = 0;
CHECKPOINT;
GO
BACKUP DATABASE bak_enc TO DISK = '$(DIR)/enc_diff.bak'
	WITH FORMAT, INIT, CHECKSUM, COMPRESSION, DIFFERENTIAL, NAME = 'enc_diff', ENCRYPTION (ALGORITHM = AES_256, SERVER CERTIFICATE = enc_cert);
GO
-- TDE: backups of each, uncompressed and compressed (E4c); an encrypted backup of a TDE database (two keys).
BACKUP DATABASE bak_tde256 TO DISK = '$(DIR)/tde256_full.bak' WITH FORMAT, INIT, CHECKSUM, NO_COMPRESSION, NAME = 'tde256_full';
BACKUP DATABASE bak_tde256 TO DISK = '$(DIR)/tde256_full_xpress.bak' WITH FORMAT, INIT, CHECKSUM, COMPRESSION, NAME = 'tde256_full_xpress';
BACKUP DATABASE bak_tde256 TO DISK = '$(DIR)/tde256_full_xpress_nock.bak' WITH FORMAT, INIT, NO_CHECKSUM, COMPRESSION, NAME = 'tde256_full_xpress_nock';
BACKUP DATABASE bak_tde128 TO DISK = '$(DIR)/tde128_full.bak' WITH FORMAT, INIT, CHECKSUM, NO_COMPRESSION, NAME = 'tde128_full';
BACKUP DATABASE bak_tde128 TO DISK = '$(DIR)/tde128_full_xpress.bak' WITH FORMAT, INIT, CHECKSUM, COMPRESSION, NAME = 'tde128_full_xpress';
BACKUP DATABASE bak_tde256 TO DISK = '$(DIR)/tde256_enc_full.bak'
	WITH FORMAT, INIT, CHECKSUM, COMPRESSION, NAME = 'tde256_enc_full', ENCRYPTION (ALGORITHM = AES_256, SERVER CERTIFICATE = enc_cert);
GO
BEGIN TRY
	EXEC (N'BACKUP DATABASE bak_tde256 TO DISK = ''$(DIR)/tde256_full_zstd.bak'' WITH FORMAT, INIT, CHECKSUM, COMPRESSION (ALGORITHM = ZSTD), NAME = ''tde256_full_zstd''');
	PRINT 'written: tde256_full_zstd';
END TRY
BEGIN CATCH
	PRINT CONCAT('refused: tde256_full_zstd: ', ERROR_MESSAGE());
END CATCH
GO
-- The DEK rotated to tde_cert2 (the same key, re-wrapped: E4b), a change, a full and a differential; then a
-- compressed differential over the full.
USE bak_tde256;
ALTER DATABASE ENCRYPTION KEY ENCRYPTION BY SERVER CERTIFICATE tde_cert2;
GO
USE master;
UPDATE bak_tde256.dbo.t_rows SET name = CONCAT(N'after rotation ', id) WHERE id % 50 = 0;
CHECKPOINT;
GO
BACKUP DATABASE bak_tde256 TO DISK = '$(DIR)/tde256_rotated_full.bak' WITH FORMAT, INIT, CHECKSUM, NO_COMPRESSION, NAME = 'tde256_rotated_full';
UPDATE bak_tde256.dbo.t_rows SET amount = 0 WHERE id % 77 = 0;
CHECKPOINT;
BACKUP DATABASE bak_tde256 TO DISK = '$(DIR)/tde256_rotated_diff.bak' WITH FORMAT, INIT, CHECKSUM, NO_COMPRESSION, DIFFERENTIAL, NAME = 'tde256_rotated_diff';
UPDATE bak_tde256.dbo.t_rows SET amount = 1 WHERE id % 91 = 0;
CHECKPOINT;
BACKUP DATABASE bak_tde256 TO DISK = '$(DIR)/tde256_diff_xpress.bak' WITH FORMAT, INIT, CHECKSUM, COMPRESSION, DIFFERENTIAL, NAME = 'tde256_diff_xpress';
GO
-- A tiny TDE database backed up three ways: the same pages plain, MS_XPRESS and ZSTD (E4c's comparison).
IF DB_ID('bak_tde_small') IS NOT NULL DROP DATABASE bak_tde_small;
CREATE DATABASE bak_tde_small;
GO
USE bak_tde_small;
CREATE TABLE dbo.t (id int PRIMARY KEY, v varchar(20) NOT NULL);
INSERT INTO dbo.t VALUES (1, 'one'), (2, 'two'), (3, 'three');
CREATE DATABASE ENCRYPTION KEY WITH ALGORITHM = AES_256 ENCRYPTION BY SERVER CERTIFICATE tde_cert;
ALTER DATABASE bak_tde_small SET ENCRYPTION ON;
GO
USE master;
DECLARE @tries int = 0;
WHILE EXISTS (SELECT 1 FROM sys.dm_database_encryption_keys k WHERE k.database_id = DB_ID('bak_tde_small') AND k.encryption_state <> 3) AND @tries < 120
BEGIN WAITFOR DELAY '00:00:01'; SET @tries += 1; END
CHECKPOINT;
BACKUP DATABASE bak_tde_small TO DISK = '$(DIR)/tde_small_plain.bak' WITH FORMAT, INIT, CHECKSUM, NO_COMPRESSION, NAME = 'tde_small_plain';
BACKUP DATABASE bak_tde_small TO DISK = '$(DIR)/tde_small_xpress.bak' WITH FORMAT, INIT, CHECKSUM, COMPRESSION, NAME = 'tde_small_xpress';
GO
BEGIN TRY
	EXEC (N'BACKUP DATABASE bak_tde_small TO DISK = ''$(DIR)/tde_small_zstd.bak'' WITH FORMAT, INIT, CHECKSUM, COMPRESSION (ALGORITHM = ZSTD), NAME = ''tde_small_zstd''');
	PRINT 'written: tde_small_zstd';
END TRY
BEGIN CATCH
	PRINT CONCAT('refused: tde_small_zstd: ', ERROR_MESSAGE());
END CATCH
GO
SELECT d.name, k.encryption_state, k.key_algorithm, k.encryptor_type, CONVERT(varchar(64), k.encryptor_thumbprint, 2) AS thumbprint
FROM sys.dm_database_encryption_keys k JOIN sys.databases d ON d.database_id = k.database_id ORDER BY d.name;
SELECT name, CONVERT(varchar(64), thumbprint, 2) AS thumbprint, key_length, pvt_key_encryption_type_desc FROM sys.certificates
WHERE name LIKE '%cert%' ORDER BY name;
GO
