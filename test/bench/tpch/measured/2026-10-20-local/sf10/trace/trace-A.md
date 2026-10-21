# A: DuckDB 1.5.6 + mssql 0.2.5 -- tpch_sf10_columnstore

## q01: client 22.558 s, 1 statement(s), 59142609 rows to the client, server 22553 ms elapsed / 22495 ms CPU

- 1 exec, 59142609 rows, 22552.6 ms elapsed, 22495.0 ms CPU, 111746 reads: `SELECT CONVERT([nvarchar](1),[l_returnflag]) [l_returnflag],CONVERT([nvarchar](1),[l_linestatus]) [l_linestatus],[l_quantity],[l_extendedprice],[l_discount],[l_tax] FROM [dbo].[lineitem] WHERE [l_shipdate]<=@1`

## q02: client 2.636 s, 8 statement(s), 16207866 rows to the client, server 4196 ms elapsed / 4114 ms CPU

- 2 exec, 16000000 rows, 3779.8 ms elapsed, 3743.4 ms CPU, 35739 reads: `SELECT [ps_partkey], [ps_suppkey], [ps_supplycost] FROM [dbo].[partsupp]`
- 1 exec, 100000 rows, 274.0 ms elapsed, 273.6 ms CPU, 2612 reads: `SELECT [s_suppkey], [s_nationkey], [s_acctbal], CAST([s_name] AS NVARCHAR(25)) AS [s_name], CAST([s_address] AS NVARCHAR(40)) AS [s_address], CAST([s_phone] AS NVARCHAR(15)) AS [s_phone], CAST([s_comment] AS NVARCHAR(101)) AS [s_comment] FROM [dbo].[supplier]`
- 1 exec, 7854 rows, 95.0 ms elapsed, 77.9 ms CPU, 2582 reads: `SELECT [p_partkey], CAST([p_mfgr] AS NVARCHAR(25)) AS [p_mfgr] FROM [dbo].[part] WHERE ([p_type] LIKE N'%BRASS') AND ([p_size] = 15)`
- 1 exec, 100000 rows, 18.0 ms elapsed, 14.8 ms CPU, 121 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier]`
- 1 exec, 1 rows, 13.5 ms elapsed, 1.6 ms CPU, 41 reads: `SELECT [r_regionkey] FROM [dbo].[region] WHERE [r_name]=@1`
- 1 exec, 5 rows, 11.9 ms elapsed, 1.0 ms CPU, 43 reads: `SELECT [n_nationkey],[n_regionkey],CONVERT([nvarchar](25),[n_name]) [n_name] FROM [dbo].[nation] WHERE [n_regionkey]=@1`
- 1 exec, 5 rows, 1.9 ms elapsed, 1.3 ms CPU, 25 reads: `SELECT [n_nationkey],[n_regionkey] FROM [dbo].[nation] WHERE [n_regionkey]=@1`
- 1 exec, 1 rows, 1.6 ms elapsed, 0.9 ms CPU, 41 reads: `SELECT [r_regionkey] FROM [dbo].[region] WHERE [r_name]=@1`

## q03: client 8.209 s, 3 statement(s), 39923968 rows to the client, server 8293 ms elapsed / 8268 ms CPU

- 1 exec, 32334250 rows, 6507.1 ms elapsed, 6485.2 ms CPU, 156869 reads: `SELECT [l_orderkey],[l_extendedprice],[l_discount] FROM [dbo].[lineitem] WHERE [l_shipdate]>@1`
- 1 exec, 7289442 rows, 1651.4 ms elapsed, 1649.0 ms CPU, 34046 reads: `SELECT [o_custkey],[o_orderkey],[o_orderdate],[o_shippriority] FROM [dbo].[orders] WHERE [o_orderdate]<@1`
- 1 exec, 300276 rows, 134.2 ms elapsed, 134.0 ms CPU, 1287 reads: `SELECT [c_custkey] FROM [dbo].[customer] WHERE [c_mktsegment]=@1`

## q04: client 11.038 s, 2 statement(s), 38503019 rows to the client, server 10985 ms elapsed / 18475 ms CPU

- 1 exec, 37929348 rows, 10586.8 ms elapsed, 17694.7 ms CPU, 114875 reads: `SELECT [l_orderkey] FROM [dbo].[lineitem] WHERE ([l_commitdate] < [l_receiptdate])`
- 1 exec, 573671 rows, 398.0 ms elapsed, 779.9 ms CPU, 35884 reads: `SELECT [o_orderkey],CONVERT([nvarchar](15),[o_orderpriority]) [o_orderpriority] FROM [dbo].[orders] WHERE [o_orderdate]>=@1 AND [o_orderdate]<@2`

## q05: client 14.503 s, 6 statement(s), 63861977 rows to the client, server 15500 ms elapsed / 16243 ms CPU

- 1 exec, 59986052 rows, 13914.0 ms elapsed, 13864.7 ms CPU, 175390 reads: `SELECT [l_orderkey], [l_suppkey], [l_extendedprice], [l_discount] FROM [dbo].[lineitem]`
- 1 exec, 2275919 rows, 1365.9 ms elapsed, 2166.0 ms CPU, 51204 reads: `SELECT [o_custkey],[o_orderkey] FROM [dbo].[orders] WHERE [o_orderdate]>=@1 AND [o_orderdate]<@2`
- 1 exec, 1500000 rows, 203.4 ms elapsed, 197.2 ms CPU, 1276 reads: `SELECT [c_custkey], [c_nationkey] FROM [dbo].[customer]`
- 1 exec, 100000 rows, 14.0 ms elapsed, 13.3 ms CPU, 121 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier]`
- 1 exec, 5 rows, 1.2 ms elapsed, 1.0 ms CPU, 58 reads: `SELECT [n_nationkey],[n_regionkey],CONVERT([nvarchar](25),[n_name]) [n_name] FROM [dbo].[nation] WHERE [n_regionkey]=@1`
- 1 exec, 1 rows, 1.1 ms elapsed, 0.9 ms CPU, 41 reads: `SELECT [r_regionkey] FROM [dbo].[region] WHERE [r_name]=@1`

