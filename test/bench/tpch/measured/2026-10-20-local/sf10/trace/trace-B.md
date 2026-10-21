# B: DuckDB 2.0 + mssql 0.3.0 (pushdown) -- tpch_sf10_columnstore

## q01: client 25.832 s, 1 statement(s), 59142609 rows to the client, server 25826 ms elapsed / 25796 ms CPU

- 1 exec, 59142609 rows, 25825.7 ms elapsed, 25796.3 ms CPU, 83071 reads: `SELECT CAST([l_returnflag] AS NCHAR(1)) AS [l_returnflag], CAST([l_linestatus] AS NCHAR(1)) AS [l_linestatus], [l_quantity], [l_extendedprice], [l_discount], [l_tax] FROM [dbo].[lineitem] WHERE ([l_shipdate] <= @p0)`

## q02: client 2.239 s, 5 statement(s), 8107860 rows to the client, server 2167 ms elapsed / 2146 ms CPU

- 1 exec, 8000000 rows, 1701.5 ms elapsed, 1680.6 ms CPU, 8198 reads: `SELECT [ps_partkey], [ps_suppkey], [ps_supplycost] FROM [dbo].[partsupp]`
- 1 exec, 100000 rows, 450.1 ms elapsed, 450.0 ms CPU, 1151 reads: `SELECT [s_suppkey], [s_nationkey], [s_acctbal], CAST([s_name] AS NCHAR(25)) AS [s_name], CAST([s_address] AS NVARCHAR(40)) AS [s_address], CAST([s_phone] AS NCHAR(15)) AS [s_phone], CAST([s_comment] AS NVARCHAR(101)) AS [s_comment] FROM [dbo].[supplier]`
- 1 exec, 7854 rows, 14.2 ms elapsed, 14.2 ms CPU, 1185 reads: `SELECT [p_partkey], CAST([p_mfgr] AS NCHAR(25)) AS [p_mfgr] FROM [dbo].[part] WHERE ([p_type] LIKE @p0) AND ([p_size] = @p1)`
- 1 exec, 5 rows, 0.7 ms elapsed, 0.7 ms CPU, 13 reads: `SELECT [n_nationkey], [n_regionkey], CAST([n_name] AS NCHAR(25)) AS [n_name] FROM [dbo].[nation] WHERE ([n_regionkey] = @p0)`
- 1 exec, 1 rows, 0.6 ms elapsed, 0.6 ms CPU, 13 reads: `SELECT [r_regionkey] FROM [dbo].[region] WHERE ([r_name] = @p0)`

## q03: client 9.602 s, 3 statement(s), 39923968 rows to the client, server 9579 ms elapsed / 9567 ms CPU

- 1 exec, 32334250 rows, 7862.2 ms elapsed, 7850.5 ms CPU, 160286 reads: `SELECT [l_orderkey], [l_extendedprice], [l_discount] FROM [dbo].[lineitem] WHERE ([l_shipdate] > @p0)`
- 1 exec, 7289442 rows, 1682.0 ms elapsed, 1681.8 ms CPU, 34159 reads: `SELECT [o_custkey], [o_orderkey], [o_orderdate], [o_shippriority] FROM [dbo].[orders] WHERE ([o_orderdate] < @p0)`
- 1 exec, 300276 rows, 34.5 ms elapsed, 34.3 ms CPU, 1368 reads: `SELECT [c_custkey] FROM [dbo].[customer] WHERE ([c_mktsegment] = @p0)`

## q04: client 9.502 s, 2 statement(s), 38503019 rows to the client, server 9486 ms elapsed / 16267 ms CPU

- 1 exec, 37929348 rows, 9188.3 ms elapsed, 15708.6 ms CPU, 106146 reads: `SELECT [l_orderkey] FROM [dbo].[lineitem] WHERE ([l_orderkey] IS NOT NULL) AND ([l_commitdate] < [l_receiptdate])`
- 1 exec, 573671 rows, 297.7 ms elapsed, 558.0 ms CPU, 32621 reads: `SELECT [o_orderkey], CAST([o_orderpriority] AS NCHAR(15)) AS [o_orderpriority] FROM [dbo].[orders] WHERE (([o_orderdate] >= @p0) AND ([o_orderdate] < @p1))`

## q05: client 14.84 s, 6 statement(s), 63861977 rows to the client, server 14840 ms elapsed / 15529 ms CPU

