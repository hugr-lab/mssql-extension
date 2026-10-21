# B: DuckDB 2.0 + mssql 0.3.0 (pushdown) -- tpch_sf10_columnstore_utf8

## q01: client 19.469 s, 1 statement(s), 59142609 rows to the client, server 19464 ms elapsed / 19461 ms CPU

- 1 exec, 59142609 rows, 19463.5 ms elapsed, 19461.0 ms CPU, 53847 reads: `SELECT [l_returnflag], [l_linestatus], [l_quantity], [l_extendedprice], [l_discount], [l_tax] FROM [dbo].[lineitem] WHERE ([l_shipdate] <= @p0)`

## q02: client 0.161 s, 1 statement(s), 100 rows to the client, server 98 ms elapsed / 98 ms CPU

- 1 exec, 100 rows, 98.5 ms elapsed, 98.5 ms CPU, 18829 reads: `SELECT TOP (100) [r2].[s_acctbal] AS [s_acctbal], [r2].[s_name] AS [s_name], [r4].[n_name] AS [n_name], [r1].[p_partkey] AS [p_partkey], [r1].[p_mfgr] AS [p_mfgr], [r2].[s_address] AS [s_address], [r2].[s_phone] AS [s_phone], [r2].[s_comment] AS [s_comment] FROM [dbo].[part] AS [r1] CROSS JOIN [dbo].[supplier] AS [r2] CROSS JOIN [dbo].[partsupp] AS [r3] CROSS JOIN [dbo].[nation] AS [r4] CROSS JOIN [dbo].[region] AS [r5] WHERE (([r1].[p_partkey] = [r3].[ps_partkey]) AND ([r2].[s_suppkey] = [r3].[ps_suppkey]) AND ([r1].[p_size] = @p0) AND ([r1].[p_type] LIKE @p1) AND ([r2].[s_nationkey] = [r4].[`

## q03: client 7.524 s, 3 statement(s), 39923968 rows to the client, server 7502 ms elapsed / 7501 ms CPU

- 1 exec, 32334250 rows, 5931.2 ms elapsed, 5930.3 ms CPU, 75279 reads: `SELECT [l_orderkey], [l_extendedprice], [l_discount] FROM [dbo].[lineitem] WHERE ([l_shipdate] > @p0)`
- 1 exec, 7289442 rows, 1543.5 ms elapsed, 1543.2 ms CPU, 15736 reads: `SELECT [o_custkey], [o_orderkey], [o_orderdate], [o_shippriority] FROM [dbo].[orders] WHERE ([o_orderdate] < @p0)`
- 1 exec, 300276 rows, 27.2 ms elapsed, 27.2 ms CPU, 569 reads: `SELECT [c_custkey] FROM [dbo].[customer] WHERE ([c_mktsegment] = @p0)`

## q04: client 10.215 s, 2 statement(s), 38503019 rows to the client, server 10198 ms elapsed / 17210 ms CPU

- 1 exec, 37929348 rows, 9947.7 ms elapsed, 16779.5 ms CPU, 54540 reads: `SELECT [l_orderkey] FROM [dbo].[lineitem] WHERE ([l_orderkey] IS NOT NULL) AND ([l_commitdate] < [l_receiptdate])`
- 1 exec, 573671 rows, 250.4 ms elapsed, 430.3 ms CPU, 23117 reads: `SELECT [o_orderkey], [o_orderpriority] FROM [dbo].[orders] WHERE (([o_orderdate] >= @p0) AND ([o_orderdate] < @p1))`

## q05: client 14.149 s, 6 statement(s), 63861977 rows to the client, server 14140 ms elapsed / 14720 ms CPU

