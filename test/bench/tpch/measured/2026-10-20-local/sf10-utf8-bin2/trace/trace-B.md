# B: DuckDB 2.0 + mssql 0.3.0 (pushdown) -- tpch_sf10_columnstore_utf8bin2

## q01: client 19.758 s, 1 statement(s), 59142609 rows to the client, server 19753 ms elapsed / 19747 ms CPU

- 1 exec, 59142609 rows, 19752.8 ms elapsed, 19747.4 ms CPU, 53873 reads: `SELECT [l_returnflag], [l_linestatus], [l_quantity], [l_extendedprice], [l_discount], [l_tax] FROM [dbo].[lineitem] WHERE ([l_shipdate] <= @p0)`

## q02: client 0.156 s, 1 statement(s), 100 rows to the client, server 96 ms elapsed / 96 ms CPU

- 1 exec, 100 rows, 95.7 ms elapsed, 95.7 ms CPU, 18829 reads: `SELECT TOP (100) [r2].[s_acctbal] AS [s_acctbal], [r2].[s_name] AS [s_name], [r4].[n_name] AS [n_name], [r1].[p_partkey] AS [p_partkey], [r1].[p_mfgr] AS [p_mfgr], [r2].[s_address] AS [s_address], [r2].[s_phone] AS [s_phone], [r2].[s_comment] AS [s_comment] FROM [dbo].[part] AS [r1] CROSS JOIN [dbo].[supplier] AS [r2] CROSS JOIN [dbo].[partsupp] AS [r3] CROSS JOIN [dbo].[nation] AS [r4] CROSS JOIN [dbo].[region] AS [r5] WHERE (([r1].[p_partkey] = [r3].[ps_partkey]) AND ([r2].[s_suppkey] = [r3].[ps_suppkey]) AND ([r1].[p_size] = @p0) AND ([r1].[p_type] LIKE @p1) AND ([r2].[s_nationkey] = [r4].[`

## q03: client 8.399 s, 3 statement(s), 39923968 rows to the client, server 8376 ms elapsed / 8375 ms CPU

- 1 exec, 32334250 rows, 6815.2 ms elapsed, 6813.9 ms CPU, 75349 reads: `SELECT [l_orderkey], [l_extendedprice], [l_discount] FROM [dbo].[lineitem] WHERE ([l_shipdate] > @p0)`
- 1 exec, 7289442 rows, 1532.1 ms elapsed, 1532.0 ms CPU, 15739 reads: `SELECT [o_custkey], [o_orderkey], [o_orderdate], [o_shippriority] FROM [dbo].[orders] WHERE ([o_orderdate] < @p0)`
- 1 exec, 300276 rows, 29.1 ms elapsed, 29.1 ms CPU, 569 reads: `SELECT [c_custkey] FROM [dbo].[customer] WHERE ([c_mktsegment] = @p0)`

## q04: client 10.325 s, 2 statement(s), 38503019 rows to the client, server 10309 ms elapsed / 17404 ms CPU

- 1 exec, 37929348 rows, 10043.4 ms elapsed, 16942.1 ms CPU, 54607 reads: `SELECT [l_orderkey] FROM [dbo].[lineitem] WHERE ([l_orderkey] IS NOT NULL) AND ([l_commitdate] < [l_receiptdate])`
- 1 exec, 573671 rows, 265.2 ms elapsed, 462.2 ms CPU, 23111 reads: `SELECT [o_orderkey], [o_orderpriority] FROM [dbo].[orders] WHERE (([o_orderdate] >= @p0) AND ([o_orderdate] < @p1))`

## q05: client 14.196 s, 6 statement(s), 63861977 rows to the client, server 14190 ms elapsed / 14797 ms CPU