- 1 exec, 59986052 rows, 13782.9 ms elapsed, 13774.7 ms CPU, 175634 reads: `SELECT [l_orderkey], [l_suppkey], [l_extendedprice], [l_discount] FROM [dbo].[lineitem]`
- 1 exec, 2275919 rows, 830.9 ms elapsed, 1535.2 ms CPU, 51230 reads: `SELECT [o_custkey], [o_orderkey] FROM [dbo].[orders] WHERE (([o_orderdate] >= @p0) AND ([o_orderdate] < @p1))`
- 1 exec, 1500000 rows, 197.6 ms elapsed, 196.6 ms CPU, 1276 reads: `SELECT [c_custkey], [c_nationkey] FROM [dbo].[customer]`
- 1 exec, 100000 rows, 19.5 ms elapsed, 19.5 ms CPU, 121 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier]`
- 1 exec, 1 rows, 7.4 ms elapsed, 1.4 ms CPU, 51 reads: `SELECT [r_regionkey] FROM [dbo].[region] WHERE ([r_name] = @p0)`
- 1 exec, 5 rows, 2.0 ms elapsed, 1.6 ms CPU, 58 reads: `SELECT [n_nationkey], [n_regionkey], CAST([n_name] AS NCHAR(25)) AS [n_name] FROM [dbo].[nation] WHERE ([n_regionkey] = @p0)`

## q06: client 0.379 s, 1 statement(s), 1139264 rows to the client, server 372 ms elapsed / 1140 ms CPU

- 1 exec, 1139264 rows, 372.0 ms elapsed, 1140.0 ms CPU, 127570 reads: `SELECT [l_discount], [l_extendedprice] FROM [dbo].[lineitem] WHERE (([l_shipdate] >= @p0) AND ([l_shipdate] < @p1)) AND ([l_discount] BETWEEN @p2 AND @p3) AND ([l_quantity] < @p4)`

## q07: client 10.435 s, 6 statement(s), 34830329 rows to the client, server 10400 ms elapsed / 9458 ms CPU

- 1 exec, 18230325 rows, 7676.4 ms elapsed, 6816.0 ms CPU, 202532 reads: `SELECT [l_suppkey], [l_orderkey], [l_shipdate], [l_extendedprice], [l_discount] FROM [dbo].[lineitem] WHERE ([l_shipdate] BETWEEN @p0 AND @p1)`
- 1 exec, 15000000 rows, 2476.0 ms elapsed, 2426.8 ms CPU, 26452 reads: `SELECT [o_orderkey], [o_custkey] FROM [dbo].[orders]`
- 1 exec, 1500000 rows, 207.6 ms elapsed, 199.4 ms CPU, 1276 reads: `SELECT [c_custkey], [c_nationkey] FROM [dbo].[customer]`
- 1 exec, 100000 rows, 20.7 ms elapsed, 14.3 ms CPU, 121 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier]`
- 1 exec, 2 rows, 11.4 ms elapsed, 0.8 ms CPU, 15 reads: `SELECT [n_nationkey], CAST([n_name] AS NCHAR(25)) AS [n_name] FROM [dbo].[nation] WHERE (([n_name] = @p0) OR ([n_name] = @p1))`
- 1 exec, 2 rows, 8.0 ms elapsed, 1.2 ms CPU, 41 reads: `SELECT [n_nationkey], CAST([n_name] AS NCHAR(25)) AS [n_name] FROM [dbo].[nation] WHERE (([n_name] = @p0) OR ([n_name] = @p1))`

## q08: client 23.204 s, 8 statement(s), 66157048 rows to the client, server 23211 ms elapsed / 23194 ms CPU

