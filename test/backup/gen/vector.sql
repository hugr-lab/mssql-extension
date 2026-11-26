-- Spec 093 T8d: SQL Server 2025's vector in a backup, read by T3's kernel from its stored bytes -- the 0xF5 wire
-- value's (measured/V4.md): float32 and float16 at their edges and at their dimension limits (1998, 3996; #644),
-- NULLs; in every storage form -- a clustered table, a heap, ROW- and PAGE-compressed tables, a value pushed off
-- row (row-overflow) -- then vector_changes.sql's changes and a differential (vector_over). Run by default, on 2025
-- only (generate.sh skips it on older servers, which have no vector), so the oracle lane compares it with a
-- RESTORE through TYPE mssql, which reads a vector as FLOAT[n] (T3); 2025's committed under
-- test/data/backup/vector (made by its generate.sh). Compressed: small. Synthetic values only (T4).
SET NOCOUNT ON;
GO
IF DB_ID('bak_vector') IS NOT NULL DROP DATABASE bak_vector;
GO
CREATE DATABASE bak_vector;
GO
USE bak_vector;
GO
-- float16 vectors are a preview feature of SQL Server 2025.
ALTER DATABASE SCOPED CONFIGURATION SET PREVIEW_FEATURES = ON;
GO
-- 1. float32 and float16 values at their edges (float16: 65504 the largest, 6.1035156e-05 the smallest normal,
-- 5.9604645e-08 the smallest subnormal; all exact in float16), and NULLs. A NULL vector is stored with no header.
CREATE TABLE dbo.vec_edges (id int PRIMARY KEY, v vector(3) NULL, h vector(3, float16) NULL);
INSERT INTO dbo.vec_edges VALUES
	(1, '[1, 2, 3]', '[1, 2, 3]'),
	(2, '[-1.5, 0, 0.25]', '[-1.5, 0, 0.25]'),
	(3, '[3.4028235e38, -3.4028235e38, 1.1754944e-38]', '[65504, -65504, 6.1035156e-05]'),
	(4, '[1e-45, -0, 0.1]', '[5.9604645e-08, -0, 0.1]'),
	(5, NULL, '[7, 8, 9]'),
	(6, '[4, 5, 6]', NULL),
	(7, NULL, NULL);
GO
-- A numbers table (GENERATE_SERIES needs compatibility level 160).
CREATE TABLE dbo.nums (n int PRIMARY KEY);
WITH n AS (SELECT TOP (4000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.nums SELECT i FROM n;
GO
-- 2. The dimension limits: vector(1998) and vector(3996, float16), both 8000 bytes a value (#644).
CREATE TABLE dbo.vec_limits (id int PRIMARY KEY, v vector(1998) NULL, h vector(3996, float16) NULL);
INSERT INTO dbo.vec_limits
SELECT i,
	CAST('[' + (SELECT STRING_AGG(e, ',') WITHIN GROUP (ORDER BY n) FROM (SELECT n, CAST(CAST((n + i) % 97 AS float) / 8 AS varchar(max)) AS e FROM dbo.nums WHERE n <= 1998) d) + ']' AS vector(1998)),
	CAST('[' + (SELECT STRING_AGG(e, ',') WITHIN GROUP (ORDER BY n) FROM (SELECT n, CAST(CAST((n * i) % 61 AS float) / 4 AS varchar(max)) AS e FROM dbo.nums WHERE n <= 3996) d) + ']' AS vector(3996, float16))
FROM (SELECT n AS i FROM dbo.nums WHERE n <= 6) AS g;
INSERT INTO dbo.vec_limits VALUES (7, NULL, NULL);
GO
-- 3. A clustered table of generated values, 2000 rows, some NULL.
CREATE TABLE dbo.vec_many (id int PRIMARY KEY, tag varchar(20) NOT NULL, v vector(16) NULL, h vector(8, float16) NULL);
WITH n AS (SELECT TOP (2000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.vec_many
SELECT i, CONCAT('t', i % 7),
	CASE WHEN i % 13 = 0 THEN NULL ELSE CAST('[' + (SELECT STRING_AGG(e, ',') WITHIN GROUP (ORDER BY n) FROM (SELECT n, CAST(CAST((n * 31 + i) % 1000 AS float) / 64 - 7.5 AS varchar(max)) AS e FROM dbo.nums WHERE n <= 16) d) + ']' AS vector(16)) END,
	CASE WHEN i % 11 = 0 THEN NULL ELSE CAST('[' + (SELECT STRING_AGG(e, ',') WITHIN GROUP (ORDER BY n) FROM (SELECT n, CAST(CAST((n * 17 + i) % 500 AS float) / 16 - 15 AS varchar(max)) AS e FROM dbo.nums WHERE n <= 8) d) + ']' AS vector(8, float16)) END
FROM n;
GO
-- 4. A heap, and ROW- and PAGE-compressed tables: a vector is stored as its varbinary bytes in every form.
CREATE TABLE dbo.vec_heap (id int NOT NULL, v vector(4) NULL, h vector(4, float16) NULL);
INSERT INTO dbo.vec_heap SELECT id, CAST(CONCAT('[', id, ',', -id, ',', id % 7, ',0.5]') AS vector(4)),
	CAST(CONCAT('[', id % 100, ',', -(id % 50), ',0.25,1]') AS vector(4, float16)) FROM dbo.vec_many WHERE id <= 300;
CREATE TABLE dbo.vec_row (id int PRIMARY KEY, v vector(16) NULL, h vector(8, float16) NULL) WITH (DATA_COMPRESSION = ROW);
INSERT INTO dbo.vec_row SELECT id, v, h FROM dbo.vec_many WHERE id <= 500;
CREATE TABLE dbo.vec_page (id int PRIMARY KEY, tag varchar(20) NOT NULL, v vector(16) NULL, h vector(8, float16) NULL)
	WITH (DATA_COMPRESSION = PAGE);
INSERT INTO dbo.vec_page SELECT id, tag, v, h FROM dbo.vec_many;
GO
-- 5. Off row: a vector(1998) beside a long varchar, so one of them is pushed to row-overflow.
CREATE TABLE dbo.vec_ovf (id int PRIMARY KEY, filler varchar(8000) NULL, v vector(1998) NULL);
INSERT INTO dbo.vec_ovf SELECT id, REPLICATE('x', 7000), v FROM dbo.vec_limits WHERE id <= 3;
INSERT INTO dbo.vec_ovf SELECT id, 'short', v FROM dbo.vec_limits WHERE id BETWEEN 4 AND 6;
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_vector TO DISK = '$(DIR)/vector_full.bak' WITH FORMAT, INIT, CHECKSUM, COMPRESSION, NAME = 'vector_full';
GO
-- 6 is vector_changes.sql's, which generate.sh runs after the golden catalog of vector_full.
