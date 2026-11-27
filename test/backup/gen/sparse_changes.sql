-- Spec 086 PR 3: the sparse fixture's second stage (generate.sh runs it after sparse.sql and the golden catalog
-- of sparse_full): changes inside the vectors, then a differential over the full (sparse_diff).
SET NOCOUNT ON;
SET QUOTED_IDENTIFIER ON;	-- the filtered index on types_sparse needs it for every write
GO
USE bak_sparse;
GO
-- 12. Values set, changed and cleared inside the vector (an entry added, rewritten, removed; a value grown off
-- row and back), rows inserted and deleted; a column made sparse, its rows rewritten with their values the same.
UPDATE dbo.types_sparse SET s_int = -id, s_vc = NULL, s_guid = '11111111-2222-3333-4444-555555555555'
	WHERE id % 50 = 7 AND id > 100;
UPDATE dbo.types_sparse SET s_vcmax = REPLICATE(CAST('G' AS varchar(max)), 9000) WHERE id IN (110, 120);
UPDATE dbo.types_sparse SET s_vcmax = 'shrunk' WHERE id = 5;
WITH n AS (SELECT TOP (300) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT dbo.types_sparse (id, s_small, s_nvc, r1) SELECT 20000 + i, i, CONCAT(N'new_', i), 'ins' FROM n;
DELETE FROM dbo.types_sparse WHERE id BETWEEN 200 AND 240;
UPDATE dbo.wide_sparse SET w777 = id, w3 = NULL WHERE id % 9 = 0;
UPDATE dbo.ovf_sparse SET b = REPLICATE('B', 4000) WHERE id = 3;
UPDATE dbo.heap_sparse SET s = REPLICATE('S', 3500) WHERE id % 40 = 11;
UPDATE dbo.uniq_sparse SET s1 = 0 WHERE k = 3;
ALTER TABLE dbo.alter_sparse ALTER COLUMN v ADD SPARSE;	-- every row rewritten, its values the same
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_sparse TO DISK = '$(DIR)/sparse_diff.bak' WITH FORMAT, INIT, CHECKSUM, COMPRESSION, DIFFERENTIAL, NAME = 'sparse_diff';
GO
