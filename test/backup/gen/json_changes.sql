-- Spec 086 PR 6: the json fixture's second stage (generate.sh runs it after json.sql and the golden catalog
-- of json_full): documents replaced, changed in place again, set to NULL and back, rows inserted and deleted,
-- then a differential over the full (json_diff, paired as json_over).
SET NOCOUNT ON;
GO
USE bak_json;
GO
-- 6. Changes in every table.
UPDATE dbo.j_corpus SET j = '{"replaced":true}' WHERE id IN (1, 15, 400);
UPDATE dbo.j_corpus SET j = NULL WHERE id = 11;
UPDATE dbo.j_corpus SET j = '[1]' WHERE id = 23;
UPDATE dbo.j_corpus SET j.modify('$.w1', 'changed') WHERE id = 500;
UPDATE dbo.j_rows SET j = JSON_MODIFY(j, '$.s', 'changed') WHERE id % 50 = 0;
DELETE FROM dbo.j_rows WHERE id BETWEEN 700 AND 760;
INSERT INTO dbo.j_rows VALUES (5001, '{"new":[1,2,3]}'), (5002, '[]');
UPDATE dbo.j_mod SET j = JSON_MODIFY(j, '$.a', 3) WHERE id = 1;
UPDATE dbo.j_mod SET j.modify('$.d', '{"deep":1}') WHERE id = 5;
UPDATE dbo.j_added SET jn = '{"set":1}' WHERE id % 30 = 0;
UPDATE dbo.j_page SET j = '{"page":"changed"}' WHERE id % 97 = 0;
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_json TO DISK = '$(DIR)/json_diff.bak' WITH FORMAT, INIT, CHECKSUM, COMPRESSION, DIFFERENTIAL, NAME = 'json_diff';
GO