- 1 exec, 59986052 rows, 13057.2 ms elapsed, 13055.4 ms CPU, 82963 reads: `SELECT [l_orderkey], [l_suppkey], [l_extendedprice], [l_discount] FROM [dbo].[lineitem]`
- 1 exec, 2275919 rows, 882.3 ms elapsed, 1464.2 ms CPU, 33311 reads: `SELECT [o_custkey], [o_orderkey] FROM [dbo].[orders] WHERE (([o_orderdate] >= @p0) AND ([o_orderdate] < @p1))`
- 1 exec, 1500000 rows, 186.3 ms elapsed, 186.3 ms CPU, 551 reads: `SELECT [c_custkey], [c_nationkey] FROM [dbo].[customer]`
- 1 exec, 100000 rows, 12.9 ms elapsed, 12.9 ms CPU, 45 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier]`
- 1 exec, 5 rows, 0.4 ms elapsed, 0.4 ms CPU, 13 reads: `SELECT [n_nationkey], [n_regionkey], [n_name] FROM [dbo].[nation] WHERE ([n_regionkey] = @p0)`
- 1 exec, 1 rows, 0.4 ms elapsed, 0.4 ms CPU, 13 reads: `SELECT [r_regionkey] FROM [dbo].[region] WHERE ([r_name] = @p0)`

## q06: client 0.538 s, 1 statement(s), 1139264 rows to the client, server 532 ms elapsed / 1028 ms CPU

- 1 exec, 1139264 rows, 532.5 ms elapsed, 1028.2 ms CPU, 79172 reads: `SELECT [l_discount], [l_extendedprice] FROM [dbo].[lineitem] WHERE (([l_shipdate] >= @p0) AND ([l_shipdate] < @p1)) AND ([l_discount] BETWEEN @p2 AND @p3) AND ([l_quantity] < @p4)`

## q07: client 7.292 s, 5 statement(s), 34830329 rows to the client, server 7292 ms elapsed / 7268 ms CPU

- 1 exec, 18230325 rows, 5116.9 ms elapsed, 5116.1 ms CPU, 95011 reads: `SELECT [l_suppkey], [l_orderkey], [l_shipdate], [l_extendedprice], [l_discount] FROM [dbo].[lineitem] WHERE ([l_shipdate] BETWEEN @p0 AND @p1)`
- 1 exec, 15000000 rows, 1950.7 ms elapsed, 1950.4 ms CPU, 12591 reads: `SELECT [o_orderkey], [o_custkey] FROM [dbo].[orders]`
- 1 exec, 1500000 rows, 198.4 ms elapsed, 187.7 ms CPU, 551 reads: `SELECT [c_custkey], [c_nationkey] FROM [dbo].[customer]`
- 1 exec, 100000 rows, 25.0 ms elapsed, 12.9 ms CPU, 45 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier]`
- 2 exec, 4 rows, 0.7 ms elapsed, 0.7 ms CPU, 22 reads: `SELECT [n_nationkey], [n_name] FROM [dbo].[nation] WHERE (([n_name] = @p0) OR ([n_name] = @p1))`

## q08: client 17.498 s, 8 statement(s), 66157048 rows to the client, server 17481 ms elapsed / 17473 ms CPU

