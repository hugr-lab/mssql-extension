# A: DuckDB 1.5.6 + mssql 0.2.5 -- tpch_sf10_columnstore_utf8

## q01: client 19.795 s, 1 statement(s), 59142609 rows to the client, server 19790 ms elapsed / 19788 ms CPU

- 1 exec, 59142609 rows, 19789.9 ms elapsed, 19787.8 ms CPU, 53847 reads: `SELECT [l_returnflag],[l_linestatus],[l_quantity],[l_extendedprice],[l_discount],[l_tax] FROM [dbo].[lineitem] WHERE [l_shipdate]<=@1`

## q02: client 1.582 s, 8 statement(s), 16207866 rows to the client, server 3017 ms elapsed / 3016 ms CPU

- 2 exec, 16000000 rows, 2827.6 ms elapsed, 2826.4 ms CPU, 16408 reads: `SELECT [ps_partkey], [ps_suppkey], [ps_supplycost] FROM [dbo].[partsupp]`
- 1 exec, 100000 rows, 112.2 ms elapsed, 112.2 ms CPU, 1138 reads: `SELECT [s_suppkey], [s_nationkey], [s_acctbal], [s_name], [s_address], [s_phone], [s_comment] FROM [dbo].[supplier]`
- 1 exec, 7854 rows, 62.4 ms elapsed, 62.4 ms CPU, 1188 reads: `SELECT [p_partkey], [p_mfgr] FROM [dbo].[part] WHERE ([p_type] LIKE N'%BRASS') AND ([p_size] = 15)`
- 1 exec, 100000 rows, 12.9 ms elapsed, 12.9 ms CPU, 45 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier]`
- 1 exec, 5 rows, 0.4 ms elapsed, 0.4 ms CPU, 13 reads: `SELECT [n_nationkey],[n_regionkey],[n_name] FROM [dbo].[nation] WHERE [n_regionkey]=@1`
- 1 exec, 5 rows, 0.4 ms elapsed, 0.4 ms CPU, 11 reads: `SELECT [n_nationkey],[n_regionkey] FROM [dbo].[nation] WHERE [n_regionkey]=@1`
- 1 exec, 1 rows, 0.4 ms elapsed, 0.4 ms CPU, 11 reads: `SELECT [r_regionkey] FROM [dbo].[region] WHERE [r_name]=@1`
- 1 exec, 1 rows, 0.4 ms elapsed, 0.4 ms CPU, 11 reads: `SELECT [r_regionkey] FROM [dbo].[region] WHERE [r_name]=@1`

## q03: client 7.547 s, 3 statement(s), 39923968 rows to the client, server 7670 ms elapsed / 7667 ms CPU

- 1 exec, 32334250 rows, 5965.9 ms elapsed, 5962.7 ms CPU, 75279 reads: `SELECT [l_orderkey],[l_extendedprice],[l_discount] FROM [dbo].[lineitem] WHERE [l_shipdate]>@1`
- 1 exec, 7289442 rows, 1537.7 ms elapsed, 1537.5 ms CPU, 15736 reads: `SELECT [o_custkey],[o_orderkey],[o_orderdate],[o_shippriority] FROM [dbo].[orders] WHERE [o_orderdate]<@1`
- 1 exec, 300276 rows, 166.7 ms elapsed, 166.6 ms CPU, 551 reads: `SELECT [c_custkey] FROM [dbo].[customer] WHERE [c_mktsegment]=@1`

## q04: client 10.449 s, 2 statement(s), 38503019 rows to the client, server 10396 ms elapsed / 17368 ms CPU

- 1 exec, 37929348 rows, 10137.6 ms elapsed, 16912.8 ms CPU, 54540 reads: `SELECT [l_orderkey] FROM [dbo].[lineitem] WHERE ([l_commitdate] < [l_receiptdate])`
- 1 exec, 573671 rows, 258.9 ms elapsed, 455.7 ms CPU, 23112 reads: `SELECT [o_orderkey],[o_orderpriority] FROM [dbo].[orders] WHERE [o_orderdate]>=@1 AND [o_orderdate]<@2`

## q05: client 13.718 s, 6 statement(s), 63861977 rows to the client, server 14679 ms elapsed / 15316 ms CPU

- 1 exec, 59986052 rows, 13140.0 ms elapsed, 13120.2 ms CPU, 82963 reads: `SELECT [l_orderkey], [l_suppkey], [l_extendedprice], [l_discount] FROM [dbo].[lineitem]`
- 1 exec, 2275919 rows, 1319.3 ms elapsed, 1983.6 ms CPU, 33305 reads: `SELECT [o_custkey],[o_orderkey] FROM [dbo].[orders] WHERE [o_orderdate]>=@1 AND [o_orderdate]<@2`
- 1 exec, 1500000 rows, 205.1 ms elapsed, 198.8 ms CPU, 551 reads: `SELECT [c_custkey], [c_nationkey] FROM [dbo].[customer]`
- 1 exec, 100000 rows, 13.3 ms elapsed, 12.8 ms CPU, 45 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier]`
- 1 exec, 1 rows, 0.5 ms elapsed, 0.5 ms CPU, 11 reads: `SELECT [r_regionkey] FROM [dbo].[region] WHERE [r_name]=@1`
- 1 exec, 5 rows, 0.5 ms elapsed, 0.5 ms CPU, 13 reads: `SELECT [n_nationkey],[n_regionkey],[n_name] FROM [dbo].[nation] WHERE [n_regionkey]=@1`

