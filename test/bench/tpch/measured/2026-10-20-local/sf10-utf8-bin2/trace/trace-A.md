# A: DuckDB 1.5.6 + mssql 0.2.5 -- tpch_sf10_columnstore_utf8bin2

## q01: client 19.994 s, 1 statement(s), 59142609 rows to the client, server 19989 ms elapsed / 19984 ms CPU

- 1 exec, 59142609 rows, 19988.7 ms elapsed, 19984.1 ms CPU, 53873 reads: `SELECT [l_returnflag],[l_linestatus],[l_quantity],[l_extendedprice],[l_discount],[l_tax] FROM [dbo].[lineitem] WHERE [l_shipdate]<=@1`

## q02: client 2.57 s, 7 statement(s), 16207866 rows to the client, server 4014 ms elapsed / 4012 ms CPU

- 2 exec, 16000000 rows, 3856.9 ms elapsed, 3856.3 ms CPU, 16408 reads: `SELECT [ps_partkey], [ps_suppkey], [ps_supplycost] FROM [dbo].[partsupp]`
- 1 exec, 100000 rows, 113.9 ms elapsed, 113.9 ms CPU, 1138 reads: `SELECT [s_suppkey], [s_nationkey], [s_acctbal], [s_name], [s_address], [s_phone], [s_comment] FROM [dbo].[supplier]`
- 1 exec, 7854 rows, 23.9 ms elapsed, 22.8 ms CPU, 1188 reads: `SELECT [p_partkey], [p_mfgr] FROM [dbo].[part] WHERE ([p_type] LIKE N'%BRASS') AND ([p_size] = 15)`
- 1 exec, 100000 rows, 16.9 ms elapsed, 16.9 ms CPU, 45 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier]`
- 2 exec, 2 rows, 1.1 ms elapsed, 1.1 ms CPU, 22 reads: `SELECT [r_regionkey] FROM [dbo].[region] WHERE [r_name]=@1`
- 1 exec, 5 rows, 0.6 ms elapsed, 0.6 ms CPU, 13 reads: `SELECT [n_nationkey],[n_regionkey],[n_name] FROM [dbo].[nation] WHERE [n_regionkey]=@1`
- 1 exec, 5 rows, 0.5 ms elapsed, 0.5 ms CPU, 11 reads: `SELECT [n_nationkey],[n_regionkey] FROM [dbo].[nation] WHERE [n_regionkey]=@1`

## q03: client 7.851 s, 3 statement(s), 39923968 rows to the client, server 7973 ms elapsed / 7972 ms CPU

- 1 exec, 32334250 rows, 6227.3 ms elapsed, 6226.5 ms CPU, 75349 reads: `SELECT [l_orderkey],[l_extendedprice],[l_discount] FROM [dbo].[lineitem] WHERE [l_shipdate]>@1`
- 1 exec, 7289442 rows, 1573.6 ms elapsed, 1573.3 ms CPU, 15739 reads: `SELECT [o_custkey],[o_orderkey],[o_orderdate],[o_shippriority] FROM [dbo].[orders] WHERE [o_orderdate]<@1`
- 1 exec, 300276 rows, 171.8 ms elapsed, 171.8 ms CPU, 551 reads: `SELECT [c_custkey] FROM [dbo].[customer] WHERE [c_mktsegment]=@1`

## q04: client 10.122 s, 2 statement(s), 38503019 rows to the client, server 10069 ms elapsed / 16838 ms CPU

- 1 exec, 37929348 rows, 9815.0 ms elapsed, 16398.8 ms CPU, 54607 reads: `SELECT [l_orderkey] FROM [dbo].[lineitem] WHERE ([l_commitdate] < [l_receiptdate])`
- 1 exec, 573671 rows, 254.4 ms elapsed, 439.4 ms CPU, 23111 reads: `SELECT [o_orderkey],[o_orderpriority] FROM [dbo].[orders] WHERE [o_orderdate]>=@1 AND [o_orderdate]<@2`

## q05: client 13.909 s, 6 statement(s), 63861977 rows to the client, server 14494 ms elapsed / 15066 ms CPU

- 1 exec, 59986052 rows, 13296.8 ms elapsed, 13281.6 ms CPU, 83027 reads: `SELECT [l_orderkey], [l_suppkey], [l_extendedprice], [l_discount] FROM [dbo].[lineitem]`
- 1 exec, 2275919 rows, 929.2 ms elapsed, 1528.3 ms CPU, 33326 reads: `SELECT [o_custkey],[o_orderkey] FROM [dbo].[orders] WHERE [o_orderdate]>=@1 AND [o_orderdate]<@2`
- 1 exec, 1500000 rows, 206.2 ms elapsed, 203.4 ms CPU, 551 reads: `SELECT [c_custkey], [c_nationkey] FROM [dbo].[customer]`
- 1 exec, 100000 rows, 50.3 ms elapsed, 48.7 ms CPU, 45 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier]`
- 1 exec, 1 rows, 10.8 ms elapsed, 2.9 ms CPU, 11 reads: `SELECT [r_regionkey] FROM [dbo].[region] WHERE [r_name]=@1`
- 1 exec, 5 rows, 0.7 ms elapsed, 0.7 ms CPU, 13 reads: `SELECT [n_nationkey],[n_regionkey],[n_name] FROM [dbo].[nation] WHERE [n_regionkey]=@1`

