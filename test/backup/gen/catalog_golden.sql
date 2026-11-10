-- Spec 085 PR 2b: the server's catalog of the current database, written beside a backup of it
-- through sqlcmd's :OUT, one batch per file (sqlcmd runs a batch at its GO): <set>.tables.txt
-- (shape, partitions, rows, dates), .columns.txt, .views.txt (definition length and SHA-256) --
-- what test/backup/compare_catalog.py compares mssql_backup_tables / _views with. Run by
-- generate.sh's golden() with -d <the database the backup was taken of> -v OUTPREFIX=<dir>/<the set>
-- -s '|' -W, right after that backup, nothing changed since.
SET NOCOUNT ON;
:OUT $(OUTPREFIX).tables.txt
SELECT s.name AS schema_name, t.name AS table_name, t.object_id,
	   CASE i.type WHEN 0 THEN 'heap' WHEN 1 THEN 'clustered' WHEN 5 THEN 'columnstore' ELSE CONCAT('type ', i.type) END AS shape,
	   (SELECT COUNT(*) FROM sys.partitions p WHERE p.object_id = t.object_id AND p.index_id = i.index_id) AS partition_count,
	   (SELECT SUM(p.rows) FROM sys.partitions p WHERE p.object_id = t.object_id AND p.index_id = i.index_id) AS row_count,
	   CONVERT(varchar(23), t.create_date, 121) AS created, CONVERT(varchar(23), t.modify_date, 121) AS modified
FROM sys.tables t JOIN sys.schemas s ON s.schema_id = t.schema_id JOIN sys.indexes i ON i.object_id = t.object_id AND i.index_id <= 1
WHERE t.is_ms_shipped = 0
ORDER BY s.name, t.name;
GO
:OUT $(OUTPREFIX).columns.txt
SELECT s.name AS schema_name, t.name AS table_name, c.column_id, c.name AS column_name, TYPE_NAME(c.user_type_id) AS type_name,
	   c.system_type_id, c.max_length, c.precision, c.scale, c.collation_name, c.is_nullable, c.is_identity, c.is_computed,
	   c.is_sparse, c.is_column_set, c.is_rowguidcol
FROM sys.columns c JOIN sys.tables t ON t.object_id = c.object_id JOIN sys.schemas s ON s.schema_id = t.schema_id
WHERE t.is_ms_shipped = 0
ORDER BY s.name, t.name, c.column_id;
GO
:OUT $(OUTPREFIX).views.txt
SELECT s.name AS schema_name, v.name AS view_name, v.object_id,
	   CASE WHEN m.definition IS NULL THEN 1 ELSE 0 END AS is_encrypted,
	   DATALENGTH(m.definition) / 2 AS definition_length,	-- UTF-16 units, trailing blanks included
	   CONVERT(varchar(64), HASHBYTES('SHA2_256', CAST(m.definition AS nvarchar(max))), 2) AS definition_sha256
FROM sys.views v JOIN sys.schemas s ON s.schema_id = v.schema_id LEFT JOIN sys.sql_modules m ON m.object_id = v.object_id
WHERE v.is_ms_shipped = 0
ORDER BY s.name, v.name;
GO
:OUT stdout
