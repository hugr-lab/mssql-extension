# B: DuckDB 2.0 + mssql 0.3.0 (pushdown) -- tpch_sf1_columnstore

## q13: client 1.039 s, 1 statement(s), 150000 rows to the client, server 1024 ms elapsed / 1024 ms CPU

- 1 exec, 150000 rows, 1024.2 ms elapsed, 1024.1 ms CPU, 7498 reads: `SELECT [r1].[c_custkey] AS [c_custkey], COUNT_BIG([r2].[o_orderkey]) AS [count(o_orderkey)] FROM [dbo].[customer] AS [r1] LEFT JOIN [dbo].[orders] AS [r2] ON (([r1].[c_custkey] = [r2].[o_custkey]) AND (NOT ([r2].[o_comment] LIKE @p0))) GROUP BY [r1].[c_custkey]`

## q18: client 0.156 s, 4 statement(s), 72 rows to the client, server 109 ms elapsed / 476 ms CPU

- 1 exec, 57 rows, 105.7 ms elapsed, 473.3 ms CPU, 30527 reads: `SELECT TOP (100) CAST([r1].[c_name] AS NVARCHAR(25)) AS [c_name], [r1].[c_custkey] AS [c_custkey], [r2].[o_orderkey] AS [o_orderkey], [r2].[o_orderdate] AS [o_orderdate], [r2].[o_totalprice] AS [o_totalprice], SUM([r3].[l_quantity]) AS [sum(l_quantity)] FROM [dbo].[customer] AS [r1] CROSS JOIN [dbo].[orders] AS [r2] CROSS JOIN [dbo].[lineitem] AS [r3] WHERE (([r2].[o_orderkey] IN (SELECT [r4].[l_orderkey] AS [l_orderkey] FROM [dbo].[lineitem] AS [r4] GROUP BY [r4].[l_orderkey] HAVING (SUM([r4].[l_quantity]) > @p0))) AND ([r1].[c_custkey] = [r2].[o_custkey]) AND ([r2].[o_orderkey] = [r3].[l_ord`
- 3 exec, 3 rows, 1.4 ms elapsed, 1.4 ms CPU, 37 reads: `select																																	@sumCountExecutions = sum(case when (execution_type = 0 or @accountAbortedFlag = 1) then count_executions else 0 end),				@sumCountAborted = sum(case when execution_type in(3, 4) then count_executions else 0 end),											@sumCpuTime = sum(case when (execution_type = 0 or @accountAbortedFlag = 1) then total_cpu_time else 0 end),						@sumSquareCpuTime = sum(case when (execution_type = 0 or @accountAbortedFlag = 1)																				 then sumsquare_cpu_time else 0 end) / @APRC_SUM_SQUARE_CPU_SCALE									from sys.plan_persist_runtime_stats_merged as r `
- 3 exec, 9 rows, 1.0 ms elapsed, 1.0 ms CPU, 60 reads: `insert into @intervals(interval_id)																									select runtime_stats_interval_id																										from sys.plan_persist_runtime_stats_interval	with(nolock)																		where end_time >= @startTime`
- 3 exec, 3 rows, 0.5 ms elapsed, 0.5 ms CPU, 39 reads: `insert into @plansToCheck(plan_id)																									select @planId`

## q06: client 0.041 s, 1 statement(s), 114160 rows to the client, server 36 ms elapsed / 33 ms CPU

- 1 exec, 114160 rows, 35.8 ms elapsed, 32.9 ms CPU, 5285 reads: `SELECT [l_discount], [l_extendedprice] FROM [dbo].[lineitem] WHERE (([l_shipdate] >= @p0) AND ([l_shipdate] < @p1)) AND ([l_discount] BETWEEN @p2 AND @p3) AND ([l_quantity] < @p4)`