## q06: client 0.524 s, 1 statement(s), 1139264 rows to the client, server 510 ms elapsed / 983 ms CPU

- 1 exec, 1139264 rows, 509.6 ms elapsed, 982.6 ms CPU, 79186 reads: `SELECT [l_discount],[l_extendedprice] FROM [dbo].[lineitem] WHERE [l_shipdate]>=@1 AND [l_shipdate]<@2 AND [l_discount]>=@3 AND [l_discount]<=@4 AND [l_quantity]<@5`

## q07: client 7.161 s, 6 statement(s), 34830329 rows to the client, server 7282 ms elapsed / 7272 ms CPU

- 1 exec, 18230325 rows, 5159.9 ms elapsed, 5149.5 ms CPU, 95080 reads: `SELECT [l_suppkey],[l_orderkey],[l_shipdate],[l_extendedprice],[l_discount] FROM [dbo].[lineitem] WHERE [l_shipdate]>=@1 AND [l_shipdate]<=@2`
- 1 exec, 15000000 rows, 1919.9 ms elapsed, 1919.7 ms CPU, 12594 reads: `SELECT [o_orderkey], [o_custkey] FROM [dbo].[orders]`
- 1 exec, 1500000 rows, 188.5 ms elapsed, 188.5 ms CPU, 551 reads: `SELECT [c_custkey], [c_nationkey] FROM [dbo].[customer]`
- 1 exec, 100000 rows, 12.8 ms elapsed, 12.8 ms CPU, 45 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier]`
- 1 exec, 2 rows, 0.6 ms elapsed, 0.6 ms CPU, 11 reads: `SELECT [n_nationkey], [n_name] FROM [dbo].[nation] WHERE (([n_name] = N'GERMANY') OR ([n_name] = N'FRANCE'))`
- 1 exec, 2 rows, 0.5 ms elapsed, 0.5 ms CPU, 11 reads: `SELECT [n_nationkey], [n_name] FROM [dbo].[nation] WHERE (([n_name] = N'FRANCE') OR ([n_name] = N'GERMANY'))`

## q08: client 17.83 s, 8 statement(s), 66157048 rows to the client, server 18291 ms elapsed / 18288 ms CPU

- 1 exec, 59986052 rows, 16763.1 ms elapsed, 16760.8 ms CPU, 102758 reads: `SELECT [l_partkey], [l_suppkey], [l_orderkey], [l_extendedprice], [l_discount] FROM [dbo].[lineitem]`
- 1 exec, 4557513 rows, 825.9 ms elapsed, 825.9 ms CPU, 15639 reads: `SELECT [o_orderkey],[o_custkey],[o_orderdate] FROM [dbo].[orders] WHERE [o_orderdate]>=@1 AND [o_orderdate]<=@2`
- 1 exec, 13452 rows, 492.2 ms elapsed, 492.2 ms CPU, 962 reads: `SELECT [p_partkey] FROM [dbo].[part] WHERE [p_type]=@1`
- 1 exec, 1500000 rows, 192.6 ms elapsed, 192.6 ms CPU, 551 reads: `SELECT [c_custkey], [c_nationkey] FROM [dbo].[customer]`
- 1 exec, 100000 rows, 15.6 ms elapsed, 14.4 ms CPU, 45 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier]`
- 1 exec, 1 rows, 0.8 ms elapsed, 0.8 ms CPU, 11 reads: `SELECT [r_regionkey] FROM [dbo].[region] WHERE [r_name]=@1`
- 1 exec, 5 rows, 0.7 ms elapsed, 0.7 ms CPU, 11 reads: `SELECT [n_nationkey],[n_regionkey] FROM [dbo].[nation] WHERE [n_regionkey]=@1`
- 1 exec, 25 rows, 0.5 ms elapsed, 0.5 ms CPU, 11 reads: `SELECT [n_nationkey], [n_name] FROM [dbo].[nation]`