- 1 exec, 59986052 rows, 22050.0 ms elapsed, 22038.1 ms CPU, 217845 reads: `SELECT [l_partkey], [l_suppkey], [l_orderkey], [l_extendedprice], [l_discount] FROM [dbo].[lineitem]`
- 1 exec, 4557513 rows, 921.3 ms elapsed, 921.2 ms CPU, 33399 reads: `SELECT [o_orderkey], [o_custkey], [o_orderdate] FROM [dbo].[orders] WHERE ([o_orderdate] BETWEEN @p0 AND @p1)`
- 1 exec, 1500000 rows, 197.1 ms elapsed, 197.0 ms CPU, 1276 reads: `SELECT [c_custkey], [c_nationkey] FROM [dbo].[customer]`
- 1 exec, 13452 rows, 21.7 ms elapsed, 21.3 ms CPU, 2376 reads: `SELECT [p_partkey] FROM [dbo].[part] WHERE ([p_type] = @p0)`
- 1 exec, 100000 rows, 14.1 ms elapsed, 14.1 ms CPU, 121 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier]`
- 1 exec, 25 rows, 4.9 ms elapsed, 0.8 ms CPU, 15 reads: `SELECT [n_nationkey], CAST([n_name] AS NCHAR(25)) AS [n_name] FROM [dbo].[nation]`
- 1 exec, 1 rows, 1.5 ms elapsed, 1.2 ms CPU, 51 reads: `SELECT [r_regionkey] FROM [dbo].[region] WHERE ([r_name] = @p0)`
- 1 exec, 5 rows, 0.6 ms elapsed, 0.6 ms CPU, 41 reads: `SELECT [n_nationkey], [n_regionkey] FROM [dbo].[nation] WHERE ([n_regionkey] = @p0)`

## q09: client 24.872 s, 6 statement(s), 83194859 rows to the client, server 24846 ms elapsed / 24795 ms CPU

- 1 exec, 59986052 rows, 20382.4 ms elapsed, 20368.7 ms CPU, 233161 reads: `SELECT [l_suppkey], [l_partkey], [l_orderkey], [l_extendedprice], [l_discount], [l_quantity] FROM [dbo].[lineitem]`
- 1 exec, 15000000 rows, 2113.4 ms elapsed, 2113.2 ms CPU, 22707 reads: `SELECT [o_orderkey], [o_orderdate] FROM [dbo].[orders]`
- 1 exec, 8000000 rows, 1480.4 ms elapsed, 1480.2 ms CPU, 18476 reads: `SELECT [ps_suppkey], [ps_partkey], [ps_supplycost] FROM [dbo].[partsupp]`
- 1 exec, 108782 rows, 834.6 ms elapsed, 814.5 ms CPU, 22772 reads: `SELECT [p_partkey] FROM [dbo].[part] WHERE ([p_name] LIKE @p0)`
- 1 exec, 100000 rows, 29.9 ms elapsed, 17.4 ms CPU, 121 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier]`
- 1 exec, 25 rows, 5.4 ms elapsed, 1.2 ms CPU, 26 reads: `SELECT [n_nationkey], CAST([n_name] AS NCHAR(25)) AS [n_name] FROM [dbo].[nation]`

## q10: client 6.29 s, 4 statement(s), 16881365 rows to the client, server 6253 ms elapsed / 6564 ms CPU

- 1 exec, 14808183 rows, 3202.3 ms elapsed, 3184.5 ms CPU, 137463 reads: `SELECT [l_orderkey], [l_extendedprice], [l_discount] FROM [dbo].[lineitem] WHERE ([l_returnflag] = @p0)`
- 1 exec, 1500000 rows, 2790.8 ms elapsed, 2789.2 ms CPU, 49925 reads: `SELECT [c_custkey], [c_nationkey], CAST([c_name] AS NVARCHAR(25)) AS [c_name], [c_acctbal], CAST([c_phone] AS NCHAR(15)) AS [c_phone], CAST([c_address] AS NVARCHAR(40)) AS [c_address], CAST([c_comment] AS NVARCHAR(117)) AS [c_comment] FROM [dbo].[customer]`
- 1 exec, 573157 rows, 259.0 ms elapsed, 588.7 ms CPU, 51028 reads: `SELECT [o_custkey], [o_orderkey] FROM [dbo].[orders] WHERE (([o_orderdate] >= @p0) AND ([o_orderdate] < @p1))`
- 1 exec, 25 rows, 1.3 ms elapsed, 1.1 ms CPU, 42 reads: `SELECT [n_nationkey], CAST([n_name] AS NCHAR(25)) AS [n_name] FROM [dbo].[nation]`

## q11: client 3.649 s, 6 statement(s), 16008100 rows to the client, server 3637 ms elapsed / 3636 ms CPU

