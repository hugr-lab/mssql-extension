# B: DuckDB 2.0 + mssql 0.3.0 (pushdown) -- tpch_sf10_columnstore_nvarchar

## q01: client 17.688 s, 1 statement(s), 59142609 rows to the client, server 17683 ms elapsed / 17672 ms CPU

- 1 exec, 59142609 rows, 17683.3 ms elapsed, 17672.3 ms CPU, 53865 reads: `SELECT [l_returnflag], [l_linestatus], [l_quantity], [l_extendedprice], [l_discount], [l_tax] FROM [dbo].[lineitem] WHERE ([l_shipdate] <= @p0)`

## q02: client 1.553 s, 5 statement(s), 8107860 rows to the client, server 1547 ms elapsed / 1547 ms CPU

- 1 exec, 8000000 rows, 1404.7 ms elapsed, 1404.6 ms CPU, 8179 reads: `SELECT [ps_partkey], [ps_suppkey], [ps_supplycost] FROM [dbo].[partsupp]`
- 1 exec, 100000 rows, 132.9 ms elapsed, 132.9 ms CPU, 1385 reads: `SELECT [s_suppkey], [s_nationkey], [s_acctbal], [s_name], [s_address], [s_phone], [s_comment] FROM [dbo].[supplier]`
- 1 exec, 7854 rows, 8.5 ms elapsed, 8.5 ms CPU, 1186 reads: `SELECT [p_partkey], [p_mfgr] FROM [dbo].[part] WHERE ([p_type] LIKE @p0) AND ([p_size] = @p1)`
- 1 exec, 1 rows, 0.5 ms elapsed, 0.5 ms CPU, 17 reads: `SELECT [r_regionkey] FROM [dbo].[region] WHERE ([r_name] = @p0)`
- 1 exec, 5 rows, 0.4 ms elapsed, 0.4 ms CPU, 15 reads: `SELECT [n_nationkey], [n_regionkey], [n_name] FROM [dbo].[nation] WHERE ([n_regionkey] = @p0)`

## q03: client 7.574 s, 3 statement(s), 39923968 rows to the client, server 7553 ms elapsed / 7552 ms CPU

- 1 exec, 32334250 rows, 5965.6 ms elapsed, 5964.5 ms CPU, 75093 reads: `SELECT [l_orderkey], [l_extendedprice], [l_discount] FROM [dbo].[lineitem] WHERE ([l_shipdate] > @p0)`
- 1 exec, 7289442 rows, 1561.0 ms elapsed, 1560.9 ms CPU, 15736 reads: `SELECT [o_custkey], [o_orderkey], [o_orderdate], [o_shippriority] FROM [dbo].[orders] WHERE ([o_orderdate] < @p0)`
- 1 exec, 300276 rows, 26.7 ms elapsed, 26.7 ms CPU, 577 reads: `SELECT [c_custkey] FROM [dbo].[customer] WHERE ([c_mktsegment] = @p0)`

## q04: client 10.432 s, 2 statement(s), 38503019 rows to the client, server 10415 ms elapsed / 17434 ms CPU

- 1 exec, 37929348 rows, 10118.6 ms elapsed, 16915.8 ms CPU, 54369 reads: `SELECT [l_orderkey] FROM [dbo].[lineitem] WHERE ([l_orderkey] IS NOT NULL) AND ([l_commitdate] < [l_receiptdate])`
- 1 exec, 573671 rows, 296.8 ms elapsed, 518.7 ms CPU, 23040 reads: `SELECT [o_orderkey], [o_orderpriority] FROM [dbo].[orders] WHERE (([o_orderdate] >= @p0) AND ([o_orderdate] < @p1))`

## q05: client 14.241 s, 6 statement(s), 63861977 rows to the client, server 14234 ms elapsed / 14851 ms CPU

