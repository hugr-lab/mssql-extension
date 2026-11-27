-- Spec 093 T8b: the variant fixture's second stage (generate.sh runs it after variant.sql and the golden catalog
-- of variant_full): values changed -- to another base type, to NULL and back, grown off row and back -- rows
-- inserted and deleted, then a differential over the full (variant_diff, paired as variant_over).
SET NOCOUNT ON;
GO
USE bak_variant;
GO
-- 7. Changes in every table.
UPDATE dbo.v_edges SET v = CAST(CONCAT(N'was ', id) AS sql_variant) WHERE id % 5 = 0;
UPDATE dbo.v_edges SET v = NULL WHERE id IN (3, 13);
UPDATE dbo.v_edges SET v = CAST(CAST(48 AS bigint) AS sql_variant) WHERE id = 48;
UPDATE dbo.v_mixed SET v = CAST(CAST(-id AS smallint) AS sql_variant) WHERE id % 97 = 1;
UPDATE dbo.v_mixed SET v = CAST(CAST(id AS decimal(38, 10)) / 3 AS sql_variant) WHERE id % 89 = 2;
DELETE FROM dbo.v_mixed WHERE id BETWEEN 500 AND 560;
WITH n AS (SELECT TOP (200) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.v_mixed SELECT 10000 + i, 'new', CAST(CAST(DATEADD(minute, i, '2030-01-01') AS smalldatetime) AS sql_variant) FROM n;
UPDATE dbo.v_heap SET v = CAST(REPLICATE(N'д', 3500) AS sql_variant) WHERE id % 40 = 11;
UPDATE dbo.v_heap SET v = CAST(id AS sql_variant) WHERE id % 40 = 7;
UPDATE dbo.v_row SET v = CAST(CAST(id AS real) AS sql_variant) WHERE id % 7 = 0;
UPDATE dbo.v_page SET v = CAST('other text' AS sql_variant) WHERE id % 11 = 0;
UPDATE dbo.v_ovf SET v = CAST(1 AS sql_variant) WHERE id = 1;
UPDATE dbo.v_ovf SET v = CAST(REPLICATE('e', 7000) AS varchar(7000)) WHERE id = 4;
INSERT INTO dbo.v_key VALUES (CAST(1 AS sql_variant), 8), (CAST(CAST(2 AS tinyint) AS sql_variant), 9);
DELETE FROM dbo.v_key WHERE n = 3;
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_variant TO DISK = '$(DIR)/variant_diff.bak' WITH FORMAT, INIT, CHECKSUM, COMPRESSION, DIFFERENTIAL, NAME = 'variant_diff';
GO