## q06: client 0.452 s, 1 statement(s), 1139264 rows to the client, server 448 ms elapsed / 1183 ms CPU

- 1 exec, 1139264 rows, 447.6 ms elapsed, 1182.6 ms CPU, 137647 reads: `SELECT [l_discount],[l_extendedprice] FROM [dbo].[lineitem] WHERE [l_shipdate]>=@1 AND [l_shipdate]<@2 AND [l_discount]>=@3 AND [l_discount]<=@4 AND [l_quantity]<@5`

## q07: client 7.947 s, 6 statement(s), 34830329 rows to the client, server 8076 ms elapsed / 8047 ms CPU

- 1 exec, 18230325 rows, 5817.4 ms elapsed, 5789.4 ms CPU, 202148 reads: `SELECT [l_suppkey],[l_orderkey],[l_shipdate],[l_extendedprice],[l_discount] FROM [dbo].[lineitem] WHERE [l_shipdate]>=@1 AND [l_shipdate]<=@2`
- 1 exec, 15000000 rows, 2046.5 ms elapsed, 2046.2 ms CPU, 26452 reads: `SELECT [o_orderkey], [o_custkey] FROM [dbo].[orders]`
- 1 exec, 1500000 rows, 193.8 ms elapsed, 193.6 ms CPU, 1276 reads: `SELECT [c_custkey], [c_nationkey] FROM [dbo].[customer]`
- 1 exec, 100000 rows, 14.2 ms elapsed, 13.8 ms CPU, 121 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier]`
- 1 exec, 2 rows, 3.7 ms elapsed, 3.4 ms CPU, 41 reads: `SELECT [n_nationkey], CAST([n_name] AS NVARCHAR(25)) AS [n_name] FROM [dbo].[nation] WHERE (([n_name] = N'FRANCE') OR ([n_name] = N'GERMANY'))`
- 1 exec, 2 rows, 0.7 ms elapsed, 0.7 ms CPU, 11 reads: `SELECT [n_nationkey], CAST([n_name] AS NVARCHAR(25)) AS [n_name] FROM [dbo].[nation] WHERE (([n_name] = N'GERMANY') OR ([n_name] = N'FRANCE'))`

## q08: client 19.274 s, 8 statement(s), 66157048 rows to the client, server 20005 ms elapsed / 19411 ms CPU

- 1 exec, 59986052 rows, 17598.1 ms elapsed, 17586.5 ms CPU, 217885 reads: `SELECT [l_partkey], [l_suppkey], [l_orderkey], [l_extendedprice], [l_discount] FROM [dbo].[lineitem]`
- 1 exec, 4557513 rows, 1412.9 ms elapsed, 1127.4 ms CPU, 33399 reads: `SELECT [o_orderkey],[o_custkey],[o_orderdate] FROM [dbo].[orders] WHERE [o_orderdate]>=@1 AND [o_orderdate]<=@2`
- 1 exec, 13452 rows, 780.1 ms elapsed, 488.3 ms CPU, 2064 reads: `SELECT [p_partkey] FROM [dbo].[part] WHERE [p_type]=@1`
- 1 exec, 1500000 rows, 192.8 ms elapsed, 192.4 ms CPU, 1276 reads: `SELECT [c_custkey], [c_nationkey] FROM [dbo].[customer]`
- 1 exec, 100000 rows, 13.7 ms elapsed, 13.6 ms CPU, 121 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier]`
- 1 exec, 5 rows, 5.2 ms elapsed, 1.1 ms CPU, 25 reads: `SELECT [n_nationkey],[n_regionkey] FROM [dbo].[nation] WHERE [n_regionkey]=@1`
- 1 exec, 1 rows, 1.2 ms elapsed, 1.1 ms CPU, 41 reads: `SELECT [r_regionkey] FROM [dbo].[region] WHERE [r_name]=@1`
- 1 exec, 25 rows, 0.9 ms elapsed, 0.8 ms CPU, 27 reads: `SELECT [n_nationkey], CAST([n_name] AS NVARCHAR(25)) AS [n_name] FROM [dbo].[nation]`