- 1 exec, 59986052 rows, 16437.1 ms elapsed, 16435.0 ms CPU, 102695 reads: `SELECT [l_partkey], [l_suppkey], [l_orderkey], [l_extendedprice], [l_discount] FROM [dbo].[lineitem]`
- 1 exec, 4557513 rows, 827.7 ms elapsed, 827.6 ms CPU, 15636 reads: `SELECT [o_orderkey], [o_custkey], [o_orderdate] FROM [dbo].[orders] WHERE ([o_orderdate] BETWEEN @p0 AND @p1)`
- 1 exec, 1500000 rows, 187.5 ms elapsed, 187.5 ms CPU, 551 reads: `SELECT [c_custkey], [c_nationkey] FROM [dbo].[customer]`
- 1 exec, 100000 rows, 15.6 ms elapsed, 12.6 ms CPU, 45 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier]`
- 1 exec, 13452 rows, 10.8 ms elapsed, 7.8 ms CPU, 1223 reads: `SELECT [p_partkey] FROM [dbo].[part] WHERE ([p_type] = @p0)`
- 1 exec, 1 rows, 0.9 ms elapsed, 0.9 ms CPU, 13 reads: `SELECT [r_regionkey] FROM [dbo].[region] WHERE ([r_name] = @p0)`
- 1 exec, 25 rows, 0.7 ms elapsed, 0.7 ms CPU, 11 reads: `SELECT [n_nationkey], [n_name] FROM [dbo].[nation]`
- 1 exec, 5 rows, 0.5 ms elapsed, 0.5 ms CPU, 11 reads: `SELECT [n_nationkey], [n_regionkey] FROM [dbo].[nation] WHERE ([n_regionkey] = @p0)`

## q09: client 26.24 s, 6 statement(s), 83194859 rows to the client, server 26201 ms elapsed / 26197 ms CPU

- 1 exec, 59986052 rows, 19353.6 ms elapsed, 19350.5 ms CPU, 108850 reads: `SELECT [l_suppkey], [l_partkey], [l_orderkey], [l_extendedprice], [l_discount], [l_quantity] FROM [dbo].[lineitem]`
- 1 exec, 108782 rows, 3424.6 ms elapsed, 3424.2 ms CPU, 7934 reads: `SELECT [p_partkey] FROM [dbo].[part] WHERE ([p_name] LIKE @p0)`
- 1 exec, 15000000 rows, 2023.6 ms elapsed, 2023.3 ms CPU, 10711 reads: `SELECT [o_orderkey], [o_orderdate] FROM [dbo].[orders]`
- 1 exec, 8000000 rows, 1386.1 ms elapsed, 1386.0 ms CPU, 8204 reads: `SELECT [ps_suppkey], [ps_partkey], [ps_supplycost] FROM [dbo].[partsupp]`
- 1 exec, 100000 rows, 12.3 ms elapsed, 12.3 ms CPU, 45 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier]`
- 1 exec, 25 rows, 0.4 ms elapsed, 0.4 ms CPU, 11 reads: `SELECT [n_nationkey], [n_name] FROM [dbo].[nation]`

## q10: client 4.593 s, 4 statement(s), 16881365 rows to the client, server 4559 ms elapsed / 4752 ms CPU

- 1 exec, 14808183 rows, 2781.7 ms elapsed, 2781.4 ms CPU, 63935 reads: `SELECT [l_orderkey], [l_extendedprice], [l_discount] FROM [dbo].[lineitem] WHERE ([l_returnflag] = @p0)`
- 1 exec, 1500000 rows, 1556.2 ms elapsed, 1556.0 ms CPU, 17839 reads: `SELECT [c_custkey], [c_nationkey], [c_name], [c_acctbal], [c_phone], [c_address], [c_comment] FROM [dbo].[customer]`
- 1 exec, 573157 rows, 220.9 ms elapsed, 413.9 ms CPU, 33317 reads: `SELECT [o_custkey], [o_orderkey] FROM [dbo].[orders] WHERE (([o_orderdate] >= @p0) AND ([o_orderdate] < @p1))`
- 1 exec, 25 rows, 0.4 ms elapsed, 0.4 ms CPU, 11 reads: `SELECT [n_nationkey], [n_name] FROM [dbo].[nation]`

## q11: client 3.169 s, 6 statement(s), 16008100 rows to the client, server 3156 ms elapsed / 3156 ms CPU