- 1 exec, 59986052 rows, 13099.3 ms elapsed, 13088.4 ms CPU, 82778 reads: `SELECT [l_orderkey], [l_suppkey], [l_extendedprice], [l_discount] FROM [dbo].[lineitem]`
- 1 exec, 2275919 rows, 933.5 ms elapsed, 1561.3 ms CPU, 33239 reads: `SELECT [o_custkey], [o_orderkey] FROM [dbo].[orders] WHERE (([o_orderdate] >= @p0) AND ([o_orderdate] < @p1))`
- 1 exec, 1500000 rows, 187.5 ms elapsed, 187.5 ms CPU, 557 reads: `SELECT [c_custkey], [c_nationkey] FROM [dbo].[customer]`
- 1 exec, 100000 rows, 13.1 ms elapsed, 13.1 ms CPU, 45 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier]`
- 1 exec, 1 rows, 0.4 ms elapsed, 0.4 ms CPU, 17 reads: `SELECT [r_regionkey] FROM [dbo].[region] WHERE ([r_name] = @p0)`
- 1 exec, 5 rows, 0.4 ms elapsed, 0.4 ms CPU, 15 reads: `SELECT [n_nationkey], [n_regionkey], [n_name] FROM [dbo].[nation] WHERE ([n_regionkey] = @p0)`

## q06: client 0.534 s, 1 statement(s), 1139264 rows to the client, server 530 ms elapsed / 1030 ms CPU

- 1 exec, 1139264 rows, 530.2 ms elapsed, 1030.5 ms CPU, 79191 reads: `SELECT [l_discount], [l_extendedprice] FROM [dbo].[lineitem] WHERE (([l_shipdate] >= @p0) AND ([l_shipdate] < @p1)) AND ([l_discount] BETWEEN @p2 AND @p3) AND ([l_quantity] < @p4)`

## q07: client 7.363 s, 6 statement(s), 34830329 rows to the client, server 7354 ms elapsed / 7353 ms CPU

- 1 exec, 18230325 rows, 5138.9 ms elapsed, 5138.2 ms CPU, 94832 reads: `SELECT [l_suppkey], [l_orderkey], [l_shipdate], [l_extendedprice], [l_discount] FROM [dbo].[lineitem] WHERE ([l_shipdate] BETWEEN @p0 AND @p1)`
- 1 exec, 15000000 rows, 2014.9 ms elapsed, 2014.7 ms CPU, 12610 reads: `SELECT [o_orderkey], [o_custkey] FROM [dbo].[orders]`
- 1 exec, 1500000 rows, 185.6 ms elapsed, 185.6 ms CPU, 557 reads: `SELECT [c_custkey], [c_nationkey] FROM [dbo].[customer]`
- 1 exec, 100000 rows, 13.1 ms elapsed, 13.1 ms CPU, 45 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier]`
- 1 exec, 2 rows, 0.6 ms elapsed, 0.6 ms CPU, 13 reads: `SELECT [n_nationkey], [n_name] FROM [dbo].[nation] WHERE (([n_name] = @p0) OR ([n_name] = @p1))`
- 1 exec, 2 rows, 0.6 ms elapsed, 0.6 ms CPU, 13 reads: `SELECT [n_nationkey], [n_name] FROM [dbo].[nation] WHERE (([n_name] = @p0) OR ([n_name] = @p1))`

## q08: client 17.59 s, 8 statement(s), 66157048 rows to the client, server 17576 ms elapsed / 17564 ms CPU

