-- Spec 086 PR 6 (E9b): SQL Server 2025's json type in a backup -- the stored binary form (E9) read by the
-- backup reader as TYPE mssql reads the column (the server's text, JSON) or, under json_as 'variant', as
-- DuckDB's own JSON -> VARIANT gives that text. E9's four corpora: every JSON type in every container,
-- the length-prefix widths, string escapes and Unicode, integers at every width and sign edge, decimals
-- and exponents, nesting to the limit, wide documents (u32 counts), shared and escaped keys; the same rows
-- uncompressed, ROW and PAGE; values changed in place (JSON_MODIFY, .modify()) on pages not rewritten
-- since; a json column added with a default (metadata only); values off row. Then json_changes.sql's
-- changes and a differential (json_over). SQL Server 2025 only (the type is 2025's): generate.sh skips it
-- below major 17. 2025's committed under test/data/backup/json. Synthetic values only (T4).
SET NOCOUNT ON;
GO
IF DB_ID('bak_json') IS NOT NULL DROP DATABASE bak_json;
GO
CREATE DATABASE bak_json;
GO
USE bak_json;
GO
-- 1. The corpus: one value a row, each a top-level object or array (a bare scalar is refused, E9).
CREATE TABLE dbo.j_corpus (id int PRIMARY KEY, j json NULL);
INSERT INTO dbo.j_corpus VALUES
	(1, '{}'), (2, '[]'), (3, '{"":""}'), (4, '[""]'), (5, '{"a":{}}'), (6, '{"a":[]}'), (7, '[[],{}]'),
	(8, '{"t":true,"f":false,"n":null}'), (9, '[true,false,null]'),
	(10, '{"a":1,"a":2}'), (11, '{"b":1,"a":2,"c":3}'), (12, '{"A":1,"a":2}'),
	(13, '[0,1,-1,536870911,536870912,-536870912,-536870913]'),
	(14, '[2147483647,-2147483648,2147483648,9223372036854775807,-9223372036854775808]'),
	(15, '[0.5,-0.5,2.50,1.000,0.1,123456789.123456789,-0.0000001]'),
	(16, '[12345678901234567890123456789012345678,-12345678901234567890123456789012345678]'),
	(17, '[0.12345678901234567890123456789012345678,1234567890123456789.1234567890123456789]'),
	(18, '[1E2,1e-10,-1.5e3,2E+5,5e-324,-0,0.0]'),
	(19, '["quote\" backslash\\ slash\/ b\b f\f n\n r\r t\t","\u0001\u001f\u007f"]'),
	(20, N'["é","日本語","😀","😀","é","Ω"]'),
	(21, N'{"a\"b":1,"c\\d":2,"\u0041":3,"é":4,"😀":5}'),
	(22, '{"nested":{"a":{"b":{"c":[1,{"d":[2,3]}]}}}}'),
	(23, NULL),
	-- Keys whose escapes a reader unescapes: two that unescape alike (DuckDB's JSON -> VARIANT keeps the last),
	-- control characters, \u escapes of a non-ASCII character and of a surrogate pair.
	(24, '{"a\/b":1,"a/b":2}'),
	(25, '{"line\nbreak":1,"\u000a":2,"tab\t":3,"q\u0022":4}'),
	(26, N'{"\u00e9t\u00e9":1,"\uD83D\uDE00":2,"x\/":3}'),
	(27, N'{"z":1,"é":2,"a":3,"Z":4,"~":5,"\u007f":6}'),
	-- A lone surrogate: the server keeps it in a key as written and turns it into U+FFFD in a string; DuckDB's
	-- JSON parse refuses the key (the VARIANT NULL, the text the server's).
	(28, N'{"\uD800":["\uD800"]}');
-- Strings across the length-prefix widths (1, 2 and 3 bytes: < 128, < 16384, < 2^21), around each edge.
INSERT INTO dbo.j_corpus SELECT 100 + n, CONCAT('["', REPLICATE(CAST('s' AS varchar(max)), n), '"]')
FROM (VALUES (0), (1), (126), (127), (128), (129), (16382), (16383), (16384), (16385), (70000)) v(n);
-- Keys across the widths too, to a key's limit: 7,998 bytes (E9b: 13646 past it, so a key's length is never
-- 3 bytes).
INSERT INTO dbo.j_corpus SELECT 200 + n, CONCAT('{"', REPLICATE(CAST('k' AS varchar(max)), n), '":1,"x":2}')
FROM (VALUES (1), (127), (128), (7997), (7998)) v(n);
-- Nesting to the limit, 128 (129 is refused).
INSERT INTO dbo.j_corpus SELECT 300 + n, CONCAT(REPLICATE(CAST('[' AS varchar(max)), n), '1', REPLICATE(CAST(']' AS varchar(max)), n))
FROM (VALUES (1), (32), (64), (127), (128)) v(n);
INSERT INTO dbo.j_corpus SELECT 310 + n, CONCAT(REPLICATE(CAST('{"o":' AS varchar(max)), n), '1', REPLICATE(CAST('}' AS varchar(max)), n))
FROM (VALUES (1), (64), (128)) v(n);
GO
-- A 10,000-element array, a 1,000- and a 2,000-key object, 3,000 strings in one array.
WITH n AS (SELECT TOP (10000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.j_corpus SELECT 400, CONCAT('[', STRING_AGG(CAST(i AS varchar(max)), ','), ']') FROM n;
WITH n AS (SELECT TOP (1000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.j_corpus SELECT 401, CONCAT('{', STRING_AGG(CAST(CONCAT('"k', i, '":', i) AS varchar(max)), ','), '}') FROM n;
WITH n AS (SELECT TOP (2000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.j_corpus SELECT 402, CONCAT('{', STRING_AGG(CAST(CONCAT('"key', i, '":"v', i, '"') AS varchar(max)), ','), '}') FROM n;
WITH n AS (SELECT TOP (3000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.j_corpus SELECT 403, CONCAT('[', STRING_AGG(CAST(CONCAT('"str', i, '"') AS varchar(max)), ','), ']') FROM n;
GO
-- 2. Wide documents (u32 counts and key references): a 70,000-key object; keys shared by nested objects.
WITH n AS (SELECT TOP (70000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.j_corpus SELECT 500, CONCAT('{', STRING_AGG(CAST(CONCAT('"w', i, '":', i) AS varchar(max)), ','), '}') FROM n;
-- Around the narrow / wide boundary: a narrow key reference's top bit is the deleted mark, so a document
-- past 32,767 keys must be wide (E9b measures where the server switches).
WITH n AS (SELECT TOP (65535) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b),
	 c AS (SELECT * FROM (VALUES (510, 32767), (511, 32768), (512, 65535)) v (id, keys))
INSERT INTO dbo.j_corpus SELECT c.id, CONCAT('{', STRING_AGG(CAST(CONCAT('"b', n.i, '":', n.i) AS varchar(max)), ','), '}')
FROM c JOIN n ON n.i <= c.keys GROUP BY c.id;
WITH n AS (SELECT TOP (400) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.j_corpus SELECT 501, CONCAT('[', STRING_AGG(CAST(CONCAT('{"id":', i, ',"name":"n', i, '","tags":["a","b"],"price":', i, '.25}') AS varchar(max)), ','), ']') FROM n;
GO
-- 3. Generated documents, each kind of member, many rows (pages full, then ROW / PAGE below).
CREATE TABLE dbo.j_rows (id int PRIMARY KEY, j json NULL);
WITH n AS (SELECT TOP (1500) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.j_rows SELECT i, CASE WHEN i % 23 = 0 THEN NULL ELSE CONCAT('{"id":', i, ',"neg":', -i * 1000003, ',"d":', i, '.', i % 100,
	',"s":"row ', i, '","b":', IIF(i % 2 = 0, 'true', 'false'), ',"a":[', i % 7, ',', i % 11, ',null],"o":{"x":', i % 13, '}}') END FROM n;
CREATE TABLE dbo.j_row (id int PRIMARY KEY, j json NULL) WITH (DATA_COMPRESSION = ROW);
INSERT INTO dbo.j_row SELECT id, j FROM dbo.j_corpus WHERE id < 500;
INSERT INTO dbo.j_row SELECT 10000 + id, j FROM dbo.j_rows;
CREATE TABLE dbo.j_page (id int PRIMARY KEY, j json NULL) WITH (DATA_COMPRESSION = PAGE);
INSERT INTO dbo.j_page SELECT id, j FROM dbo.j_corpus WHERE id < 500;
INSERT INTO dbo.j_page SELECT 10000 + id, j FROM dbo.j_rows;
ALTER TABLE dbo.j_page REBUILD WITH (DATA_COMPRESSION = PAGE);
GO
-- 4. Changed in place (the old items and nodes kept, the trailer naming the new root): JSON_MODIFY's replace,
-- add, delete, a string grown, an array appended, a nested change; .modify(); a value's type changed twice;
-- add / delete / re-add. No rebuild after: the documents stay as the changes left them.
CREATE TABLE dbo.j_mod (id int PRIMARY KEY, j json NOT NULL);
INSERT INTO dbo.j_mod VALUES (1, '{"a":1,"b":"x","c":[1,2],"d":{"e":1}}'), (2, '{"a":1,"b":"x","c":[1,2],"d":{"e":1}}'),
	(3, '{"a":1,"b":"x","c":[1,2],"d":{"e":1}}'), (4, '{"a":1,"b":"x","c":[1,2],"d":{"e":1}}'),
	(5, '{"a":1,"b":"x","c":[1,2],"d":{"e":1}}'), (6, '{"a":1}');
UPDATE dbo.j_mod SET j = JSON_MODIFY(j, '$.a', 2) WHERE id = 1;
UPDATE dbo.j_mod SET j = JSON_MODIFY(j, '$.z', 'added') WHERE id = 2;
UPDATE dbo.j_mod SET j = JSON_MODIFY(j, '$.b', NULL) WHERE id = 3;
UPDATE dbo.j_mod SET j = JSON_MODIFY(j, '$.b', REPLICATE('y', 20000)) WHERE id = 4;
UPDATE dbo.j_mod SET j = JSON_MODIFY(j, 'append $.c', 3) WHERE id = 5;
UPDATE dbo.j_mod SET j = JSON_MODIFY(j, '$.d.e', 'nested') WHERE id = 5;
UPDATE dbo.j_mod SET j.modify('$.a', 'text') WHERE id = 6;
UPDATE dbo.j_mod SET j.modify('$.a', 7) WHERE id = 6;
UPDATE dbo.j_mod SET j.modify('$.n', 1) WHERE id = 6;
UPDATE dbo.j_mod SET j = JSON_MODIFY(j, '$.n', NULL) WHERE id = 6;
UPDATE dbo.j_mod SET j.modify('$.n', 2) WHERE id = 6;
GO
-- 5. A json column added with a default: metadata only, the rows written before hold no value of it.
CREATE TABLE dbo.j_added (id int PRIMARY KEY, v int NOT NULL);
INSERT INTO dbo.j_added SELECT TOP (300) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)), 1 FROM sys.all_objects;
ALTER TABLE dbo.j_added ADD jd json NOT NULL CONSTRAINT df_jd DEFAULT ('{"default":[1,2]}');
ALTER TABLE dbo.j_added ADD jn json NULL;
INSERT INTO dbo.j_added (id, v, jn) VALUES (1000, 2, '{"after":true}');
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_json TO DISK = '$(DIR)/json_full.bak' WITH FORMAT, INIT, CHECKSUM, COMPRESSION, NAME = 'json_full';
GO
-- 6 is json_changes.sql's, which generate.sh runs after the golden catalog of json_full.