- 1 exec, 8000000 rows, 1730.5 ms elapsed, 1730.1 ms CPU, 10265 reads: `SELECT [ps_suppkey], [ps_partkey], [ps_supplycost], [ps_availqty] FROM [dbo].[partsupp]`
- 1 exec, 8000000 rows, 1422.6 ms elapsed, 1422.4 ms CPU, 7594 reads: `SELECT [ps_suppkey], [ps_supplycost], [ps_availqty] FROM [dbo].[partsupp]`
- 1 exec, 4049 rows, 1.2 ms elapsed, 1.2 ms CPU, 45 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier] WHERE ([s_nationkey] = @p0)`
- 1 exec, 4049 rows, 1.2 ms elapsed, 1.2 ms CPU, 45 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier] WHERE ([s_nationkey] = @p0)`
- 1 exec, 1 rows, 0.5 ms elapsed, 0.5 ms CPU, 13 reads: `SELECT [n_nationkey] FROM [dbo].[nation] WHERE ([n_name] = @p0)`
- 1 exec, 1 rows, 0.5 ms elapsed, 0.5 ms CPU, 13 reads: `SELECT [n_nationkey] FROM [dbo].[nation] WHERE ([n_name] = @p0)`

## q12: client 2.27 s, 2 statement(s), 15310803 rows to the client, server 2255 ms elapsed / 2888 ms CPU

- 1 exec, 15000000 rows, 2155.4 ms elapsed, 2155.0 ms CPU, 7766 reads: `SELECT [o_orderkey], [o_orderpriority] FROM [dbo].[orders]`
- 1 exec, 310803 rows, 99.3 ms elapsed, 732.9 ms CPU, 100275 reads: `SELECT [l_orderkey], [l_shipmode] FROM [dbo].[lineitem] WHERE ([l_shipmode] IN (@p0, @p1)) AND ([l_commitdate] < [l_receiptdate]) AND ([l_shipdate] < [l_commitdate]) AND (([l_receiptdate] >= @p2) AND ([l_receiptdate] < @p3)) AND ([l_commitdate] < @p4) AND ([l_shipdate] < @p5)`

## q13: client 6.521 s, 1 statement(s), 1500000 rows to the client, server 6508 ms elapsed / 63447 ms CPU

- 1 exec, 1500000 rows, 6508.4 ms elapsed, 63447.0 ms CPU, 75419 reads: `SELECT [r1].[c_custkey] AS [c_custkey], COUNT_BIG([r2].[o_orderkey]) AS [count(o_orderkey)] FROM [dbo].[customer] AS [r1] LEFT JOIN [dbo].[orders] AS [r2] ON (([r1].[c_custkey] = [r2].[o_custkey]) AND (NOT ([r2].[o_comment] LIKE @p0))) GROUP BY [r1].[c_custkey]`

## q14: client 1.04 s, 2 statement(s), 2749223 rows to the client, server 1025 ms elapsed / 1470 ms CPU

- 1 exec, 2000000 rows, 561.9 ms elapsed, 561.9 ms CPU, 963 reads: `SELECT [p_partkey], [p_type] FROM [dbo].[part]`
- 1 exec, 749223 rows, 463.4 ms elapsed, 908.5 ms CPU, 99371 reads: `SELECT [l_partkey], [l_extendedprice], [l_discount] FROM [dbo].[lineitem] WHERE (([l_shipdate] >= @p0) AND ([l_shipdate] < @p1))`

## q15: client 1.267 s, 2 statement(s), 2365714 rows to the client, server 1353 ms elapsed / 2211 ms CPU

- 1 exec, 2265714 rows, 1255.1 ms elapsed, 2162.7 ms CPU, 99376 reads: `SELECT [l_suppkey], [l_extendedprice], [l_discount] FROM [dbo].[lineitem] WHERE (([l_shipdate] >= @p0) AND ([l_shipdate] < @p1))`
- 1 exec, 100000 rows, 97.8 ms elapsed, 48.5 ms CPU, 539 reads: `SELECT [s_suppkey], [s_name], [s_address], [s_phone] FROM [dbo].[supplier]`

## q16: client 1.193 s, 3 statement(s), 8396824 rows to the client, server 1220 ms elapsed / 1220 ms CPU

