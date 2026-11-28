-- Spec 093 T8d: the vector fixture's second stage (generate.sh runs it after vector.sql and the golden catalog of
-- vector_full): values changed, set NULL and back, rows inserted and deleted, then a differential over the full
-- (vector_diff, paired as vector_over).
SET NOCOUNT ON;
GO
USE bak_vector;
GO
-- 6. Changes in every table.
UPDATE dbo.vec_edges SET v = '[9, 9, 9]', h = NULL WHERE id = 1;
UPDATE dbo.vec_edges SET v = NULL WHERE id = 2;
UPDATE dbo.vec_edges SET h = '[0.5, 0.5, 0.5]' WHERE id = 7;
UPDATE dbo.vec_many SET v = NULL WHERE id % 97 = 1;
UPDATE dbo.vec_many SET h = CAST('[1,2,3,4,5,6,7,8]' AS vector(8, float16)) WHERE id % 89 = 2;
DELETE FROM dbo.vec_many WHERE id BETWEEN 500 AND 560;
WITH n AS (SELECT TOP (200) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.vec_many SELECT 10000 + i, 'new', CAST(CONCAT('[', i, ',1,2,3,4,5,6,7,8,9,10,11,12,13,14,15]') AS vector(16)), NULL FROM n;
UPDATE dbo.vec_heap SET v = CAST('[0,0,0,0]' AS vector(4)) WHERE id % 40 = 7;
UPDATE dbo.vec_row SET h = NULL WHERE id % 7 = 0;
UPDATE dbo.vec_page SET v = CAST('[1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1]' AS vector(16)) WHERE id % 11 = 0;
UPDATE dbo.vec_limits SET h = NULL WHERE id = 2;
UPDATE dbo.vec_ovf SET filler = 'short again' WHERE id = 1;
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_vector TO DISK = '$(DIR)/vector_diff.bak' WITH FORMAT, INIT, CHECKSUM, COMPRESSION, DIFFERENTIAL, NAME = 'vector_diff';
GO
