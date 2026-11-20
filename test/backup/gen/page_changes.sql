-- Spec 086 PR 2: the page fixture's second stage (generate.sh runs it after page.sql and the golden
-- catalog of page_full, which compare_catalog reads against page_full): changes, then a differential.
SET NOCOUNT ON;
GO
USE bak_page;
GO
-- 13. Changes after the full, then a differential over it (page_diff): rows updated, inserted (pages that split)
-- and deleted under anchors and dictionaries of their own; a table rebuilt whole, every page's anchor and
-- dictionary made anew, its rows the same (no change: compared by their values); and a nullable column added
-- to a PAGE table after the full, then the table rebuilt, its records rewritten with the column and with the
-- defaults section 7 added (stored now, metadata-only in the full) -- its rows unchanged too (review of spec
-- 086 PR 2: compared by the layout's positions, every row was an update). Not here: a column added with a
-- default between the states and then rebuilt, whose every row is an update (the rebuild clears the
-- column's default from sysrscols: nothing says the stored value is the one the older rows read).
UPDATE dbo.types_page SET vc20 = CONCAT('changed_', id % 7), i = i + 1 WHERE id % 50 = 3 AND id > 100;	-- not the edge rows
WITH n AS (SELECT TOP (500) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.types_page (id, vc20, i, c10) SELECT 10000 + i, CONCAT('customer_', RIGHT(CONCAT('0000', i), 5)), i, 'bravo' FROM n;
DELETE FROM dbo.types_page WHERE id BETWEEN 200 AND 260;
UPDATE dbo.uniq_page SET v = 'u_updated' WHERE k = 3 AND n = 7;
DELETE FROM dbo.heap_page WHERE id % 211 = 0;
ALTER INDEX ALL ON dbo.keys_page REBUILD WITH (DATA_COMPRESSION = PAGE, FILLFACTOR = 70);
ALTER TABLE dbo.added_page ADD grown_n int NULL;
ALTER INDEX ALL ON dbo.added_page REBUILD WITH (DATA_COMPRESSION = PAGE);
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_page TO DISK = '$(DIR)/page_diff.bak' WITH FORMAT, INIT, CHECKSUM, DIFFERENTIAL, NAME = 'page_diff';
GO
DBCC TRACEOFF (661, -1);
GO