- 1 exec, 8000000 rows, 1916.2 ms elapsed, 1915.8 ms CPU, 7591 reads: `SELECT [ps_suppkey], [ps_supplycost], [ps_availqty] FROM [dbo].[partsupp]`
- 1 exec, 8000000 rows, 1716.7 ms elapsed, 1716.5 ms CPU, 10260 reads: `SELECT [ps_suppkey], [ps_partkey], [ps_supplycost], [ps_availqty] FROM [dbo].[partsupp]`
- 1 exec, 4049 rows, 1.4 ms elapsed, 1.4 ms CPU, 45 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier] WHERE ([s_nationkey] = @p0)`
- 1 exec, 4049 rows, 1.1 ms elapsed, 1.1 ms CPU, 45 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier] WHERE ([s_nationkey] = @p0)`
- 1 exec, 1 rows, 0.9 ms elapsed, 0.9 ms CPU, 13 reads: `SELECT [n_nationkey] FROM [dbo].[nation] WHERE ([n_name] = @p0)`
- 1 exec, 1 rows, 0.6 ms elapsed, 0.6 ms CPU, 13 reads: `SELECT [n_nationkey] FROM [dbo].[nation] WHERE ([n_name] = @p0)`

## q12: client 4.778 s, 2 statement(s), 15310803 rows to the client, server 4763 ms elapsed / 5556 ms CPU

- 1 exec, 15000000 rows, 4649.1 ms elapsed, 4647.8 ms CPU, 7772 reads: `SELECT [o_orderkey], CAST([o_orderpriority] AS NCHAR(15)) AS [o_orderpriority] FROM [dbo].[orders]`
- 1 exec, 310803 rows, 113.5 ms elapsed, 908.7 ms CPU, 135429 reads: `SELECT [l_orderkey], CAST([l_shipmode] AS NCHAR(10)) AS [l_shipmode] FROM [dbo].[lineitem] WHERE ([l_shipmode] IN (@p0, @p1)) AND ([l_commitdate] < [l_receiptdate]) AND ([l_shipdate] < [l_commitdate]) AND (([l_receiptdate] >= @p2) AND ([l_receiptdate] < @p3)) AND ([l_commitdate] < @p4) AND ([l_shipdate] < @p5)`

## q13: client 2.28 s, 1 statement(s), 1500000 rows to the client, server 2261 ms elapsed / 18384 ms CPU

- 1 exec, 1500000 rows, 2261.0 ms elapsed, 18383.6 ms CPU, 166350 reads: `SELECT [r1].[c_custkey] AS [c_custkey], COUNT_BIG([r2].[o_orderkey]) AS [count(o_orderkey)] FROM [dbo].[customer] AS [r1] LEFT JOIN [dbo].[orders] AS [r2] ON (([r1].[c_custkey] = [r2].[o_custkey]) AND (NOT ([r2].[o_comment] LIKE @p0))) GROUP BY [r1].[c_custkey]`

## q14: client 1.148 s, 2 statement(s), 2749223 rows to the client, server 1132 ms elapsed / 1822 ms CPU

- 1 exec, 2000000 rows, 736.8 ms elapsed, 736.7 ms CPU, 2064 reads: `SELECT [p_partkey], CAST([p_type] AS NVARCHAR(25)) AS [p_type] FROM [dbo].[part]`
- 1 exec, 749223 rows, 395.7 ms elapsed, 1085.1 ms CPU, 154016 reads: `SELECT [l_partkey], [l_extendedprice], [l_discount] FROM [dbo].[lineitem] WHERE (([l_shipdate] >= @p0) AND ([l_shipdate] < @p1))`

## q15: client 1.16 s, 2 statement(s), 2365714 rows to the client, server 1281 ms elapsed / 2437 ms CPU

- 1 exec, 2265714 rows, 1153.0 ms elapsed, 2326.1 ms CPU, 158123 reads: `SELECT [l_suppkey], [l_extendedprice], [l_discount] FROM [dbo].[lineitem] WHERE (([l_shipdate] >= @p0) AND ([l_shipdate] < @p1))`
- 1 exec, 100000 rows, 128.0 ms elapsed, 111.1 ms CPU, 552 reads: `SELECT [s_suppkey], CAST([s_name] AS NCHAR(25)) AS [s_name], CAST([s_address] AS NVARCHAR(40)) AS [s_address], CAST([s_phone] AS NCHAR(15)) AS [s_phone] FROM [dbo].[supplier]`

## q16: client 1.353 s, 3 statement(s), 8396824 rows to the client, server 1400 ms elapsed / 1400 ms CPU

