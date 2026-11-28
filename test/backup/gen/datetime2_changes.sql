-- Spec 093 T8c: the datetime2 fixture's second stage (generate.sh runs it after datetime2.sql and the golden
-- catalog of datetime2_full): values moved across TIMESTAMP_NS's edges both ways, to NULL and back, rows
-- inserted and deleted, the temporal table's periods closed, then a differential over the full (datetime2_diff,
-- paired as datetime2_over).
SET NOCOUNT ON;
GO
USE bak_datetime2;
GO
-- 6. Changes in every table.
UPDATE dbo.d_edges SET d7 = '9999-12-31 23:59:59.9999999' WHERE id = 7;
UPDATE dbo.d_edges SET d7 = '2000-02-29 12:34:56.1234567' WHERE id = 10;
UPDATE dbo.d_edges SET d7 = NULL WHERE id = 1;
UPDATE dbo.d_edges SET d7 = '0001-01-01 00:00:00.0000009' WHERE id = 11;
UPDATE dbo.d_temporal SET name = CONCAT('u', id) WHERE id % 5 = 0;
DELETE FROM dbo.d_temporal WHERE id % 19 = 0;
UPDATE dbo.d_mixed SET d7 = DATEADD(year, 8000, d7) WHERE id % 97 = 1 AND d7 < '1990-01-01';
UPDATE dbo.d_mixed SET d7 = NULL WHERE id % 89 = 2;
DELETE FROM dbo.d_mixed WHERE id BETWEEN 500 AND 560;
WITH n AS (SELECT TOP (200) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.d_mixed SELECT 10000 + i, DATEADD(minute, i, CAST('5000-01-01 00:00:00.0000003' AS datetime2(7))), NULL FROM n;
UPDATE dbo.d_heap SET d7 = '0001-01-01 00:00:00.0000001' WHERE id % 40 = 7;
UPDATE dbo.d_row SET d7 = DATEADD(nanosecond, 100, d7) WHERE id % 7 = 0 AND d7 < '9999-12-31';
UPDATE dbo.d_page SET d7 = '1677-09-21 00:12:43.1452241' WHERE id % 11 = 0;
INSERT INTO dbo.d_key VALUES ('9999-12-31 23:59:59.9999999', 7), ('3000-06-01', 8);
DELETE FROM dbo.d_key WHERE n = 3;
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_datetime2 TO DISK = '$(DIR)/datetime2_diff.bak' WITH FORMAT, INIT, CHECKSUM, COMPRESSION, DIFFERENTIAL, NAME = 'datetime2_diff';
GO