- 1 exec, 59986052 rows, 13003.5 ms elapsed, 13001.4 ms CPU, 83027 reads: `SELECT [l_orderkey], [l_suppkey], [l_extendedprice], [l_discount] FROM [dbo].[lineitem]`
- 1 exec, 2275919 rows, 910.4 ms elapsed, 1519.7 ms CPU, 33320 reads: `SELECT [o_custkey], [o_orderkey] FROM [dbo].[orders] WHERE (([o_orderdate] >= @p0) AND ([o_orderdate] < @p1))`
- 1 exec, 1500000 rows, 261.1 ms elapsed, 261.1 ms CPU, 551 reads: `SELECT [c_custkey], [c_nationkey] FROM [dbo].[customer]`
- 1 exec, 100000 rows, 13.5 ms elapsed, 13.5 ms CPU, 45 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier]`
- 1 exec, 5 rows, 0.6 ms elapsed, 0.6 ms CPU, 13 reads: `SELECT [n_nationkey], [n_regionkey], [n_name] FROM [dbo].[nation] WHERE ([n_regionkey] = @p0)`
- 1 exec, 1 rows, 0.4 ms elapsed, 0.4 ms CPU, 13 reads: `SELECT [r_regionkey] FROM [dbo].[region] WHERE ([r_name] = @p0)`

## q06: client 0.536 s, 1 statement(s), 1139264 rows to the client, server 531 ms elapsed / 1029 ms CPU

- 1 exec, 1139264 rows, 531.1 ms elapsed, 1029.3 ms CPU, 79186 reads: `SELECT [l_discount], [l_extendedprice] FROM [dbo].[lineitem] WHERE (([l_shipdate] >= @p0) AND ([l_shipdate] < @p1)) AND ([l_discount] BETWEEN @p2 AND @p3) AND ([l_quantity] < @p4)`

## q07: client 7.259 s, 6 statement(s), 34830329 rows to the client, server 7251 ms elapsed / 7250 ms CPU

- 1 exec, 18230325 rows, 5112.4 ms elapsed, 5111.8 ms CPU, 95080 reads: `SELECT [l_suppkey], [l_orderkey], [l_shipdate], [l_extendedprice], [l_discount] FROM [dbo].[lineitem] WHERE ([l_shipdate] BETWEEN @p0 AND @p1)`
- 1 exec, 15000000 rows, 1909.8 ms elapsed, 1909.4 ms CPU, 12594 reads: `SELECT [o_orderkey], [o_custkey] FROM [dbo].[orders]`
- 1 exec, 1500000 rows, 214.2 ms elapsed, 214.2 ms CPU, 551 reads: `SELECT [c_custkey], [c_nationkey] FROM [dbo].[customer]`
- 1 exec, 100000 rows, 13.1 ms elapsed, 13.1 ms CPU, 45 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier]`
- 1 exec, 2 rows, 0.7 ms elapsed, 0.7 ms CPU, 11 reads: `SELECT [n_nationkey], [n_name] FROM [dbo].[nation] WHERE (([n_name] = @p0) OR ([n_name] = @p1))`
- 1 exec, 2 rows, 0.7 ms elapsed, 0.7 ms CPU, 11 reads: `SELECT [n_nationkey], [n_name] FROM [dbo].[nation] WHERE (([n_name] = @p0) OR ([n_name] = @p1))`

## q08: client 17.548 s, 8 statement(s), 66157048 rows to the client, server 17536 ms elapsed / 17532 ms CPU