## q06: client 0.543 s, 1 statement(s), 1139264 rows to the client, server 538 ms elapsed / 1037 ms CPU

- 1 exec, 1139264 rows, 537.5 ms elapsed, 1036.9 ms CPU, 79165 reads: `SELECT [l_discount],[l_extendedprice] FROM [dbo].[lineitem] WHERE [l_shipdate]>=@1 AND [l_shipdate]<@2 AND [l_discount]>=@3 AND [l_discount]<=@4 AND [l_quantity]<@5`

## q07: client 7.115 s, 6 statement(s), 34830329 rows to the client, server 7323 ms elapsed / 7319 ms CPU

- 1 exec, 18230325 rows, 5112.6 ms elapsed, 5112.1 ms CPU, 95011 reads: `SELECT [l_suppkey],[l_orderkey],[l_shipdate],[l_extendedprice],[l_discount] FROM [dbo].[lineitem] WHERE [l_shipdate]>=@1 AND [l_shipdate]<=@2`
- 1 exec, 15000000 rows, 1925.6 ms elapsed, 1921.4 ms CPU, 12591 reads: `SELECT [o_orderkey], [o_custkey] FROM [dbo].[orders]`
- 1 exec, 1500000 rows, 271.0 ms elapsed, 271.0 ms CPU, 551 reads: `SELECT [c_custkey], [c_nationkey] FROM [dbo].[customer]`
- 1 exec, 100000 rows, 13.1 ms elapsed, 13.1 ms CPU, 45 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier]`
- 1 exec, 2 rows, 0.5 ms elapsed, 0.5 ms CPU, 11 reads: `SELECT [n_nationkey], [n_name] FROM [dbo].[nation] WHERE (([n_name] = N'GERMANY') OR ([n_name] = N'FRANCE'))`
- 1 exec, 2 rows, 0.5 ms elapsed, 0.5 ms CPU, 11 reads: `SELECT [n_nationkey], [n_name] FROM [dbo].[nation] WHERE (([n_name] = N'FRANCE') OR ([n_name] = N'GERMANY'))`

## q08: client 17.849 s, 8 statement(s), 66157048 rows to the client, server 18580 ms elapsed / 18040 ms CPU

- 1 exec, 59986052 rows, 16488.4 ms elapsed, 16486.5 ms CPU, 102695 reads: `SELECT [l_partkey], [l_suppkey], [l_orderkey], [l_extendedprice], [l_discount] FROM [dbo].[lineitem]`
- 1 exec, 4557513 rows, 1109.1 ms elapsed, 841.5 ms CPU, 15636 reads: `SELECT [o_orderkey],[o_custkey],[o_orderdate] FROM [dbo].[orders] WHERE [o_orderdate]>=@1 AND [o_orderdate]<=@2`
- 1 exec, 13452 rows, 753.9 ms elapsed, 486.2 ms CPU, 963 reads: `SELECT [p_partkey] FROM [dbo].[part] WHERE [p_type]=@1`
- 1 exec, 1500000 rows, 211.7 ms elapsed, 211.7 ms CPU, 551 reads: `SELECT [c_custkey], [c_nationkey] FROM [dbo].[customer]`
- 1 exec, 100000 rows, 15.5 ms elapsed, 13.0 ms CPU, 45 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier]`
- 1 exec, 25 rows, 0.6 ms elapsed, 0.6 ms CPU, 11 reads: `SELECT [n_nationkey], [n_name] FROM [dbo].[nation]`
- 1 exec, 5 rows, 0.3 ms elapsed, 0.3 ms CPU, 11 reads: `SELECT [n_nationkey],[n_regionkey] FROM [dbo].[nation] WHERE [n_regionkey]=@1`
- 1 exec, 1 rows, 0.3 ms elapsed, 0.3 ms CPU, 11 reads: `SELECT [r_regionkey] FROM [dbo].[region] WHERE [r_name]=@1`

