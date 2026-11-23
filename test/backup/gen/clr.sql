-- Spec 085 / 093: CLR columns of every storage form, for local development only. The user types of
-- test/clr/TestTypes.cs (generate.sh writes clr_assembly.sql from test/data/clr/mssql_test_udt.dll.hex):
-- dbo.TestPoint, Format.Native, 9 bytes -- stored at a fixed offset, which sysrscols describes as binary(9)
-- (measured on 2022 and 2025: offsets 8 and 17 for a nullable and a NOT NULL one); dbo.TestLabel, Format.UserDefined
-- with MaxByteSize -1, one value past 8000 bytes (off row); hierarchyid and geometry beside them. The same
-- table uncompressed, ROW- and PAGE-compressed (TestPoint in the variable part there), and a heap.
-- Compressed (MS_XPRESS), so the committed backup under test/data/backup/clr stays small. The values'
-- bytes as the server gives them (CAST AS varbinary) are printed at the end: the test pins them.
SET NOCOUNT ON;
SET QUOTED_IDENTIFIER ON;
GO
EXEC sp_configure 'show advanced options', 1;
RECONFIGURE;
EXEC sp_configure 'clr enabled', 1;
RECONFIGURE;
GO
IF DB_ID('bak_clr') IS NOT NULL DROP DATABASE bak_clr;
GO
CREATE DATABASE bak_clr;
GO
USE bak_clr;
GO
:r /tmp/gen/clr_assembly.sql
GO
CREATE TYPE dbo.TestPoint EXTERNAL NAME MssqlTestUdt.[MssqlTest.TestPoint];
CREATE TYPE dbo.TestLabel EXTERNAL NAME MssqlTestUdt.[MssqlTest.TestLabel];
GO
CREATE TABLE dbo.clr_plain (id int NOT NULL, p dbo.TestPoint NULL, pn dbo.TestPoint NOT NULL, l dbo.TestLabel NULL,
	h hierarchyid NULL, g geometry NULL, c char(3) NULL, CONSTRAINT pk_clr_plain PRIMARY KEY (id));
CREATE TABLE dbo.clr_row (id int NOT NULL, p dbo.TestPoint NULL, pn dbo.TestPoint NOT NULL, l dbo.TestLabel NULL,
	h hierarchyid NULL, g geometry NULL, c char(3) NULL, CONSTRAINT pk_clr_row PRIMARY KEY (id))
	WITH (DATA_COMPRESSION = ROW);
CREATE TABLE dbo.clr_page (id int NOT NULL, p dbo.TestPoint NULL, pn dbo.TestPoint NOT NULL, l dbo.TestLabel NULL,
	h hierarchyid NULL, g geometry NULL, c char(3) NULL, CONSTRAINT pk_clr_page PRIMARY KEY (id))
	WITH (DATA_COMPRESSION = PAGE);
CREATE TABLE dbo.clr_heap (id int NOT NULL, p dbo.TestPoint NULL, pn dbo.TestPoint NOT NULL, l dbo.TestLabel NULL,
	h hierarchyid NULL, g geometry NULL, c char(3) NULL);
GO
INSERT INTO dbo.clr_plain VALUES
	(1, '1,2', '3,4', 'one', '/1/', geometry::Point(1, 2, 0), 'abc'),
	(2, NULL, '-7,2147483647', REPLICATE(CAST(N'x' AS nvarchar(max)), 9000), '/1/2/', NULL, NULL),
	(3, '0,0', '-2147483648,0', NULL, NULL, geometry::STGeomFromText('LINESTRING(0 0, 1 1)', 0), 'z');
INSERT INTO dbo.clr_row SELECT * FROM dbo.clr_plain;
INSERT INTO dbo.clr_page SELECT * FROM dbo.clr_plain;
INSERT INTO dbo.clr_heap SELECT * FROM dbo.clr_plain;
ALTER INDEX pk_clr_page ON dbo.clr_page REBUILD;
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_clr TO DISK = '$(DIR)/clr_full.bak' WITH FORMAT, INIT, CHECKSUM, COMPRESSION, NAME = 'clr_full';
GO
-- Where each column is stored (leaf_offset < 0: the variable part) and the values' bytes, for the test.
SELECT o.name AS tbl, c.name AS col, pc.system_type_id AS stored_xtype, pc.leaf_offset, pc.max_inrow_length
FROM sys.system_internals_partition_columns pc
JOIN sys.partitions p ON p.partition_id = pc.partition_id
JOIN sys.objects o ON o.object_id = p.object_id
JOIN sys.columns c ON c.object_id = o.object_id AND c.column_id = pc.partition_column_id
WHERE o.name LIKE 'clr[_]%' AND p.index_id IN (0, 1) ORDER BY o.name, c.column_id;
SELECT id, CAST(p AS varbinary(20)) AS p, CAST(pn AS varbinary(20)) AS pn, DATALENGTH(l) AS l_bytes,
	CAST(h AS varbinary(20)) AS h, CAST(g AS varbinary(100)) AS g, c
FROM dbo.clr_plain ORDER BY id;
GO