- 1 exec, 59986052 rows, 16543.5 ms elapsed, 16531.4 ms CPU, 102517 reads: `SELECT [l_partkey], [l_suppkey], [l_orderkey], [l_extendedprice], [l_discount] FROM [dbo].[lineitem]`
- 1 exec, 4557513 rows, 824.7 ms elapsed, 824.6 ms CPU, 15636 reads: `SELECT [o_orderkey], [o_custkey], [o_orderdate] FROM [dbo].[orders] WHERE ([o_orderdate] BETWEEN @p0 AND @p1)`
- 1 exec, 1500000 rows, 184.7 ms elapsed, 184.7 ms CPU, 557 reads: `SELECT [c_custkey], [c_nationkey] FROM [dbo].[customer]`
- 1 exec, 100000 rows, 13.0 ms elapsed, 13.0 ms CPU, 45 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier]`
- 1 exec, 13452 rows, 8.6 ms elapsed, 8.6 ms CPU, 1220 reads: `SELECT [p_partkey] FROM [dbo].[part] WHERE ([p_type] = @p0)`
- 1 exec, 25 rows, 0.7 ms elapsed, 0.7 ms CPU, 13 reads: `SELECT [n_nationkey], [n_name] FROM [dbo].[nation]`
- 1 exec, 1 rows, 0.4 ms elapsed, 0.4 ms CPU, 17 reads: `SELECT [r_regionkey] FROM [dbo].[region] WHERE ([r_name] = @p0)`
- 1 exec, 5 rows, 0.3 ms elapsed, 0.3 ms CPU, 11 reads: `SELECT [n_nationkey], [n_regionkey] FROM [dbo].[nation] WHERE ([n_regionkey] = @p0)`

## q09: client 26.404 s, 6 statement(s), 83194859 rows to the client, server 26365 ms elapsed / 26362 ms CPU

- 1 exec, 59986052 rows, 20096.1 ms elapsed, 20093.3 ms CPU, 108673 reads: `SELECT [l_suppkey], [l_partkey], [l_orderkey], [l_extendedprice], [l_discount], [l_quantity] FROM [dbo].[lineitem]`
- 1 exec, 108782 rows, 2821.2 ms elapsed, 2820.8 ms CPU, 7955 reads: `SELECT [p_partkey] FROM [dbo].[part] WHERE ([p_name] LIKE @p0)`
- 1 exec, 15000000 rows, 2035.1 ms elapsed, 2034.9 ms CPU, 10691 reads: `SELECT [o_orderkey], [o_orderdate] FROM [dbo].[orders]`
- 1 exec, 8000000 rows, 1399.6 ms elapsed, 1399.5 ms CPU, 8179 reads: `SELECT [ps_suppkey], [ps_partkey], [ps_supplycost] FROM [dbo].[partsupp]`
- 1 exec, 100000 rows, 13.0 ms elapsed, 13.0 ms CPU, 45 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier]`
- 1 exec, 25 rows, 0.4 ms elapsed, 0.4 ms CPU, 13 reads: `SELECT [n_nationkey], [n_name] FROM [dbo].[nation]`

## q10: client 4.877 s, 4 statement(s), 16881365 rows to the client, server 4843 ms elapsed / 5011 ms CPU

- 1 exec, 14808183 rows, 2776.6 ms elapsed, 2776.2 ms CPU, 63751 reads: `SELECT [l_orderkey], [l_extendedprice], [l_discount] FROM [dbo].[lineitem] WHERE ([l_returnflag] = @p0)`
- 1 exec, 1500000 rows, 1860.0 ms elapsed, 1859.8 ms CPU, 21731 reads: `SELECT [c_custkey], [c_nationkey], [c_name], [c_acctbal], [c_phone], [c_address], [c_comment] FROM [dbo].[customer]`
- 1 exec, 573157 rows, 206.1 ms elapsed, 374.5 ms CPU, 33245 reads: `SELECT [o_custkey], [o_orderkey] FROM [dbo].[orders] WHERE (([o_orderdate] >= @p0) AND ([o_orderdate] < @p1))`
- 1 exec, 25 rows, 0.6 ms elapsed, 0.6 ms CPU, 13 reads: `SELECT [n_nationkey], [n_name] FROM [dbo].[nation]`

## q11: client 3.113 s, 5 statement(s), 16008100 rows to the client, server 3101 ms elapsed / 3100 ms CPU