## q09: client 21.498 s, 6 statement(s), 83194859 rows to the client, server 23801 ms elapsed / 23771 ms CPU

- 1 exec, 59986052 rows, 19336.5 ms elapsed, 19334.0 ms CPU, 108916 reads: `SELECT [l_suppkey], [l_partkey], [l_orderkey], [l_extendedprice], [l_discount], [l_quantity] FROM [dbo].[lineitem]`
- 1 exec, 15000000 rows, 2087.8 ms elapsed, 2087.6 ms CPU, 10712 reads: `SELECT [o_orderkey], [o_orderdate] FROM [dbo].[orders]`
- 1 exec, 8000000 rows, 1416.9 ms elapsed, 1401.9 ms CPU, 8204 reads: `SELECT [ps_suppkey], [ps_partkey], [ps_supplycost] FROM [dbo].[partsupp]`
- 1 exec, 108782 rows, 933.3 ms elapsed, 933.2 ms CPU, 7931 reads: `SELECT [p_partkey] FROM [dbo].[part] WHERE ([p_name] LIKE N'%green%')`
- 1 exec, 100000 rows, 25.1 ms elapsed, 12.9 ms CPU, 45 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier]`
- 1 exec, 25 rows, 1.2 ms elapsed, 1.2 ms CPU, 11 reads: `SELECT [n_nationkey], [n_name] FROM [dbo].[nation]`

## q10: client 9.929 s, 4 statement(s), 16881365 rows to the client, server 9904 ms elapsed / 10087 ms CPU

- 1 exec, 14808183 rows, 8142.3 ms elapsed, 8141.0 ms CPU, 63652 reads: `SELECT [l_orderkey],[l_extendedprice],[l_discount] FROM [dbo].[lineitem] WHERE [l_returnflag]=@1`
- 1 exec, 1500000 rows, 1527.4 ms elapsed, 1527.3 ms CPU, 17844 reads: `SELECT [c_custkey], [c_nationkey], [c_name], [c_acctbal], [c_phone], [c_address], [c_comment] FROM [dbo].[customer]`
- 1 exec, 573157 rows, 234.1 ms elapsed, 417.9 ms CPU, 33320 reads: `SELECT [o_custkey],[o_orderkey] FROM [dbo].[orders] WHERE [o_orderdate]>=@1 AND [o_orderdate]<@2`
- 1 exec, 25 rows, 0.4 ms elapsed, 0.4 ms CPU, 11 reads: `SELECT [n_nationkey], [n_name] FROM [dbo].[nation]`

## q11: client 1.694 s, 3 statement(s), 8004050 rows to the client, server 1680 ms elapsed / 1680 ms CPU

- 1 exec, 8000000 rows, 1678.2 ms elapsed, 1678.0 ms CPU, 10265 reads: `SELECT [ps_suppkey], [ps_partkey], [ps_supplycost], [ps_availqty] FROM [dbo].[partsupp]`
- 1 exec, 4049 rows, 1.0 ms elapsed, 1.0 ms CPU, 45 reads: `SELECT [s_suppkey],[s_nationkey] FROM [dbo].[supplier] WHERE [s_nationkey]=@1`
- 1 exec, 1 rows, 0.6 ms elapsed, 0.6 ms CPU, 11 reads: `SELECT [n_nationkey] FROM [dbo].[nation] WHERE [n_name]=@1`

## q12: client 2.528 s, 2 statement(s), 16089346 rows to the client, server 2517 ms elapsed / 3514 ms CPU

- 1 exec, 15000000 rows, 2116.2 ms elapsed, 2116.0 ms CPU, 7767 reads: `SELECT [o_orderkey], [o_orderpriority] FROM [dbo].[orders]`
- 1 exec, 1089346 rows, 400.8 ms elapsed, 1397.7 ms CPU, 100354 reads: `SELECT [l_orderkey],[l_shipmode] FROM [dbo].[lineitem] WHERE [l_commitdate]<[l_receiptdate] AND [l_shipdate]<[l_commitdate] AND [l_receiptdate]>=@1 AND [l_receiptdate]<@2 AND [l_commitdate]<@3 AND [l_shipdate]<@4`

## q13: client 7.486 s, 2 statement(s), 16500000 rows to the client, server 7460 ms elapsed / 7459 ms CPU

- 1 exec, 15000000 rows, 7330.4 ms elapsed, 7329.1 ms CPU, 81465 reads: `SELECT [o_custkey], [o_comment], [o_orderkey] FROM [dbo].[orders]`
- 1 exec, 1500000 rows, 129.7 ms elapsed, 129.7 ms CPU, 533 reads: `SELECT [c_custkey] FROM [dbo].[customer]`

## q14: client 1.028 s, 2 statement(s), 2749223 rows to the client, server 1014 ms elapsed / 1449 ms CPU

- 1 exec, 2000000 rows, 547.7 ms elapsed, 547.7 ms CPU, 962 reads: `SELECT [p_partkey], [p_type] FROM [dbo].[part]`
- 1 exec, 749223 rows, 466.1 ms elapsed, 901.7 ms CPU, 99386 reads: `SELECT [l_partkey],[l_extendedprice],[l_discount] FROM [dbo].[lineitem] WHERE [l_shipdate]>=@1 AND [l_shipdate]<@2`

## q15: client 1.348 s, 2 statement(s), 2265715 rows to the client, server 1337 ms elapsed / 2345 ms CPU

- 1 exec, 2265714 rows, 1336.0 ms elapsed, 2344.7 ms CPU, 99366 reads: `SELECT [l_suppkey],[l_extendedprice],[l_discount] FROM [dbo].[lineitem] WHERE [l_shipdate]>=@1 AND [l_shipdate]<@2`
- 1 exec, 1 rows, 0.6 ms elapsed, 0.6 ms CPU, 539 reads: `SELECT [s_suppkey],[s_name],[s_address],[s_phone] FROM [dbo].[supplier] WHERE [s_suppkey]=@1`

## q16: client 2.345 s, 3 statement(s), 9955696 rows to the client, server 2367 ms elapsed / 2367 ms CPU

- 1 exec, 1855696 rows, 1258.5 ms elapsed, 1258.3 ms CPU, 1188 reads: `SELECT [p_partkey], [p_brand], [p_type], [p_size] FROM [dbo].[part] WHERE (NOT ([p_type] LIKE N'MEDIUM POLISHED%')) AND ([p_brand] <> N'Brand#45')`
- 1 exec, 8000000 rows, 1047.3 ms elapsed, 1047.0 ms CPU, 5533 reads: `SELECT [ps_partkey], [ps_suppkey] FROM [dbo].[partsupp]`
- 1 exec, 100000 rows, 61.3 ms elapsed, 61.3 ms CPU, 602 reads: `SELECT [s_comment], [s_suppkey] FROM [dbo].[supplier]`

## q17: client 19.041 s, 3 statement(s), 119974148 rows to the client, server 19340 ms elapsed / 19337 ms CPU

- 1 exec, 59986052 rows, 10792.3 ms elapsed, 10791.1 ms CPU, 56429 reads: `SELECT [l_partkey], [l_quantity], [l_extendedprice] FROM [dbo].[lineitem]`
- 1 exec, 59986052 rows, 8182.2 ms elapsed, 8181.0 ms CPU, 26949 reads: `SELECT [l_partkey], [l_quantity] FROM [dbo].[lineitem]`
- 1 exec, 2044 rows, 365.3 ms elapsed, 365.3 ms CPU, 739 reads: `SELECT [p_partkey] FROM [dbo].[part] WHERE [p_brand]=@1 AND [p_container]=@2`

## q18: client 19.835 s, 3 statement(s), 136472104 rows to the client, server 19790 ms elapsed / 19787 ms CPU

- 2 exec, 119972104 rows, 16290.1 ms elapsed, 16288.2 ms CPU, 72082 reads: `SELECT [l_orderkey], [l_quantity] FROM [dbo].[lineitem]`
- 1 exec, 15000000 rows, 3258.4 ms elapsed, 3258.0 ms CPU, 23011 reads: `SELECT [o_orderkey], [o_custkey], [o_orderdate], [o_totalprice] FROM [dbo].[orders]`
- 1 exec, 1500000 rows, 241.0 ms elapsed, 241.0 ms CPU, 1129 reads: `SELECT [c_custkey], [c_name] FROM [dbo].[customer]`

## q19: client 11.437 s, 2 statement(s), 16994611 rows to the client, server 11424 ms elapsed / 11423 ms CPU

- 1 exec, 14994611 rows, 10975.6 ms elapsed, 10974.3 ms CPU, 61831 reads: `SELECT [l_partkey],[l_quantity],[l_shipmode],[l_extendedprice],[l_discount] FROM [dbo].[lineitem] WHERE [l_shipinstruct]=@1`
- 1 exec, 2000000 rows, 448.4 ms elapsed, 448.3 ms CPU, 949 reads: `SELECT [p_partkey], [p_brand], [p_container], [p_size] FROM [dbo].[part]`

## q20: client 6.787 s, 5 statement(s), 17220717 rows to the client, server 7590 ms elapsed / 11086 ms CPU

- 1 exec, 9099165 rows, 5350.0 ms elapsed, 8855.3 ms CPU, 59279 reads: `SELECT [l_partkey],[l_suppkey],[l_quantity] FROM [dbo].[lineitem] WHERE [l_shipdate]>=@1 AND [l_shipdate]<@2`
- 1 exec, 8000000 rows, 1335.9 ms elapsed, 1335.8 ms CPU, 7594 reads: `SELECT [ps_partkey], [ps_availqty], [ps_suppkey] FROM [dbo].[partsupp]`
- 1 exec, 21551 rows, 850.6 ms elapsed, 841.8 ms CPU, 7931 reads: `SELECT [p_partkey] FROM [dbo].[part] WHERE ([p_name] LIKE N'forest%')`
- 1 exec, 100000 rows, 52.9 ms elapsed, 52.9 ms CPU, 458 reads: `SELECT [s_suppkey], [s_nationkey], [s_name], [s_address] FROM [dbo].[supplier]`
- 1 exec, 1 rows, 0.6 ms elapsed, 0.6 ms CPU, 11 reads: `SELECT [n_nationkey] FROM [dbo].[nation] WHERE [n_name]=@1`

## q21: client 21.261 s, 6 statement(s), 143253933 rows to the client, server 20587 ms elapsed / 20585 ms CPU

- 1 exec, 59986052 rows, 7778.3 ms elapsed, 7777.4 ms CPU, 49614 reads: `SELECT [l_orderkey], [l_suppkey] FROM [dbo].[lineitem]`
- 1 exec, 37929348 rows, 5418.3 ms elapsed, 5417.6 ms CPU, 73734 reads: `SELECT [l_suppkey], [l_orderkey] FROM [dbo].[lineitem] WHERE ([l_receiptdate] > [l_commitdate])`
- 1 exec, 37929348 rows, 5395.9 ms elapsed, 5395.2 ms CPU, 73734 reads: `SELECT [l_orderkey], [l_suppkey] FROM [dbo].[lineitem] WHERE ([l_receiptdate] > [l_commitdate])`
- 1 exec, 7309184 rows, 1970.8 ms elapsed, 1970.6 ms CPU, 7767 reads: `SELECT [o_orderkey] FROM [dbo].[orders] WHERE [o_orderstatus]=@1`
- 1 exec, 100000 rows, 23.4 ms elapsed, 23.4 ms CPU, 79 reads: `SELECT [s_suppkey], [s_nationkey], [s_name] FROM [dbo].[supplier]`
- 1 exec, 1 rows, 0.7 ms elapsed, 0.7 ms CPU, 11 reads: `SELECT [n_nationkey] FROM [dbo].[nation] WHERE [n_name]=@1`

## q22: client 1.833 s, 3 statement(s), 17863614 rows to the client, server 1819 ms elapsed / 1819 ms CPU

- 1 exec, 15000000 rows, 1280.9 ms elapsed, 1280.7 ms CPU, 5222 reads: `SELECT [o_custkey] FROM [dbo].[orders]`
- 1 exec, 1500000 rows, 307.9 ms elapsed, 307.9 ms CPU, 2284 reads: `SELECT [c_phone], [c_acctbal], [c_custkey] FROM [dbo].[customer]`
- 1 exec, 1363614 rows, 230.2 ms elapsed, 230.1 ms CPU, 1787 reads: `SELECT [c_acctbal],[c_phone] FROM [dbo].[customer] WHERE [c_acctbal]>@1`