- 1 exec, 59986052 rows, 16469.9 ms elapsed, 16468.1 ms CPU, 102758 reads: `SELECT [l_partkey], [l_suppkey], [l_orderkey], [l_extendedprice], [l_discount] FROM [dbo].[lineitem]`
- 1 exec, 4557513 rows, 834.5 ms elapsed, 834.2 ms CPU, 15639 reads: `SELECT [o_orderkey], [o_custkey], [o_orderdate] FROM [dbo].[orders] WHERE ([o_orderdate] BETWEEN @p0 AND @p1)`
- 1 exec, 1500000 rows, 207.2 ms elapsed, 207.1 ms CPU, 551 reads: `SELECT [c_custkey], [c_nationkey] FROM [dbo].[customer]`
- 1 exec, 100000 rows, 12.8 ms elapsed, 12.8 ms CPU, 45 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier]`
- 1 exec, 13452 rows, 10.4 ms elapsed, 8.2 ms CPU, 1222 reads: `SELECT [p_partkey] FROM [dbo].[part] WHERE ([p_type] = @p0)`
- 1 exec, 5 rows, 0.5 ms elapsed, 0.5 ms CPU, 11 reads: `SELECT [n_nationkey], [n_regionkey] FROM [dbo].[nation] WHERE ([n_regionkey] = @p0)`
- 1 exec, 25 rows, 0.4 ms elapsed, 0.4 ms CPU, 11 reads: `SELECT [n_nationkey], [n_name] FROM [dbo].[nation]`
- 1 exec, 1 rows, 0.4 ms elapsed, 0.4 ms CPU, 13 reads: `SELECT [r_regionkey] FROM [dbo].[region] WHERE ([r_name] = @p0)`

## q09: client 24.721 s, 6 statement(s), 83194859 rows to the client, server 24684 ms elapsed / 24675 ms CPU

- 1 exec, 59986052 rows, 19924.0 ms elapsed, 19920.2 ms CPU, 108916 reads: `SELECT [l_suppkey], [l_partkey], [l_orderkey], [l_extendedprice], [l_discount], [l_quantity] FROM [dbo].[lineitem]`
- 1 exec, 15000000 rows, 2307.3 ms elapsed, 2306.8 ms CPU, 10712 reads: `SELECT [o_orderkey], [o_orderdate] FROM [dbo].[orders]`
- 1 exec, 8000000 rows, 1391.4 ms elapsed, 1391.3 ms CPU, 8204 reads: `SELECT [ps_suppkey], [ps_partkey], [ps_supplycost] FROM [dbo].[partsupp]`
- 1 exec, 108782 rows, 1047.8 ms elapsed, 1043.1 ms CPU, 7931 reads: `SELECT [p_partkey] FROM [dbo].[part] WHERE ([p_name] LIKE @p0)`
- 1 exec, 100000 rows, 12.8 ms elapsed, 12.8 ms CPU, 45 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier]`
- 1 exec, 25 rows, 0.4 ms elapsed, 0.4 ms CPU, 11 reads: `SELECT [n_nationkey], [n_name] FROM [dbo].[nation]`

## q10: client 4.531 s, 4 statement(s), 16881365 rows to the client, server 4500 ms elapsed / 4688 ms CPU

- 1 exec, 14808183 rows, 2727.8 ms elapsed, 2727.5 ms CPU, 64008 reads: `SELECT [l_orderkey], [l_extendedprice], [l_discount] FROM [dbo].[lineitem] WHERE ([l_returnflag] = @p0)`
- 1 exec, 1500000 rows, 1533.7 ms elapsed, 1533.5 ms CPU, 17844 reads: `SELECT [c_custkey], [c_nationkey], [c_name], [c_acctbal], [c_phone], [c_address], [c_comment] FROM [dbo].[customer]`
- 1 exec, 573157 rows, 237.4 ms elapsed, 425.9 ms CPU, 33320 reads: `SELECT [o_custkey], [o_orderkey] FROM [dbo].[orders] WHERE (([o_orderdate] >= @p0) AND ([o_orderdate] < @p1))`
- 1 exec, 25 rows, 0.6 ms elapsed, 0.6 ms CPU, 11 reads: `SELECT [n_nationkey], [n_name] FROM [dbo].[nation]`

## q11: client 3.087 s, 5 statement(s), 16008100 rows to the client, server 3075 ms elapsed / 3074 ms CPU