- 1 exec, 8000000 rows, 1041.0 ms elapsed, 1040.9 ms CPU, 5533 reads: `SELECT [ps_partkey], [ps_suppkey] FROM [dbo].[partsupp]`
- 1 exec, 296824 rows, 117.7 ms elapsed, 117.7 ms CPU, 1204 reads: `SELECT [p_partkey], [p_brand], [p_type], [p_size] FROM [dbo].[part] WHERE (NOT ([p_type] LIKE @p0)) AND ([p_size] IN (@p1, @p2, @p3, @p4, @p5, @p6, @p7, @p8)) AND ([p_brand] <> @p9)`
- 1 exec, 100000 rows, 61.4 ms elapsed, 61.4 ms CPU, 602 reads: `SELECT [s_comment], [s_suppkey] FROM [dbo].[supplier]`

## q17: client 19.322 s, 3 statement(s), 119974148 rows to the client, server 19311 ms elapsed / 19308 ms CPU

- 1 exec, 59986052 rows, 10909.2 ms elapsed, 10907.7 ms CPU, 56425 reads: `SELECT [l_partkey], [l_quantity], [l_extendedprice] FROM [dbo].[lineitem]`
- 1 exec, 59986052 rows, 8399.0 ms elapsed, 8398.1 ms CPU, 26946 reads: `SELECT [l_partkey], [l_quantity] FROM [dbo].[lineitem]`
- 1 exec, 2044 rows, 2.5 ms elapsed, 2.5 ms CPU, 777 reads: `SELECT [p_partkey] FROM [dbo].[part] WHERE ([p_brand] = @p0) AND ([p_container] = @p1)`

## q18: client 0.689 s, 1 statement(s), 100 rows to the client, server 650 ms elapsed / 4108 ms CPU

- 1 exec, 100 rows, 650.5 ms elapsed, 4108.0 ms CPU, 157626 reads: `SELECT TOP (100) [r1].[c_name] AS [c_name], [r1].[c_custkey] AS [c_custkey], [r2].[o_orderkey] AS [o_orderkey], [r2].[o_orderdate] AS [o_orderdate], [r2].[o_totalprice] AS [o_totalprice], SUM([r3].[l_quantity]) AS [sum(l_quantity)] FROM [dbo].[customer] AS [r1] CROSS JOIN [dbo].[orders] AS [r2] CROSS JOIN [dbo].[lineitem] AS [r3] WHERE (([r2].[o_orderkey] IN (SELECT [r4].[l_orderkey] AS [l_orderkey] FROM [dbo].[lineitem] AS [r4] GROUP BY [r4].[l_orderkey] HAVING (SUM([r4].[l_quantity]) > @p0))) AND ([r1].[c_custkey] = [r2].[o_custkey]) AND ([r2].[o_orderkey] = [r3].[l_orderkey])) GROUP BY [r1]`

## q19: client 0.811 s, 2 statement(s), 1289098 rows to the client, server 796 ms elapsed / 1627 ms CPU

- 1 exec, 1284344 rows, 787.0 ms elapsed, 1618.1 ms CPU, 93403 reads: `SELECT [l_partkey], [l_quantity], [l_extendedprice], [l_discount] FROM [dbo].[lineitem] WHERE ([l_shipmode] IN (@p0, @p1)) AND ((([l_quantity] >= @p2) AND ([l_quantity] <= @p3)) OR (([l_quantity] >= @p4) AND ([l_quantity] <= @p5)) OR (([l_quantity] >= @p6) AND ([l_quantity] <= @p7))) AND ([l_shipinstruct] = @p8)`
- 1 exec, 4754 rows, 9.1 ms elapsed, 9.1 ms CPU, 949 reads: `SELECT [p_partkey], [p_brand], [p_container], [p_size] FROM [dbo].[part] WHERE ((((([p_brand] = @p0) AND ([p_container] IN (@p1, @p2, @p3, @p4))) AND ([p_size] >= @p5)) AND ([p_size] <= @p6)) OR (((([p_brand] = @p7) AND ([p_container] IN (@p8, @p9, @p10, @p11))) AND ([p_size] >= @p12)) AND ([p_size] <= @p13)) OR (((([p_brand] = @p14) AND ([p_container] IN (@p15, @p16, @p17, @p18))) AND ([p_size] >= @p19)) AND ([p_size] <= @p20))) AND ((([p_brand] = @p21) OR ([p_brand] = @p22)) OR ([p_brand] = @p23)) AND ((([p_container] IN (@p24, @p25, @p26, @p27)) OR ([p_container] IN (@p28, @p29, @p30, @p31)`

