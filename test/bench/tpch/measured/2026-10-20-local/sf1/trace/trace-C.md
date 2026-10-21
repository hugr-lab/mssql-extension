# C: SQL Server (native T-SQL) -- tpch_sf1_columnstore

## q13: client 0.634 s, 1 statement(s), 42 rows to the client, server 618 ms elapsed / 618 ms CPU

- 1 exec, 42 rows, 618.0 ms elapsed, 617.9 ms CPU, 7498 reads: `SELECT     c_count,     count(*) AS custdist FROM (     SELECT         c_custkey,         count(o_orderkey)     FROM         customer     LEFT OUTER JOIN orders ON c_custkey = o_custkey     AND o_comment NOT LIKE '%special%requests%' GROUP BY     c_custkey) AS c_orders (c_custkey,         c_count) GROUP BY     c_count ORDER BY     custdist DESC,     c_count DESC`

## q18: client 0.101 s, 1 statement(s), 57 rows to the client, server 68 ms elapsed / 409 ms CPU

- 1 exec, 57 rows, 68.3 ms elapsed, 409.2 ms CPU, 30522 reads: `SELECT TOP 100     c_name,     c_custkey,     o_orderkey,     o_orderdate,     o_totalprice,     sum(l_quantity) FROM     customer,     orders,     lineitem WHERE     o_orderkey IN (         SELECT             l_orderkey         FROM             lineitem         GROUP BY             l_orderkey         HAVING             sum(l_quantity) > 300)     AND c_custkey = o_custkey     AND o_orderkey = l_orderkey GROUP BY     c_name,     c_custkey,     o_orderkey,     o_orderdate,     o_totalprice ORDER BY     o_totalprice DESC,     o_orderdate`

## q06: client 0.019 s, 4 statement(s), 16 rows to the client, server 15 ms elapsed / 15 ms CPU

- 1 exec, 1 rows, 13.0 ms elapsed, 13.0 ms CPU, 5285 reads: `SELECT SUM([l_extendedprice]*[l_discount]) [revenue] FROM [lineitem] WHERE [l_shipdate]>=CONVERT([date],@1) AND [l_shipdate]<CONVERT([date],@2) AND [l_discount]>=@3 AND [l_discount]<=@4 AND [l_quantity]<@5`
- 3 exec, 3 rows, 1.0 ms elapsed, 1.0 ms CPU, 21 reads: `select																																	@sumCountExecutions = sum(case when (execution_type = 0 or @accountAbortedFlag = 1) then count_executions else 0 end),				@sumCountAborted = sum(case when execution_type in(3, 4) then count_executions else 0 end),											@sumCpuTime = sum(case when (execution_type = 0 or @accountAbortedFlag = 1) then total_cpu_time else 0 end),						@sumSquareCpuTime = sum(case when (execution_type = 0 or @accountAbortedFlag = 1)																				 then sumsquare_cpu_time else 0 end) / @APRC_SUM_SQUARE_CPU_SCALE									from sys.plan_persist_runtime_stats_merged as r `
- 3 exec, 9 rows, 0.8 ms elapsed, 0.8 ms CPU, 60 reads: `insert into @intervals(interval_id)																									select runtime_stats_interval_id																										from sys.plan_persist_runtime_stats_interval	with(nolock)																		where end_time >= @startTime`
- 3 exec, 3 rows, 0.6 ms elapsed, 0.6 ms CPU, 39 reads: `insert into @plansToCheck(plan_id)																									select @planId`