- 1 exec, 8000000 rows, 1698.0 ms elapsed, 1697.5 ms CPU, 10256 reads: `SELECT [ps_suppkey], [ps_partkey], [ps_supplycost], [ps_availqty] FROM [dbo].[partsupp]`
- 1 exec, 8000000 rows, 1400.0 ms elapsed, 1399.6 ms CPU, 7598 reads: `SELECT [ps_suppkey], [ps_supplycost], [ps_availqty] FROM [dbo].[partsupp]`
- 1 exec, 4049 rows, 1.0 ms elapsed, 1.0 ms CPU, 45 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier] WHERE ([s_nationkey] = @p0)`
- 1 exec, 4049 rows, 1.0 ms elapsed, 1.0 ms CPU, 45 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier] WHERE ([s_nationkey] = @p0)`
- 2 exec, 2 rows, 0.8 ms elapsed, 0.8 ms CPU, 34 reads: `SELECT [n_nationkey] FROM [dbo].[nation] WHERE ([n_name] = @p0)`

## q12: client 2.281 s, 2 statement(s), 15310803 rows to the client, server 2262 ms elapsed / 2702 ms CPU

- 1 exec, 15000000 rows, 2181.1 ms elapsed, 2159.9 ms CPU, 7765 reads: `SELECT [o_orderkey], [o_orderpriority] FROM [dbo].[orders]`
- 1 exec, 310803 rows, 80.6 ms elapsed, 542.6 ms CPU, 100168 reads: `SELECT [l_orderkey], [l_shipmode] FROM [dbo].[lineitem] WHERE ([l_shipmode] IN (@p0, @p1)) AND ([l_commitdate] < [l_receiptdate]) AND ([l_shipdate] < [l_commitdate]) AND (([l_receiptdate] >= @p2) AND ([l_receiptdate] < @p3)) AND ([l_commitdate] < @p4) AND ([l_shipdate] < @p5)`

## q13: client 6.022 s, 1 statement(s), 1500000 rows to the client, server 6005 ms elapsed / 56114 ms CPU

- 1 exec, 1500000 rows, 6004.8 ms elapsed, 56113.8 ms CPU, 104770 reads: `SELECT [r1].[c_custkey] AS [c_custkey], COUNT_BIG([r2].[o_orderkey]) AS [count(o_orderkey)] FROM [dbo].[customer] AS [r1] LEFT JOIN [dbo].[orders] AS [r2] ON (([r1].[c_custkey] = [r2].[o_custkey]) AND (NOT ([r2].[o_comment] LIKE @p0))) GROUP BY [r1].[c_custkey]`

## q14: client 0.972 s, 2 statement(s), 2749223 rows to the client, server 957 ms elapsed / 1398 ms CPU

- 1 exec, 2000000 rows, 573.9 ms elapsed, 573.7 ms CPU, 961 reads: `SELECT [p_partkey], [p_type] FROM [dbo].[part]`
- 1 exec, 749223 rows, 383.1 ms elapsed, 824.6 ms CPU, 99405 reads: `SELECT [l_partkey], [l_extendedprice], [l_discount] FROM [dbo].[lineitem] WHERE (([l_shipdate] >= @p0) AND ([l_shipdate] < @p1))`

## q15: client 1.252 s, 2 statement(s), 2365714 rows to the client, server 1306 ms elapsed / 2257 ms CPU

- 1 exec, 2265714 rows, 1244.6 ms elapsed, 2196.3 ms CPU, 99400 reads: `SELECT [l_suppkey], [l_extendedprice], [l_discount] FROM [dbo].[lineitem] WHERE (([l_shipdate] >= @p0) AND ([l_shipdate] < @p1))`
- 1 exec, 100000 rows, 61.9 ms elapsed, 61.0 ms CPU, 780 reads: `SELECT [s_suppkey], [s_name], [s_address], [s_phone] FROM [dbo].[supplier]`

## q16: client 1.316 s, 3 statement(s), 8396824 rows to the client, server 1349 ms elapsed / 1349 ms CPU

- 1 exec, 8000000 rows, 1140.1 ms elapsed, 1140.0 ms CPU, 5521 reads: `SELECT [ps_partkey], [ps_suppkey] FROM [dbo].[partsupp]`
- 1 exec, 296824 rows, 137.9 ms elapsed, 137.9 ms CPU, 1202 reads: `SELECT [p_partkey], [p_brand], [p_type], [p_size] FROM [dbo].[part] WHERE (NOT ([p_type] LIKE @p0)) AND ([p_size] IN (@p1, @p2, @p3, @p4, @p5, @p6, @p7, @p8)) AND ([p_brand] <> @p9)`
- 1 exec, 100000 rows, 70.8 ms elapsed, 70.8 ms CPU, 608 reads: `SELECT [s_comment], [s_suppkey] FROM [dbo].[supplier]`