## q09: client 23.213 s, 6 statement(s), 83194859 rows to the client, server 27129 ms elapsed / 27124 ms CPU

- 1 exec, 59986052 rows, 19379.0 ms elapsed, 19376.5 ms CPU, 108850 reads: `SELECT [l_suppkey], [l_partkey], [l_orderkey], [l_extendedprice], [l_discount], [l_quantity] FROM [dbo].[lineitem]`
- 1 exec, 108782 rows, 3791.8 ms elapsed, 3791.3 ms CPU, 7934 reads: `SELECT [p_partkey] FROM [dbo].[part] WHERE ([p_name] LIKE N'%green%')`
- 1 exec, 15000000 rows, 2029.2 ms elapsed, 2028.1 ms CPU, 10711 reads: `SELECT [o_orderkey], [o_orderdate] FROM [dbo].[orders]`
- 1 exec, 8000000 rows, 1914.7 ms elapsed, 1914.3 ms CPU, 8204 reads: `SELECT [ps_suppkey], [ps_partkey], [ps_supplycost] FROM [dbo].[partsupp]`
- 1 exec, 100000 rows, 13.5 ms elapsed, 13.5 ms CPU, 45 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier]`
- 1 exec, 25 rows, 0.5 ms elapsed, 0.5 ms CPU, 11 reads: `SELECT [n_nationkey], [n_name] FROM [dbo].[nation]`

## q10: client 10.293 s, 4 statement(s), 16881365 rows to the client, server 10268 ms elapsed / 10433 ms CPU

- 1 exec, 14808183 rows, 8537.0 ms elapsed, 8535.5 ms CPU, 63583 reads: `SELECT [l_orderkey],[l_extendedprice],[l_discount] FROM [dbo].[lineitem] WHERE [l_returnflag]=@1`
- 1 exec, 1500000 rows, 1526.2 ms elapsed, 1525.4 ms CPU, 17839 reads: `SELECT [c_custkey], [c_nationkey], [c_name], [c_acctbal], [c_phone], [c_address], [c_comment] FROM [dbo].[customer]`
- 1 exec, 573157 rows, 204.8 ms elapsed, 371.5 ms CPU, 33314 reads: `SELECT [o_custkey],[o_orderkey] FROM [dbo].[orders] WHERE [o_orderdate]>=@1 AND [o_orderdate]<@2`
- 1 exec, 25 rows, 0.4 ms elapsed, 0.4 ms CPU, 11 reads: `SELECT [n_nationkey], [n_name] FROM [dbo].[nation]`

## q11: client 1.687 s, 3 statement(s), 8004050 rows to the client, server 1673 ms elapsed / 1672 ms CPU

- 1 exec, 8000000 rows, 1671.0 ms elapsed, 1670.8 ms CPU, 10265 reads: `SELECT [ps_suppkey], [ps_partkey], [ps_supplycost], [ps_availqty] FROM [dbo].[partsupp]`
- 1 exec, 4049 rows, 1.1 ms elapsed, 1.1 ms CPU, 45 reads: `SELECT [s_suppkey],[s_nationkey] FROM [dbo].[supplier] WHERE [s_nationkey]=@1`
- 1 exec, 1 rows, 0.5 ms elapsed, 0.5 ms CPU, 11 reads: `SELECT [n_nationkey] FROM [dbo].[nation] WHERE [n_name]=@1`

## q12: client 2.559 s, 2 statement(s), 16089346 rows to the client, server 2549 ms elapsed / 3538 ms CPU

- 1 exec, 15000000 rows, 2114.7 ms elapsed, 2113.3 ms CPU, 7766 reads: `SELECT [o_orderkey], [o_orderpriority] FROM [dbo].[orders]`
- 1 exec, 1089346 rows, 434.1 ms elapsed, 1424.4 ms CPU, 100242 reads: `SELECT [l_orderkey],[l_shipmode] FROM [dbo].[lineitem] WHERE [l_commitdate]<[l_receiptdate] AND [l_shipdate]<[l_commitdate] AND [l_receiptdate]>=@1 AND [l_receiptdate]<@2 AND [l_commitdate]<@3 AND [l_shipdate]<@4`

## q13: client 9.322 s, 2 statement(s), 16500000 rows to the client, server 9297 ms elapsed / 9295 ms CPU

- 1 exec, 15000000 rows, 9169.4 ms elapsed, 9167.3 ms CPU, 81457 reads: `SELECT [o_custkey], [o_comment], [o_orderkey] FROM [dbo].[orders]`
- 1 exec, 1500000 rows, 127.5 ms elapsed, 127.5 ms CPU, 533 reads: `SELECT [c_custkey] FROM [dbo].[customer]`

## q14: client 1.023 s, 2 statement(s), 2749223 rows to the client, server 1008 ms elapsed / 1441 ms CPU

- 1 exec, 2000000 rows, 545.8 ms elapsed, 545.7 ms CPU, 963 reads: `SELECT [p_partkey], [p_type] FROM [dbo].[part]`
- 1 exec, 749223 rows, 462.6 ms elapsed, 895.1 ms CPU, 99376 reads: `SELECT [l_partkey],[l_extendedprice],[l_discount] FROM [dbo].[lineitem] WHERE [l_shipdate]>=@1 AND [l_shipdate]<@2`

## q15: client 1.348 s, 2 statement(s), 2265715 rows to the client, server 1331 ms elapsed / 2318 ms CPU

- 1 exec, 2265714 rows, 1330.2 ms elapsed, 2317.3 ms CPU, 99361 reads: `SELECT [l_suppkey],[l_extendedprice],[l_discount] FROM [dbo].[lineitem] WHERE [l_shipdate]>=@1 AND [l_shipdate]<@2`
- 1 exec, 1 rows, 0.6 ms elapsed, 0.6 ms CPU, 539 reads: `SELECT [s_suppkey],[s_name],[s_address],[s_phone] FROM [dbo].[supplier] WHERE [s_suppkey]=@1`

## q16: client 2.382 s, 3 statement(s), 9955696 rows to the client, server 2401 ms elapsed / 2400 ms CPU

- 1 exec, 1855696 rows, 1273.7 ms elapsed, 1273.6 ms CPU, 1188 reads: `SELECT [p_partkey], [p_brand], [p_type], [p_size] FROM [dbo].[part] WHERE (NOT ([p_type] LIKE N'MEDIUM POLISHED%')) AND ([p_brand] <> N'Brand#45')`
- 1 exec, 8000000 rows, 1064.9 ms elapsed, 1064.3 ms CPU, 5533 reads: `SELECT [ps_partkey], [ps_suppkey] FROM [dbo].[partsupp]`
- 1 exec, 100000 rows, 62.0 ms elapsed, 62.0 ms CPU, 602 reads: `SELECT [s_comment], [s_suppkey] FROM [dbo].[supplier]`

## q17: client 19.288 s, 3 statement(s), 119974148 rows to the client, server 19575 ms elapsed / 19566 ms CPU

- 1 exec, 59986052 rows, 10945.2 ms elapsed, 10937.8 ms CPU, 56425 reads: `SELECT [l_partkey], [l_quantity], [l_extendedprice] FROM [dbo].[lineitem]`
- 1 exec, 59986052 rows, 8266.8 ms elapsed, 8265.8 ms CPU, 26946 reads: `SELECT [l_partkey], [l_quantity] FROM [dbo].[lineitem]`
- 1 exec, 2044 rows, 362.6 ms elapsed, 362.6 ms CPU, 740 reads: `SELECT [p_partkey] FROM [dbo].[part] WHERE [p_brand]=@1 AND [p_container]=@2`

## q18: client 19.944 s, 3 statement(s), 136472104 rows to the client, server 19891 ms elapsed / 19884 ms CPU

- 2 exec, 119972104 rows, 16407.8 ms elapsed, 16401.3 ms CPU, 71954 reads: `SELECT [l_orderkey], [l_quantity] FROM [dbo].[lineitem]`
- 1 exec, 15000000 rows, 3241.4 ms elapsed, 3241.0 ms CPU, 23007 reads: `SELECT [o_orderkey], [o_custkey], [o_orderdate], [o_totalprice] FROM [dbo].[orders]`
- 1 exec, 1500000 rows, 241.9 ms elapsed, 241.8 ms CPU, 1126 reads: `SELECT [c_custkey], [c_name] FROM [dbo].[customer]`

## q19: client 11.455 s, 2 statement(s), 16994611 rows to the client, server 11441 ms elapsed / 11440 ms CPU

- 1 exec, 14994611 rows, 10985.7 ms elapsed, 10984.0 ms CPU, 61813 reads: `SELECT [l_partkey],[l_quantity],[l_shipmode],[l_extendedprice],[l_discount] FROM [dbo].[lineitem] WHERE [l_shipinstruct]=@1`
- 1 exec, 2000000 rows, 455.6 ms elapsed, 455.5 ms CPU, 949 reads: `SELECT [p_partkey], [p_brand], [p_container], [p_size] FROM [dbo].[part]`

## q20: client 6.74 s, 5 statement(s), 17220717 rows to the client, server 7617 ms elapsed / 11165 ms CPU

- 1 exec, 9099165 rows, 5300.7 ms elapsed, 8856.6 ms CPU, 59270 reads: `SELECT [l_partkey],[l_suppkey],[l_quantity] FROM [dbo].[lineitem] WHERE [l_shipdate]>=@1 AND [l_shipdate]<@2`
- 1 exec, 8000000 rows, 1344.6 ms elapsed, 1344.4 ms CPU, 7594 reads: `SELECT [ps_partkey], [ps_availqty], [ps_suppkey] FROM [dbo].[partsupp]`
- 1 exec, 21551 rows, 922.9 ms elapsed, 914.9 ms CPU, 7934 reads: `SELECT [p_partkey] FROM [dbo].[part] WHERE ([p_name] LIKE N'forest%')`
- 1 exec, 100000 rows, 47.8 ms elapsed, 47.8 ms CPU, 458 reads: `SELECT [s_suppkey], [s_nationkey], [s_name], [s_address] FROM [dbo].[supplier]`
- 1 exec, 1 rows, 1.1 ms elapsed, 1.1 ms CPU, 11 reads: `SELECT [n_nationkey] FROM [dbo].[nation] WHERE [n_name]=@1`

## q21: client 22.364 s, 6 statement(s), 143253933 rows to the client, server 21652 ms elapsed / 21646 ms CPU

- 1 exec, 59986052 rows, 8921.1 ms elapsed, 8917.2 ms CPU, 49554 reads: `SELECT [l_orderkey], [l_suppkey] FROM [dbo].[lineitem]`
- 1 exec, 37929348 rows, 5367.4 ms elapsed, 5366.8 ms CPU, 73670 reads: `SELECT [l_orderkey], [l_suppkey] FROM [dbo].[lineitem] WHERE ([l_receiptdate] > [l_commitdate])`
- 1 exec, 37929348 rows, 5362.7 ms elapsed, 5361.8 ms CPU, 73670 reads: `SELECT [l_suppkey], [l_orderkey] FROM [dbo].[lineitem] WHERE ([l_receiptdate] > [l_commitdate])`
- 1 exec, 7309184 rows, 1979.7 ms elapsed, 1979.5 ms CPU, 7766 reads: `SELECT [o_orderkey] FROM [dbo].[orders] WHERE [o_orderstatus]=@1`
- 1 exec, 100000 rows, 20.2 ms elapsed, 20.2 ms CPU, 79 reads: `SELECT [s_suppkey], [s_nationkey], [s_name] FROM [dbo].[supplier]`
- 1 exec, 1 rows, 0.8 ms elapsed, 0.8 ms CPU, 11 reads: `SELECT [n_nationkey] FROM [dbo].[nation] WHERE [n_name]=@1`

## q22: client 1.837 s, 3 statement(s), 17863614 rows to the client, server 1824 ms elapsed / 1823 ms CPU

- 1 exec, 15000000 rows, 1289.6 ms elapsed, 1288.5 ms CPU, 5220 reads: `SELECT [o_custkey] FROM [dbo].[orders]`
- 1 exec, 1500000 rows, 309.5 ms elapsed, 309.4 ms CPU, 2284 reads: `SELECT [c_phone], [c_acctbal], [c_custkey] FROM [dbo].[customer]`
- 1 exec, 1363614 rows, 224.9 ms elapsed, 224.9 ms CPU, 1787 reads: `SELECT [c_acctbal],[c_phone] FROM [dbo].[customer] WHERE [c_acctbal]>@1`

