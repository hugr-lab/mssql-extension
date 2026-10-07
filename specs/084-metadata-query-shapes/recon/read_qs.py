import sys
db=sys.argv[1]
q=f"""
WITH qq AS (
 SELECT SUBSTRING(t.query_sql_text, CHARINDEX('/*mdq:', t.query_sql_text) + 6, CHARINDEX('*/', t.query_sql_text) - CHARINDEX('/*mdq:', t.query_sql_text) - 6) AS tag,
        q.query_id, q.count_compiles, q.avg_compile_duration, q.avg_optimize_duration
 FROM {db}.sys.query_store_query q JOIN {db}.sys.query_store_query_text t ON t.query_text_id = q.query_text_id
 WHERE t.query_sql_text LIKE N'%/*mdq:%' AND t.query_sql_text NOT LIKE N'%query_store%')
SELECT qq.tag,
  SUM(qq.count_compiles) AS compiles,
  CAST(SUM(qq.avg_compile_duration * qq.count_compiles) / NULLIF(SUM(qq.count_compiles), 0) / 1000.0 AS decimal(10,2)) AS compile_ms,
  CAST(SUM(qq.avg_optimize_duration * qq.count_compiles) / NULLIF(SUM(qq.count_compiles), 0) / 1000.0 AS decimal(10,2)) AS optimize_ms,
  MAX(CAST(p.is_parallel_plan AS int)) AS parallel,
  SUM(rs.count_executions) AS execs,
  CAST(SUM(rs.avg_cpu_time * rs.count_executions) / NULLIF(SUM(rs.count_executions), 0) / 1000.0 AS decimal(10,2)) AS cpu_ms,
  CAST(SUM(rs.avg_duration * rs.count_executions) / NULLIF(SUM(rs.count_executions), 0) / 1000.0 AS decimal(10,2)) AS dur_ms,
  CAST(SUM(rs.avg_logical_io_reads * rs.count_executions) / NULLIF(SUM(rs.count_executions), 0) AS bigint) AS reads
FROM qq JOIN {db}.sys.query_store_plan p ON p.query_id = qq.query_id
LEFT JOIN {db}.sys.query_store_runtime_stats rs ON rs.plan_id = p.plan_id
GROUP BY qq.tag ORDER BY qq.tag"""
print("ATTACH '${DSN}' AS adm (TYPE mssql);\n.mode markdown\nSELECT * FROM mssql_scan('adm', '"+q.replace("'","''")+"');")
