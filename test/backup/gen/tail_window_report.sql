-- Spec 085 D11: what tail_window's backup restores to, for mssql_backup_tail_window.test: each table's
-- rows after a RESTORE (the tail redone, the open transaction undone), and the transactions the log
-- began (their names and, where fn_dblog has the column, their parents: a parallel plan's nested ones).
SET NOCOUNT ON;
GO
IF DB_ID('bak_tail_window_restored') IS NOT NULL DROP DATABASE bak_tail_window_restored;
GO
RESTORE DATABASE bak_tail_window_restored FROM DISK = '$(DIR)/tail_window_full.bak' WITH RECOVERY,
	MOVE 'bak_tail_window' TO '$(DIR)/tail_window_restored.mdf',
	MOVE 'bak_tail_window_log' TO '$(DIR)/tail_window_restored.ldf';
GO
USE bak_tail_window_restored;
GO
SELECT 'restored' AS what, t.name AS tbl, p.rows
FROM sys.tables t
CROSS APPLY (SELECT SUM(ps.row_count) AS rows FROM sys.dm_db_partition_stats ps
	WHERE ps.object_id = t.object_id AND ps.index_id IN (0, 1)) p
ORDER BY t.name;
SELECT 'restored' AS what, 'stale_update' AS tbl, SUM(v) AS sum_v FROM dbo.stale_update
UNION ALL SELECT 'restored', 'savepoint', SUM(v) FROM dbo.savepoint
UNION ALL SELECT 'restored', 'widened', SUM(v) FROM dbo.widened;
GO
USE bak_tail_window;
GO
BEGIN TRY
	EXEC (N'SELECT ''began'' AS what, [Current LSN], [Transaction ID], [Transaction Name], [Parent Transaction ID]
		FROM fn_dblog(NULL, NULL) WHERE Operation = ''LOP_BEGIN_XACT''
		AND ([Transaction Name] IN (''tw_undo'', ''tw_parallel'') OR [Parent Transaction ID] <> ''0000:00000000'')
		ORDER BY [Current LSN]');
END TRY
BEGIN CATCH
	SELECT 'began' AS what, ERROR_MESSAGE() AS no_parent_column;
END CATCH;
GO
-- Whether the open transaction's INSERT ran in parallel (its plan's degree).
SELECT 'parallel insert' AS what, MAX(qs.last_dop) AS last_dop
FROM sys.dm_exec_query_stats qs CROSS APPLY sys.dm_exec_sql_text(qs.sql_handle) t
WHERE t.text LIKE '%INSERT INTO dbo.parallel_open%' AND t.text NOT LIKE '%dm_exec_query_stats%';
GO
USE master;
GO
DROP DATABASE bak_tail_window_restored;
GO