- 1 exec, 8000000 rows, 1064.1 ms elapsed, 1063.9 ms CPU, 5529 reads: `SELECT [ps_partkey], [ps_suppkey] FROM [dbo].[partsupp]`
- 1 exec, 296824 rows, 250.8 ms elapsed, 250.8 ms CPU, 1201 reads: `SELECT [p_partkey], CAST([p_brand] AS NCHAR(10)) AS [p_brand], CAST([p_type] AS NVARCHAR(25)) AS [p_type], [p_size] FROM [dbo].[part] WHERE (NOT ([p_type] LIKE @p0)) AND ([p_size] IN (@p1, @p2, @p3, @p4, @p5, @p6, @p7, @p8)) AND ([p_brand] <> @p9)`
- 1 exec, 100000 rows, 85.1 ms elapsed, 85.1 ms CPU, 602 reads: `SELECT CAST([s_comment] AS NVARCHAR(101)) AS [s_comment], [s_suppkey] FROM [dbo].[supplier]`

## q17: client 21.702 s, 3 statement(s), 119974148 rows to the client, server 21687 ms elapsed / 21678 ms CPU

- 1 exec, 59986052 rows, 11149.7 ms elapsed, 11145.0 ms CPU, 82106 reads: `SELECT [l_partkey], [l_quantity], [l_extendedprice] FROM [dbo].[lineitem]`
- 1 exec, 59986052 rows, 10530.1 ms elapsed, 10526.6 ms CPU, 30894 reads: `SELECT [l_partkey], [l_quantity] FROM [dbo].[lineitem]`
- 1 exec, 2044 rows, 7.0 ms elapsed, 6.9 ms CPU, 1790 reads: `SELECT [p_partkey] FROM [dbo].[part] WHERE ([p_brand] = @p0) AND ([p_container] = @p1)`

## q18: client 0.825 s, 1 statement(s), 100 rows to the client, server 786 ms elapsed / 6065 ms CPU

- 1 exec, 100 rows, 785.5 ms elapsed, 6064.7 ms CPU, 252328 reads: `SELECT TOP (100) CAST([r1].[c_name] AS NVARCHAR(25)) AS [c_name], [r1].[c_custkey] AS [c_custkey], [r2].[o_orderkey] AS [o_orderkey], [r2].[o_orderdate] AS [o_orderdate], [r2].[o_totalprice] AS [o_totalprice], SUM([r3].[l_quantity]) AS [sum(l_quantity)] FROM [dbo].[customer] AS [r1] CROSS JOIN [dbo].[orders] AS [r2] CROSS JOIN [dbo].[lineitem] AS [r3] WHERE (([r2].[o_orderkey] IN (SELECT [r4].[l_orderkey] AS [l_orderkey] FROM [dbo].[lineitem] AS [r4] GROUP BY [r4].[l_orderkey] HAVING (SUM([r4].[l_quantity]) > @p0))) AND ([r1].[c_custkey] = [r2].[o_custkey]) AND ([r2].[o_orderkey] = [r3].[l_ord`

## q19: client 1.066 s, 2 statement(s), 1289098 rows to the client, server 1048 ms elapsed / 2392 ms CPU

- 1 exec, 1284344 rows, 1024.9 ms elapsed, 2369.4 ms CPU, 187325 reads: `SELECT [l_partkey], [l_quantity], [l_extendedprice], [l_discount] FROM [dbo].[lineitem] WHERE ([l_shipmode] IN (@p0, @p1)) AND ((([l_quantity] >= @p2) AND ([l_quantity] <= @p3)) OR (([l_quantity] >= @p4) AND ([l_quantity] <= @p5)) OR (([l_quantity] >= @p6) AND ([l_quantity] <= @p7))) AND ([l_shipinstruct] = @p8)`
- 1 exec, 4754 rows, 22.9 ms elapsed, 22.2 ms CPU, 2186 reads: `SELECT [p_partkey], CAST([p_brand] AS NCHAR(10)) AS [p_brand], CAST([p_container] AS NCHAR(10)) AS [p_container], [p_size] FROM [dbo].[part] WHERE ((((([p_brand] = @p0) AND ([p_container] IN (@p1, @p2, @p3, @p4))) AND ([p_size] >= @p5)) AND ([p_size] <= @p6)) OR (((([p_brand] = @p7) AND ([p_container] IN (@p8, @p9, @p10, @p11))) AND ([p_size] >= @p12)) AND ([p_size] <= @p13)) OR (((([p_brand] = @p14) AND ([p_container] IN (@p15, @p16, @p17, @p18))) AND ([p_size] >= @p19)) AND ([p_size] <= @p20))) AND ((([p_brand] = @p21) OR ([p_brand] = @p22)) OR ([p_brand] = @p23)) AND ((([p_container] IN (@p`