## q20: client 6.886 s, 5 statement(s), 17220717 rows to the client, server 6960 ms elapsed / 10099 ms CPU

- 1 exec, 9099165 rows, 4841.8 ms elapsed, 8077.1 ms CPU, 59270 reads: `SELECT [l_partkey], [l_suppkey], [l_quantity] FROM [dbo].[lineitem] WHERE (([l_shipdate] >= @p0) AND ([l_shipdate] < @p1))`
- 1 exec, 8000000 rows, 1357.6 ms elapsed, 1357.3 ms CPU, 7594 reads: `SELECT [ps_partkey], [ps_availqty], [ps_suppkey] FROM [dbo].[partsupp]`
- 1 exec, 21551 rows, 666.1 ms elapsed, 616.9 ms CPU, 7934 reads: `SELECT [p_partkey] FROM [dbo].[part] WHERE ([p_name] LIKE @p0)`
- 1 exec, 100000 rows, 93.7 ms elapsed, 47.3 ms CPU, 458 reads: `SELECT [s_suppkey], [s_nationkey], [s_name], [s_address] FROM [dbo].[supplier]`
- 1 exec, 1 rows, 0.5 ms elapsed, 0.5 ms CPU, 13 reads: `SELECT [n_nationkey] FROM [dbo].[nation] WHERE ([n_name] = @p0)`

## q21: client 0.529 s, 1 statement(s), 100 rows to the client, server 457 ms elapsed / 2473 ms CPU

- 1 exec, 100 rows, 456.8 ms elapsed, 2472.7 ms CPU, 310820 reads: `SELECT TOP (100) [r1].[s_name] AS [s_name], COUNT_BIG(*) AS [numwait] FROM [dbo].[supplier] AS [r1] CROSS JOIN [dbo].[lineitem] AS [r2] CROSS JOIN [dbo].[orders] AS [r3] CROSS JOIN [dbo].[nation] AS [r4] WHERE (([r1].[s_suppkey] = [r2].[l_suppkey]) AND ([r3].[o_orderkey] = [r2].[l_orderkey]) AND ([r3].[o_orderstatus] = @p0) AND ([r2].[l_receiptdate] > [r2].[l_commitdate]) AND EXISTS (SELECT [r5].[l_orderkey] AS [l_orderkey], [r5].[l_partkey] AS [l_partkey], [r5].[l_suppkey] AS [l_suppkey], [r5].[l_linenumber] AS [l_linenumber], [r5].[l_quantity] AS [l_quantity], [r5].[l_extendedprice] AS [l_ex`

## q22: client 1.873 s, 3 statement(s), 17863614 rows to the client, server 1857 ms elapsed / 1857 ms CPU

- 1 exec, 15000000 rows, 1309.3 ms elapsed, 1309.0 ms CPU, 5220 reads: `SELECT [o_custkey] FROM [dbo].[orders]`
- 1 exec, 1500000 rows, 313.5 ms elapsed, 313.4 ms CPU, 2284 reads: `SELECT [c_phone], [c_acctbal], [c_custkey] FROM [dbo].[customer]`
- 1 exec, 1363614 rows, 234.4 ms elapsed, 234.4 ms CPU, 1787 reads: `SELECT [c_acctbal], [c_phone] FROM [dbo].[customer] WHERE ([c_acctbal] > @p0)`