## q17: client 19.468 s, 3 statement(s), 119974148 rows to the client, server 19451 ms elapsed / 19448 ms CPU

- 1 exec, 59986052 rows, 11023.4 ms elapsed, 11021.8 ms CPU, 56431 reads: `SELECT [l_partkey], [l_quantity], [l_extendedprice] FROM [dbo].[lineitem]`
- 1 exec, 59986052 rows, 8425.1 ms elapsed, 8423.6 ms CPU, 26955 reads: `SELECT [l_partkey], [l_quantity] FROM [dbo].[lineitem]`
- 1 exec, 2044 rows, 2.6 ms elapsed, 2.6 ms CPU, 776 reads: `SELECT [p_partkey] FROM [dbo].[part] WHERE ([p_brand] = @p0) AND ([p_container] = @p1)`

## q18: client 0.542 s, 1 statement(s), 100 rows to the client, server 502 ms elapsed / 4541 ms CPU

- 1 exec, 100 rows, 501.9 ms elapsed, 4541.0 ms CPU, 170700 reads: `SELECT TOP (100) [r1].[c_name] AS [c_name], [r1].[c_custkey] AS [c_custkey], [r2].[o_orderkey] AS [o_orderkey], [r2].[o_orderdate] AS [o_orderdate], [r2].[o_totalprice] AS [o_totalprice], SUM([r3].[l_quantity]) AS [sum(l_quantity)] FROM [dbo].[customer] AS [r1] CROSS JOIN [dbo].[orders] AS [r2] CROSS JOIN [dbo].[lineitem] AS [r3] WHERE (([r2].[o_orderkey] IN (SELECT [r4].[l_orderkey] AS [l_orderkey] FROM [dbo].[lineitem] AS [r4] GROUP BY [r4].[l_orderkey] HAVING (SUM([r4].[l_quantity]) > @p0))) AND ([r1].[c_custkey] = [r2].[o_custkey]) AND ([r2].[o_orderkey] = [r3].[l_orderkey])) GROUP BY [r1]`

## q19: client 0.873 s, 2 statement(s), 1289098 rows to the client, server 857 ms elapsed / 1650 ms CPU

- 1 exec, 1284344 rows, 850.7 ms elapsed, 1644.1 ms CPU, 93428 reads: `SELECT [l_partkey], [l_quantity], [l_extendedprice], [l_discount] FROM [dbo].[lineitem] WHERE ([l_shipmode] IN (@p0, @p1)) AND ((([l_quantity] >= @p2) AND ([l_quantity] <= @p3)) OR (([l_quantity] >= @p4) AND ([l_quantity] <= @p5)) OR (([l_quantity] >= @p6) AND ([l_quantity] <= @p7))) AND ([l_shipinstruct] = @p8)`
- 1 exec, 4754 rows, 6.2 ms elapsed, 6.2 ms CPU, 948 reads: `SELECT [p_partkey], [p_brand], [p_container], [p_size] FROM [dbo].[part] WHERE ((((([p_brand] = @p0) AND ([p_container] IN (@p1, @p2, @p3, @p4))) AND ([p_size] >= @p5)) AND ([p_size] <= @p6)) OR (((([p_brand] = @p7) AND ([p_container] IN (@p8, @p9, @p10, @p11))) AND ([p_size] >= @p12)) AND ([p_size] <= @p13)) OR (((([p_brand] = @p14) AND ([p_container] IN (@p15, @p16, @p17, @p18))) AND ([p_size] >= @p19)) AND ([p_size] <= @p20))) AND ((([p_brand] = @p21) OR ([p_brand] = @p22)) OR ([p_brand] = @p23)) AND ((([p_container] IN (@p24, @p25, @p26, @p27)) OR ([p_container] IN (@p28, @p29, @p30, @p31)`

