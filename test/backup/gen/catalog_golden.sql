-- Spec 085 PR 2b: the server's catalog of the current database, written beside a backup of it
-- through sqlcmd's :OUT, one batch per file (sqlcmd runs a batch at its GO): <set>.tables.txt
-- (shape, partitions, rows, dates), .columns.txt, .views.txt (definition length and SHA-256),
-- .modules.txt (the sysobjvalues rows mssql_backup_catalog may serve), .partitioning.txt /
-- .boundaries.txt / .partitions.txt (spec 085 PR 2c) --
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
:OUT $(OUTPREFIX).modules.txt
-- The sysobjvalues rows mssql_backup_catalog may serve (DQ-11), by (valclass, objid, subobjid): the
-- definitions of the views not WITH ENCRYPTION and of the defaults, the computed columns of user
-- tables, the boundaries of the partition functions. Nothing else -- a procedure's, a function's, a
-- trigger's or a check's source -- may appear.
SELECT valclass, objid, subobjid FROM (
	SELECT 1 AS valclass, o.object_id AS objid, 0 AS subobjid FROM sys.objects o
	WHERE o.is_ms_shipped = 0 AND (o.type = 'D' OR (o.type = 'V' AND OBJECTPROPERTY(o.object_id, 'IsEncrypted') = 0))
	UNION
	SELECT 2, c.object_id, c.column_id FROM sys.computed_columns c JOIN sys.tables t ON t.object_id = c.object_id
	WHERE t.is_ms_shipped = 0
	UNION
	SELECT 30, v.function_id, v.boundary_id FROM sys.partition_range_values v
) m
ORDER BY valclass, objid, subobjid;
GO
:OUT $(OUTPREFIX).partitioning.txt
-- A partitioned table's base index (heap or clustered): its function, scheme, partitioning column,
-- parameter type, side and NEXT USED filegroup (the destination past the fanout).
SELECT s.name AS schema_name, t.name AS table_name, pf.name AS function_name, ps.name AS scheme_name, c.name AS column_name,
	   c.column_id, TYPE_NAME(pp.user_type_id) AS parameter_type, pp.max_length, pp.precision, pp.scale,
	   CAST(pf.boundary_value_on_right AS int) AS range_right, pf.fanout, nu.name AS next_used
FROM sys.tables t JOIN sys.schemas s ON s.schema_id = t.schema_id
JOIN sys.indexes i ON i.object_id = t.object_id AND i.index_id <= 1
JOIN sys.partition_schemes ps ON ps.data_space_id = i.data_space_id
JOIN sys.partition_functions pf ON pf.function_id = ps.function_id
JOIN sys.partition_parameters pp ON pp.function_id = pf.function_id AND pp.parameter_id = 1
JOIN sys.index_columns ic ON ic.object_id = t.object_id AND ic.index_id = i.index_id AND ic.partition_ordinal = 1
JOIN sys.columns c ON c.object_id = t.object_id AND c.column_id = ic.column_id
LEFT JOIN sys.destination_data_spaces nd ON nd.partition_scheme_id = ps.data_space_id AND nd.destination_id = pf.fanout + 1
LEFT JOIN sys.data_spaces nu ON nu.data_space_id = nd.data_space_id
WHERE t.is_ms_shipped = 0
ORDER BY s.name, t.name;
GO
:OUT $(OUTPREFIX).boundaries.txt
-- Each partitioned table's boundaries in boundary_id order, as text: the date family in style 121 at
-- 7 digits, binary as 0x hex, a GUID lower case, money in style 2 (4 decimals; style 0 rounds to 2),
-- float and real in style 3 (round-trip), bit as 0 / 1, anything else as nvarchar.
SELECT s.name AS schema_name, t.name AS table_name, rv.boundary_id,
	   CAST(SQL_VARIANT_PROPERTY(rv.value, 'BaseType') AS varchar(30)) AS base_type,
	   CASE CAST(SQL_VARIANT_PROPERTY(rv.value, 'BaseType') AS varchar(30))
		 WHEN 'date' THEN CONVERT(nvarchar(80), CAST(rv.value AS date), 23)
		 WHEN 'datetime' THEN CONVERT(nvarchar(80), CAST(rv.value AS datetime), 121)
		 WHEN 'smalldatetime' THEN CONVERT(nvarchar(80), CAST(rv.value AS smalldatetime), 121)
		 WHEN 'datetime2' THEN CONVERT(nvarchar(80), CAST(rv.value AS datetime2(7)), 121)
		 WHEN 'datetimeoffset' THEN CONVERT(nvarchar(80), CAST(rv.value AS datetimeoffset(7)), 121)
		 WHEN 'time' THEN CONVERT(nvarchar(80), CAST(rv.value AS time(7)), 121)
		 WHEN 'varbinary' THEN CONVERT(nvarchar(80), CAST(rv.value AS varbinary(8000)), 1)
		 WHEN 'binary' THEN CONVERT(nvarchar(80), CAST(rv.value AS varbinary(8000)), 1)
		 WHEN 'uniqueidentifier' THEN LOWER(CAST(rv.value AS nvarchar(80)))
		 WHEN 'money' THEN CONVERT(nvarchar(80), CAST(rv.value AS money), 2)
		 WHEN 'smallmoney' THEN CONVERT(nvarchar(80), CAST(rv.value AS smallmoney), 2)
		 WHEN 'float' THEN CONVERT(nvarchar(80), CAST(rv.value AS float), 3)
		 WHEN 'real' THEN CONVERT(nvarchar(80), CAST(rv.value AS real), 3)
		 WHEN 'bit' THEN CAST(CAST(rv.value AS int) AS nvarchar(80))
		 ELSE CAST(rv.value AS nvarchar(80)) END AS value_text
FROM sys.tables t JOIN sys.schemas s ON s.schema_id = t.schema_id
JOIN sys.indexes i ON i.object_id = t.object_id AND i.index_id <= 1
JOIN sys.partition_schemes ps ON ps.data_space_id = i.data_space_id
JOIN sys.partition_range_values rv ON rv.function_id = ps.function_id
WHERE t.is_ms_shipped = 0
ORDER BY s.name, t.name, rv.boundary_id;
GO
:OUT $(OUTPREFIX).partitions.txt
-- Every table's base partitions: number, filegroup (the scheme's destination, or the index's own),
-- rows, compression.
SELECT s.name AS schema_name, t.name AS table_name, p.partition_number, COALESCE(dfg.name, ifg.name) AS filegroup,
	   p.rows, p.data_compression_desc AS compression
FROM sys.tables t JOIN sys.schemas s ON s.schema_id = t.schema_id
JOIN sys.indexes i ON i.object_id = t.object_id AND i.index_id <= 1
JOIN sys.partitions p ON p.object_id = t.object_id AND p.index_id = i.index_id
LEFT JOIN sys.destination_data_spaces dd ON dd.partition_scheme_id = i.data_space_id AND dd.destination_id = p.partition_number
LEFT JOIN sys.data_spaces dfg ON dfg.data_space_id = dd.data_space_id
LEFT JOIN sys.data_spaces ifg ON ifg.data_space_id = i.data_space_id AND ifg.type = 'FG'
WHERE t.is_ms_shipped = 0
ORDER BY s.name, t.name, p.partition_number;
GO
:OUT stdout