## q20: client 7.268 s, 5 statement(s), 17220717 rows to the client, server 7338 ms elapsed / 11252 ms CPU

- 1 exec, 9099165 rows, 4911.7 ms elapsed, 8827.1 ms CPU, 127569 reads: `SELECT [l_partkey], [l_suppkey], [l_quantity] FROM [dbo].[lineitem] WHERE (([l_shipdate] >= @p0) AND ([l_shipdate] < @p1))`
- 1 exec, 8000000 rows, 1834.1 ms elapsed, 1833.6 ms CPU, 17388 reads: `SELECT [ps_partkey], [ps_availqty], [ps_suppkey] FROM [dbo].[partsupp]`
- 1 exec, 21551 rows, 504.8 ms elapsed, 504.7 ms CPU, 22772 reads: `SELECT [p_partkey] FROM [dbo].[part] WHERE ([p_name] LIKE @p0)`
- 1 exec, 100000 rows, 85.7 ms elapsed, 85.3 ms CPU, 1134 reads: `SELECT [s_suppkey], [s_nationkey], CAST([s_name] AS NCHAR(25)) AS [s_name], CAST([s_address] AS NVARCHAR(40)) AS [s_address] FROM [dbo].[supplier]`
- 1 exec, 1 rows, 1.3 ms elapsed, 1.1 ms CPU, 51 reads: `SELECT [n_nationkey] FROM [dbo].[nation] WHERE ([n_name] = @p0)`

## q21: client 20.717 s, 6 statement(s), 143157943 rows to the client, server 20564 ms elapsed / 20538 ms CPU

- 1 exec, 59986052 rows, 8129.6 ms elapsed, 8127.9 ms CPU, 102754 reads: `SELECT [l_orderkey], [l_suppkey] FROM [dbo].[lineitem] WHERE ([l_orderkey] IS NOT NULL) AND ([l_suppkey] IS NOT NULL)`
- 1 exec, 37929348 rows, 5875.1 ms elapsed, 5864.9 ms CPU, 157152 reads: `SELECT [l_suppkey], [l_orderkey] FROM [dbo].[lineitem] WHERE ([l_receiptdate] > [l_commitdate])`
- 1 exec, 37929348 rows, 5858.2 ms elapsed, 5844.1 ms CPU, 157168 reads: `SELECT [l_orderkey], [l_suppkey] FROM [dbo].[lineitem] WHERE ([l_orderkey] IS NOT NULL) AND ([l_suppkey] IS NOT NULL) AND ([l_receiptdate] > [l_commitdate])`
- 1 exec, 7309184 rows, 694.5 ms elapsed, 694.4 ms CPU, 17086 reads: `SELECT [o_orderkey] FROM [dbo].[orders] WHERE ([o_orderstatus] = @p0)`
- 1 exec, 4010 rows, 6.0 ms elapsed, 5.9 ms CPU, 320 reads: `SELECT [s_suppkey], [s_nationkey], CAST([s_name] AS NCHAR(25)) AS [s_name] FROM [dbo].[supplier] WHERE ([s_nationkey] = @p0)`
- 1 exec, 1 rows, 1.1 ms elapsed, 1.0 ms CPU, 51 reads: `SELECT [n_nationkey] FROM [dbo].[nation] WHERE ([n_name] = @p0)`

## q22: client 2.366 s, 3 statement(s), 17863614 rows to the client, server 2348 ms elapsed / 2348 ms CPU

- 1 exec, 15000000 rows, 1342.7 ms elapsed, 1342.6 ms CPU, 10764 reads: `SELECT [o_custkey] FROM [dbo].[orders]`
- 1 exec, 1500000 rows, 548.8 ms elapsed, 548.7 ms CPU, 4647 reads: `SELECT CAST([c_phone] AS NCHAR(15)) AS [c_phone], [c_acctbal], [c_custkey] FROM [dbo].[customer]`
- 1 exec, 1363614 rows, 456.4 ms elapsed, 456.4 ms CPU, 4598 reads: `SELECT [c_acctbal], CAST([c_phone] AS NCHAR(15)) AS [c_phone] FROM [dbo].[customer] WHERE ([c_acctbal] > @p0)`