## q09: client 23.866 s, 6 statement(s), 83194859 rows to the client, server 28471 ms elapsed / 28408 ms CPU

- 1 exec, 59986052 rows, 20438.7 ms elapsed, 20428.8 ms CPU, 233289 reads: `SELECT [l_suppkey], [l_partkey], [l_orderkey], [l_extendedprice], [l_discount], [l_quantity] FROM [dbo].[lineitem]`
- 1 exec, 108782 rows, 3380.5 ms elapsed, 3379.6 ms CPU, 22772 reads: `SELECT [p_partkey] FROM [dbo].[part] WHERE ([p_name] LIKE N'%green%')`
- 1 exec, 15000000 rows, 3123.4 ms elapsed, 3092.7 ms CPU, 22707 reads: `SELECT [o_orderkey], [o_orderdate] FROM [dbo].[orders]`
- 1 exec, 8000000 rows, 1483.0 ms elapsed, 1482.8 ms CPU, 18476 reads: `SELECT [ps_suppkey], [ps_partkey], [ps_supplycost] FROM [dbo].[partsupp]`
- 1 exec, 100000 rows, 44.3 ms elapsed, 23.3 ms CPU, 121 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier]`
- 1 exec, 25 rows, 1.4 ms elapsed, 1.3 ms CPU, 41 reads: `SELECT [n_nationkey], CAST([n_name] AS NVARCHAR(25)) AS [n_name] FROM [dbo].[nation]`

## q10: client 11.444 s, 4 statement(s), 16881365 rows to the client, server 11423 ms elapsed / 11696 ms CPU

- 1 exec, 14808183 rows, 7114.8 ms elapsed, 7099.8 ms CPU, 136231 reads: `SELECT [l_orderkey],[l_extendedprice],[l_discount] FROM [dbo].[lineitem] WHERE [l_returnflag]=@1`
- 1 exec, 1500000 rows, 4083.9 ms elapsed, 4081.8 ms CPU, 49925 reads: `SELECT [c_custkey], [c_nationkey], CAST([c_name] AS NVARCHAR(25)) AS [c_name], [c_acctbal], CAST([c_phone] AS NVARCHAR(15)) AS [c_phone], CAST([c_address] AS NVARCHAR(40)) AS [c_address], CAST([c_comment] AS NVARCHAR(117)) AS [c_comment] FROM [dbo].[customer]`
- 1 exec, 573157 rows, 221.2 ms elapsed, 513.0 ms CPU, 51265 reads: `SELECT [o_custkey],[o_orderkey] FROM [dbo].[orders] WHERE [o_orderdate]>=@1 AND [o_orderdate]<@2`
- 1 exec, 25 rows, 3.4 ms elapsed, 1.0 ms CPU, 41 reads: `SELECT [n_nationkey], CAST([n_name] AS NVARCHAR(25)) AS [n_name] FROM [dbo].[nation]`

## q11: client 2.088 s, 3 statement(s), 8004050 rows to the client, server 2070 ms elapsed / 2069 ms CPU

- 1 exec, 8000000 rows, 2068.2 ms elapsed, 2067.5 ms CPU, 16586 reads: `SELECT [ps_suppkey], [ps_partkey], [ps_supplycost], [ps_availqty] FROM [dbo].[partsupp]`
- 1 exec, 4049 rows, 1.3 ms elapsed, 1.3 ms CPU, 45 reads: `SELECT [s_suppkey],[s_nationkey] FROM [dbo].[supplier] WHERE [s_nationkey]=@1`
- 1 exec, 1 rows, 0.6 ms elapsed, 0.6 ms CPU, 11 reads: `SELECT [n_nationkey] FROM [dbo].[nation] WHERE [n_name]=@1`

## q12: client 0.342 s, 1 statement(s), 1089346 rows to the client, server 329 ms elapsed / 1659 ms CPU

- 1 exec, 1089346 rows, 328.7 ms elapsed, 1659.2 ms CPU, 126635 reads: `SELECT [l_orderkey],CONVERT([nvarchar](10),[l_shipmode]) [l_shipmode] FROM [dbo].[lineitem] WHERE [l_commitdate]<[l_receiptdate] AND [l_shipdate]<[l_commitdate] AND [l_receiptdate]>=@1 AND [l_receiptdate]<@2 AND [l_commitdate]<@3 AND [l_shipdate]<@4`

## q13: client 10.844 s, 2 statement(s), 16500000 rows to the client, server 10817 ms elapsed / 10815 ms CPU

- 1 exec, 15000000 rows, 10681.8 ms elapsed, 10680.3 ms CPU, 96459 reads: `SELECT [o_custkey], CAST([o_comment] AS NVARCHAR(79)) AS [o_comment], [o_orderkey] FROM [dbo].[orders]`
- 1 exec, 1500000 rows, 135.0 ms elapsed, 134.9 ms CPU, 1144 reads: `SELECT [c_custkey] FROM [dbo].[customer]`

## q14: client 1.377 s, 2 statement(s), 2749223 rows to the client, server 1361 ms elapsed / 1930 ms CPU

- 1 exec, 2000000 rows, 977.0 ms elapsed, 976.8 ms CPU, 959 reads: `SELECT [p_partkey], CAST([p_type] AS NVARCHAR(25)) AS [p_type] FROM [dbo].[part]`
- 1 exec, 749223 rows, 384.4 ms elapsed, 952.9 ms CPU, 146037 reads: `SELECT [l_partkey],[l_extendedprice],[l_discount] FROM [dbo].[lineitem] WHERE [l_shipdate]>=@1 AND [l_shipdate]<@2`

## q15: client 1.381 s, 2 statement(s), 2265715 rows to the client, server 1362 ms elapsed / 2506 ms CPU

- 1 exec, 2265714 rows, 1361.5 ms elapsed, 2505.7 ms CPU, 149476 reads: `SELECT [l_suppkey],[l_extendedprice],[l_discount] FROM [dbo].[lineitem] WHERE [l_shipdate]>=@1 AND [l_shipdate]<@2`
- 1 exec, 1 rows, 0.7 ms elapsed, 0.7 ms CPU, 552 reads: `SELECT [s_suppkey],CONVERT([nvarchar](25),[s_name]) [s_name],CONVERT([nvarchar](40),[s_address]) [s_address],CONVERT([nvarchar](15),[s_phone]) [s_phone] FROM [dbo].[supplier] WHERE [s_suppkey]=@1`

## q16: client 2.519 s, 3 statement(s), 9955696 rows to the client, server 2641 ms elapsed / 2630 ms CPU

- 1 exec, 1855696 rows, 1445.9 ms elapsed, 1435.5 ms CPU, 1185 reads: `SELECT [p_partkey], CAST([p_brand] AS NVARCHAR(10)) AS [p_brand], CAST([p_type] AS NVARCHAR(25)) AS [p_type], [p_size] FROM [dbo].[part] WHERE (NOT ([p_type] LIKE N'MEDIUM POLISHED%')) AND ([p_brand] <> N'Brand#45')`
- 1 exec, 8000000 rows, 1029.5 ms elapsed, 1029.4 ms CPU, 5715 reads: `SELECT [ps_partkey], [ps_suppkey] FROM [dbo].[partsupp]`
- 1 exec, 100000 rows, 165.5 ms elapsed, 165.4 ms CPU, 1187 reads: `SELECT CAST([s_comment] AS NVARCHAR(101)) AS [s_comment], [s_suppkey] FROM [dbo].[supplier]`

## q17: client 19.367 s, 3 statement(s), 119974148 rows to the client, server 19563 ms elapsed / 19532 ms CPU

- 1 exec, 59986052 rows, 11036.8 ms elapsed, 11007.3 ms CPU, 87686 reads: `SELECT [l_partkey], [l_quantity], [l_extendedprice] FROM [dbo].[lineitem]`
- 1 exec, 59986052 rows, 8250.4 ms elapsed, 8248.7 ms CPU, 27816 reads: `SELECT [l_partkey], [l_quantity] FROM [dbo].[lineitem]`
- 1 exec, 2044 rows, 276.1 ms elapsed, 275.6 ms CPU, 893 reads: `SELECT [p_partkey] FROM [dbo].[part] WHERE [p_brand]=@1 AND [p_container]=@2`

## q18: client 20.87 s, 3 statement(s), 136472104 rows to the client, server 20817 ms elapsed / 20803 ms CPU

- 2 exec, 119972104 rows, 16750.4 ms elapsed, 16737.9 ms CPU, 102995 reads: `SELECT [l_orderkey], [l_quantity] FROM [dbo].[lineitem]`
- 1 exec, 15000000 rows, 3413.5 ms elapsed, 3412.8 ms CPU, 48990 reads: `SELECT [o_orderkey], [o_custkey], [o_orderdate], [o_totalprice] FROM [dbo].[orders]`
- 1 exec, 1500000 rows, 652.8 ms elapsed, 652.6 ms CPU, 3402 reads: `SELECT [c_custkey], CAST([c_name] AS NVARCHAR(25)) AS [c_name] FROM [dbo].[customer]`

## q19: client 13.187 s, 1 statement(s), 14994611 rows to the client, server 13176 ms elapsed / 13134 ms CPU

- 1 exec, 14994611 rows, 13176.0 ms elapsed, 13133.5 ms CPU, 106507 reads: `SELECT [l_partkey],[l_quantity],CONVERT([nvarchar](10),[l_shipmode]) [l_shipmode],[l_extendedprice],[l_discount] FROM [dbo].[lineitem] WHERE [l_shipinstruct]=@1`

## q20: client 7.38 s, 5 statement(s), 17220717 rows to the client, server 8154 ms elapsed / 11892 ms CPU

- 1 exec, 9099165 rows, 5438.9 ms elapsed, 9198.6 ms CPU, 111703 reads: `SELECT [l_partkey],[l_suppkey],[l_quantity] FROM [dbo].[lineitem] WHERE [l_shipdate]>=@1 AND [l_shipdate]<@2`
- 1 exec, 8000000 rows, 1808.7 ms elapsed, 1808.3 ms CPU, 17388 reads: `SELECT [ps_partkey], [ps_availqty], [ps_suppkey] FROM [dbo].[partsupp]`
- 1 exec, 21551 rows, 822.6 ms elapsed, 801.6 ms CPU, 22772 reads: `SELECT [p_partkey] FROM [dbo].[part] WHERE ([p_name] LIKE N'forest%')`
- 1 exec, 100000 rows, 83.0 ms elapsed, 82.8 ms CPU, 1134 reads: `SELECT [s_suppkey], [s_nationkey], CAST([s_name] AS NVARCHAR(25)) AS [s_name], CAST([s_address] AS NVARCHAR(40)) AS [s_address] FROM [dbo].[supplier]`
- 1 exec, 1 rows, 0.5 ms elapsed, 0.5 ms CPU, 11 reads: `SELECT [n_nationkey] FROM [dbo].[nation] WHERE [n_name]=@1`

## q21: client 25.291 s, 6 statement(s), 143253933 rows to the client, server 24644 ms elapsed / 24553 ms CPU

- 1 exec, 59986052 rows, 8140.5 ms elapsed, 8120.2 ms CPU, 101986 reads: `SELECT [l_orderkey], [l_suppkey] FROM [dbo].[lineitem]`
- 1 exec, 37929348 rows, 7373.5 ms elapsed, 7352.8 ms CPU, 152326 reads: `SELECT [l_suppkey], [l_orderkey] FROM [dbo].[lineitem] WHERE ([l_receiptdate] > [l_commitdate])`
- 1 exec, 37929348 rows, 7245.7 ms elapsed, 7200.1 ms CPU, 155806 reads: `SELECT [l_orderkey], [l_suppkey] FROM [dbo].[lineitem] WHERE ([l_receiptdate] > [l_commitdate])`
- 1 exec, 7309184 rows, 1842.9 ms elapsed, 1842.4 ms CPU, 16576 reads: `SELECT [o_orderkey] FROM [dbo].[orders] WHERE [o_orderstatus]=@1`
- 1 exec, 100000 rows, 36.4 ms elapsed, 36.2 ms CPU, 320 reads: `SELECT [s_suppkey], [s_nationkey], CAST([s_name] AS NVARCHAR(25)) AS [s_name] FROM [dbo].[supplier]`
- 1 exec, 1 rows, 4.6 ms elapsed, 1.6 ms CPU, 41 reads: `SELECT [n_nationkey] FROM [dbo].[nation] WHERE [n_name]=@1`

## q22: client 2.338 s, 3 statement(s), 17863614 rows to the client, server 2321 ms elapsed / 2321 ms CPU

- 1 exec, 15000000 rows, 1314.3 ms elapsed, 1314.1 ms CPU, 5224 reads: `SELECT [o_custkey] FROM [dbo].[orders]`
- 1 exec, 1363614 rows, 543.4 ms elapsed, 543.3 ms CPU, 1787 reads: `SELECT [c_acctbal],CONVERT([nvarchar](15),[c_phone]) [c_phone] FROM [dbo].[customer] WHERE [c_acctbal]>@1`
- 1 exec, 1500000 rows, 463.6 ms elapsed, 463.6 ms CPU, 2283 reads: `SELECT CAST([c_phone] AS NVARCHAR(15)) AS [c_phone], [c_acctbal], [c_custkey] FROM [dbo].[customer]`