- 1 exec, 8000000 rows, 1689.1 ms elapsed, 1688.8 ms CPU, 10265 reads: `SELECT [ps_suppkey], [ps_partkey], [ps_supplycost], [ps_availqty] FROM [dbo].[partsupp]`
- 1 exec, 8000000 rows, 1382.9 ms elapsed, 1382.8 ms CPU, 7594 reads: `SELECT [ps_suppkey], [ps_supplycost], [ps_availqty] FROM [dbo].[partsupp]`
- 2 exec, 8098 rows, 1.9 ms elapsed, 1.9 ms CPU, 90 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier] WHERE ([s_nationkey] = @p0)`
- 1 exec, 1 rows, 0.5 ms elapsed, 0.5 ms CPU, 13 reads: `SELECT [n_nationkey] FROM [dbo].[nation] WHERE ([n_name] = @p0)`
- 1 exec, 1 rows, 0.5 ms elapsed, 0.5 ms CPU, 13 reads: `SELECT [n_nationkey] FROM [dbo].[nation] WHERE ([n_name] = @p0)`

## q12: client 2.2 s, 2 statement(s), 15310803 rows to the client, server 2186 ms elapsed / 2613 ms CPU

- 1 exec, 15000000 rows, 2106.5 ms elapsed, 2105.9 ms CPU, 7767 reads: `SELECT [o_orderkey], [o_orderpriority] FROM [dbo].[orders]`
- 1 exec, 310803 rows, 79.0 ms elapsed, 507.3 ms CPU, 100365 reads: `SELECT [l_orderkey], [l_shipmode] FROM [dbo].[lineitem] WHERE ([l_shipmode] IN (@p0, @p1)) AND ([l_commitdate] < [l_receiptdate]) AND ([l_shipdate] < [l_commitdate]) AND (([l_receiptdate] >= @p2) AND ([l_receiptdate] < @p3)) AND ([l_commitdate] < @p4) AND ([l_shipdate] < @p5)`

## q13: client 2.448 s, 1 statement(s), 1500000 rows to the client, server 2436 ms elapsed / 19993 ms CPU

- 1 exec, 1500000 rows, 2435.7 ms elapsed, 19992.6 ms CPU, 75426 reads: `SELECT [r1].[c_custkey] AS [c_custkey], COUNT_BIG([r2].[o_orderkey]) AS [count(o_orderkey)] FROM [dbo].[customer] AS [r1] LEFT JOIN [dbo].[orders] AS [r2] ON (([r1].[c_custkey] = [r2].[o_custkey]) AND (NOT ([r2].[o_comment] LIKE @p0))) GROUP BY [r1].[c_custkey]`

## q14: client 1.118 s, 2 statement(s), 2749223 rows to the client, server 1101 ms elapsed / 1483 ms CPU

- 1 exec, 2000000 rows, 713.6 ms elapsed, 713.4 ms CPU, 962 reads: `SELECT [p_partkey], [p_type] FROM [dbo].[part]`
- 1 exec, 749223 rows, 387.4 ms elapsed, 769.6 ms CPU, 99391 reads: `SELECT [l_partkey], [l_extendedprice], [l_discount] FROM [dbo].[lineitem] WHERE (([l_shipdate] >= @p0) AND ([l_shipdate] < @p1))`

## q15: client 1.261 s, 2 statement(s), 2365714 rows to the client, server 1305 ms elapsed / 2272 ms CPU

- 1 exec, 2265714 rows, 1253.3 ms elapsed, 2221.4 ms CPU, 99386 reads: `SELECT [l_suppkey], [l_extendedprice], [l_discount] FROM [dbo].[lineitem] WHERE (([l_shipdate] >= @p0) AND ([l_shipdate] < @p1))`
- 1 exec, 100000 rows, 52.1 ms elapsed, 51.0 ms CPU, 539 reads: `SELECT [s_suppkey], [s_name], [s_address], [s_phone] FROM [dbo].[supplier]`

## q16: client 1.231 s, 3 statement(s), 8396824 rows to the client, server 1259 ms elapsed / 1259 ms CPU

- 1 exec, 8000000 rows, 1075.3 ms elapsed, 1075.1 ms CPU, 5533 reads: `SELECT [ps_partkey], [ps_suppkey] FROM [dbo].[partsupp]`
- 1 exec, 296824 rows, 121.9 ms elapsed, 121.9 ms CPU, 1204 reads: `SELECT [p_partkey], [p_brand], [p_type], [p_size] FROM [dbo].[part] WHERE (NOT ([p_type] LIKE @p0)) AND ([p_size] IN (@p1, @p2, @p3, @p4, @p5, @p6, @p7, @p8)) AND ([p_brand] <> @p9)`
- 1 exec, 100000 rows, 61.8 ms elapsed, 61.8 ms CPU, 602 reads: `SELECT [s_comment], [s_suppkey] FROM [dbo].[supplier]`

## q17: client 20.447 s, 3 statement(s), 119974148 rows to the client, server 20435 ms elapsed / 20429 ms CPU

- 1 exec, 59986052 rows, 12281.8 ms elapsed, 12276.6 ms CPU, 56429 reads: `SELECT [l_partkey], [l_quantity], [l_extendedprice] FROM [dbo].[lineitem]`
- 1 exec, 59986052 rows, 8151.5 ms elapsed, 8150.1 ms CPU, 26949 reads: `SELECT [l_partkey], [l_quantity] FROM [dbo].[lineitem]`
- 1 exec, 2044 rows, 1.9 ms elapsed, 1.9 ms CPU, 776 reads: `SELECT [p_partkey] FROM [dbo].[part] WHERE ([p_brand] = @p0) AND ([p_container] = @p1)`

## q18: client 0.545 s, 1 statement(s), 100 rows to the client, server 508 ms elapsed / 4404 ms CPU

- 1 exec, 100 rows, 507.9 ms elapsed, 4404.1 ms CPU, 141730 reads: `SELECT TOP (100) [r1].[c_name] AS [c_name], [r1].[c_custkey] AS [c_custkey], [r2].[o_orderkey] AS [o_orderkey], [r2].[o_orderdate] AS [o_orderdate], [r2].[o_totalprice] AS [o_totalprice], SUM([r3].[l_quantity]) AS [sum(l_quantity)] FROM [dbo].[customer] AS [r1] CROSS JOIN [dbo].[orders] AS [r2] CROSS JOIN [dbo].[lineitem] AS [r3] WHERE (([r2].[o_orderkey] IN (SELECT [r4].[l_orderkey] AS [l_orderkey] FROM [dbo].[lineitem] AS [r4] GROUP BY [r4].[l_orderkey] HAVING (SUM([r4].[l_quantity]) > @p0))) AND ([r1].[c_custkey] = [r2].[o_custkey]) AND ([r2].[o_orderkey] = [r3].[l_orderkey])) GROUP BY [r1]`

## q19: client 0.928 s, 2 statement(s), 1289098 rows to the client, server 907 ms elapsed / 1747 ms CPU

- 1 exec, 1284344 rows, 901.7 ms elapsed, 1741.1 ms CPU, 93456 reads: `SELECT [l_partkey], [l_quantity], [l_extendedprice], [l_discount] FROM [dbo].[lineitem] WHERE ([l_shipmode] IN (@p0, @p1)) AND ((([l_quantity] >= @p2) AND ([l_quantity] <= @p3)) OR (([l_quantity] >= @p4) AND ([l_quantity] <= @p5)) OR (([l_quantity] >= @p6) AND ([l_quantity] <= @p7))) AND ([l_shipinstruct] = @p8)`
- 1 exec, 4754 rows, 5.5 ms elapsed, 5.5 ms CPU, 949 reads: `SELECT [p_partkey], [p_brand], [p_container], [p_size] FROM [dbo].[part] WHERE ((((([p_brand] = @p0) AND ([p_container] IN (@p1, @p2, @p3, @p4))) AND ([p_size] >= @p5)) AND ([p_size] <= @p6)) OR (((([p_brand] = @p7) AND ([p_container] IN (@p8, @p9, @p10, @p11))) AND ([p_size] >= @p12)) AND ([p_size] <= @p13)) OR (((([p_brand] = @p14) AND ([p_container] IN (@p15, @p16, @p17, @p18))) AND ([p_size] >= @p19)) AND ([p_size] <= @p20))) AND ((([p_brand] = @p21) OR ([p_brand] = @p22)) OR ([p_brand] = @p23)) AND ((([p_container] IN (@p24, @p25, @p26, @p27)) OR ([p_container] IN (@p28, @p29, @p30, @p31)`

## q20: client 7.356 s, 5 statement(s), 17220717 rows to the client, server 7386 ms elapsed / 10818 ms CPU

- 1 exec, 9099165 rows, 5523.8 ms elapsed, 8956.4 ms CPU, 59274 reads: `SELECT [l_partkey], [l_suppkey], [l_quantity] FROM [dbo].[lineitem] WHERE (([l_shipdate] >= @p0) AND ([l_shipdate] < @p1))`
- 1 exec, 8000000 rows, 1354.9 ms elapsed, 1354.7 ms CPU, 7594 reads: `SELECT [ps_partkey], [ps_availqty], [ps_suppkey] FROM [dbo].[partsupp]`
- 1 exec, 21551 rows, 460.3 ms elapsed, 460.3 ms CPU, 7931 reads: `SELECT [p_partkey] FROM [dbo].[part] WHERE ([p_name] LIKE @p0)`
- 1 exec, 100000 rows, 46.1 ms elapsed, 46.1 ms CPU, 458 reads: `SELECT [s_suppkey], [s_nationkey], [s_name], [s_address] FROM [dbo].[supplier]`
- 1 exec, 1 rows, 0.6 ms elapsed, 0.6 ms CPU, 13 reads: `SELECT [n_nationkey] FROM [dbo].[nation] WHERE ([n_name] = @p0)`

## q21: client 0.467 s, 1 statement(s), 100 rows to the client, server 386 ms elapsed / 2292 ms CPU

- 1 exec, 100 rows, 386.0 ms elapsed, 2291.5 ms CPU, 311128 reads: `SELECT TOP (100) [r1].[s_name] AS [s_name], COUNT_BIG(*) AS [numwait] FROM [dbo].[supplier] AS [r1] CROSS JOIN [dbo].[lineitem] AS [r2] CROSS JOIN [dbo].[orders] AS [r3] CROSS JOIN [dbo].[nation] AS [r4] WHERE (([r1].[s_suppkey] = [r2].[l_suppkey]) AND ([r3].[o_orderkey] = [r2].[l_orderkey]) AND ([r3].[o_orderstatus] = @p0) AND ([r2].[l_receiptdate] > [r2].[l_commitdate]) AND EXISTS (SELECT [r5].[l_orderkey] AS [l_orderkey], [r5].[l_partkey] AS [l_partkey], [r5].[l_suppkey] AS [l_suppkey], [r5].[l_linenumber] AS [l_linenumber], [r5].[l_quantity] AS [l_quantity], [r5].[l_extendedprice] AS [l_ex`

## q22: client 1.892 s, 3 statement(s), 17863614 rows to the client, server 1875 ms elapsed / 1875 ms CPU

- 1 exec, 15000000 rows, 1318.9 ms elapsed, 1318.7 ms CPU, 5222 reads: `SELECT [o_custkey] FROM [dbo].[orders]`
- 1 exec, 1500000 rows, 319.2 ms elapsed, 319.2 ms CPU, 2284 reads: `SELECT [c_phone], [c_acctbal], [c_custkey] FROM [dbo].[customer]`
- 1 exec, 1363614 rows, 237.1 ms elapsed, 237.0 ms CPU, 1787 reads: `SELECT [c_acctbal], [c_phone] FROM [dbo].[customer] WHERE ([c_acctbal] > @p0)`