## q20: client 6.735 s, 5 statement(s), 17220717 rows to the client, server 6770 ms elapsed / 9964 ms CPU

- 1 exec, 9099165 rows, 4935.8 ms elapsed, 8130.4 ms CPU, 59294 reads: `SELECT [l_partkey], [l_suppkey], [l_quantity] FROM [dbo].[lineitem] WHERE (([l_shipdate] >= @p0) AND ([l_shipdate] < @p1))`
- 1 exec, 8000000 rows, 1370.3 ms elapsed, 1370.2 ms CPU, 7598 reads: `SELECT [ps_partkey], [ps_availqty], [ps_suppkey] FROM [dbo].[partsupp]`
- 1 exec, 21551 rows, 404.9 ms elapsed, 404.8 ms CPU, 7955 reads: `SELECT [p_partkey] FROM [dbo].[part] WHERE ([p_name] LIKE @p0)`
- 1 exec, 100000 rows, 58.1 ms elapsed, 58.1 ms CPU, 666 reads: `SELECT [s_suppkey], [s_nationkey], [s_name], [s_address] FROM [dbo].[supplier]`
- 1 exec, 1 rows, 0.6 ms elapsed, 0.6 ms CPU, 17 reads: `SELECT [n_nationkey] FROM [dbo].[nation] WHERE ([n_name] = @p0)`

## q21: client 19.618 s, 6 statement(s), 143157943 rows to the client, server 19450 ms elapsed / 19438 ms CPU

- 1 exec, 59986052 rows, 7882.8 ms elapsed, 7881.7 ms CPU, 49373 reads: `SELECT [l_orderkey], [l_suppkey] FROM [dbo].[lineitem] WHERE ([l_orderkey] IS NOT NULL) AND ([l_suppkey] IS NOT NULL)`
- 1 exec, 37929348 rows, 5492.5 ms elapsed, 5491.5 ms CPU, 73504 reads: `SELECT [l_orderkey], [l_suppkey] FROM [dbo].[lineitem] WHERE ([l_orderkey] IS NOT NULL) AND ([l_suppkey] IS NOT NULL) AND ([l_receiptdate] > [l_commitdate])`
- 1 exec, 37929348 rows, 5414.0 ms elapsed, 5403.7 ms CPU, 73504 reads: `SELECT [l_suppkey], [l_orderkey] FROM [dbo].[lineitem] WHERE ([l_receiptdate] > [l_commitdate])`
- 1 exec, 7309184 rows, 658.5 ms elapsed, 658.3 ms CPU, 7865 reads: `SELECT [o_orderkey] FROM [dbo].[orders] WHERE ([o_orderstatus] = @p0)`
- 1 exec, 4010 rows, 2.1 ms elapsed, 2.1 ms CPU, 291 reads: `SELECT [s_suppkey], [s_nationkey], [s_name] FROM [dbo].[supplier] WHERE ([s_nationkey] = @p0)`
- 1 exec, 1 rows, 0.5 ms elapsed, 0.5 ms CPU, 17 reads: `SELECT [n_nationkey] FROM [dbo].[nation] WHERE ([n_name] = @p0)`

## q22: client 1.885 s, 3 statement(s), 17863614 rows to the client, server 1869 ms elapsed / 1869 ms CPU

- 1 exec, 15000000 rows, 1309.8 ms elapsed, 1309.5 ms CPU, 5240 reads: `SELECT [o_custkey] FROM [dbo].[orders]`
- 1 exec, 1500000 rows, 316.8 ms elapsed, 316.7 ms CPU, 2789 reads: `SELECT [c_phone], [c_acctbal], [c_custkey] FROM [dbo].[customer]`
- 1 exec, 1363614 rows, 242.5 ms elapsed, 242.5 ms CPU, 2289 reads: `SELECT [c_acctbal], [c_phone] FROM [dbo].[customer] WHERE ([c_acctbal] > @p0)`

