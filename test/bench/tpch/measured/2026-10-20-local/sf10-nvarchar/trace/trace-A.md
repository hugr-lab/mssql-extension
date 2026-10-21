# A: DuckDB 1.5.6 + mssql 0.2.5 -- tpch_sf10_columnstore_nvarchar

## q01: client 18.537 s, 1 statement(s), 59142609 rows to the client, server 18531 ms elapsed / 18527 ms CPU

- 1 exec, 59142609 rows, 18530.8 ms elapsed, 18526.8 ms CPU, 53865 reads: `SELECT [l_returnflag],[l_linestatus],[l_quantity],[l_extendedprice],[l_discount],[l_tax] FROM [dbo].[lineitem] WHERE [l_shipdate]<=@1`

## q02: client 1.718 s, 7 statement(s), 16207866 rows to the client, server 3178 ms elapsed / 3175 ms CPU

- 2 exec, 16000000 rows, 2988.3 ms elapsed, 2987.6 ms CPU, 16358 reads: `SELECT [ps_partkey], [ps_suppkey], [ps_supplycost] FROM [dbo].[partsupp]`
- 1 exec, 100000 rows, 137.7 ms elapsed, 137.7 ms CPU, 1385 reads: `SELECT [s_suppkey], [s_nationkey], [s_acctbal], [s_name], [s_address], [s_phone], [s_comment] FROM [dbo].[supplier]`
- 1 exec, 100000 rows, 24.6 ms elapsed, 24.6 ms CPU, 45 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier]`
- 1 exec, 7854 rows, 20.3 ms elapsed, 20.3 ms CPU, 1445 reads: `SELECT [p_partkey], [p_mfgr] FROM [dbo].[part] WHERE ([p_type] LIKE N'%BRASS') AND ([p_size] = 15)`
- 2 exec, 2 rows, 5.3 ms elapsed, 3.1 ms CPU, 34 reads: `SELECT [r_regionkey] FROM [dbo].[region] WHERE [r_name]=@1`
- 1 exec, 5 rows, 1.3 ms elapsed, 1.3 ms CPU, 15 reads: `SELECT [n_nationkey],[n_regionkey],[n_name] FROM [dbo].[nation] WHERE [n_regionkey]=@1`
- 1 exec, 5 rows, 0.7 ms elapsed, 0.7 ms CPU, 11 reads: `SELECT [n_nationkey],[n_regionkey] FROM [dbo].[nation] WHERE [n_regionkey]=@1`

## q03: client 12.852 s, 3 statement(s), 39923968 rows to the client, server 12854 ms elapsed / 12788 ms CPU

- 1 exec, 32334250 rows, 11184.7 ms elapsed, 11151.7 ms CPU, 75093 reads: `SELECT [l_orderkey],[l_extendedprice],[l_discount] FROM [dbo].[lineitem] WHERE [l_shipdate]>@1`
- 1 exec, 7289442 rows, 1605.9 ms elapsed, 1605.6 ms CPU, 15736 reads: `SELECT [o_custkey],[o_orderkey],[o_orderdate],[o_shippriority] FROM [dbo].[orders] WHERE [o_orderdate]<@1`
- 1 exec, 300276 rows, 63.3 ms elapsed, 30.7 ms CPU, 577 reads: `SELECT [c_custkey] FROM [dbo].[customer] WHERE [c_mktsegment]=@1`

## q04: client 9.571 s, 2 statement(s), 38503019 rows to the client, server 9486 ms elapsed / 16723 ms CPU

- 1 exec, 37929348 rows, 9194.3 ms elapsed, 16217.6 ms CPU, 54369 reads: `SELECT [l_orderkey] FROM [dbo].[lineitem] WHERE ([l_commitdate] < [l_receiptdate])`
- 1 exec, 573671 rows, 291.4 ms elapsed, 505.6 ms CPU, 23040 reads: `SELECT [o_orderkey],[o_orderpriority] FROM [dbo].[orders] WHERE [o_orderdate]>=@1 AND [o_orderdate]<@2`

## q05: client 14.147 s, 6 statement(s), 63861977 rows to the client, server 14581 ms elapsed / 15141 ms CPU

- 1 exec, 59986052 rows, 13588.3 ms elapsed, 13559.9 ms CPU, 82778 reads: `SELECT [l_orderkey], [l_suppkey], [l_extendedprice], [l_discount] FROM [dbo].[lineitem]`
- 1 exec, 2275919 rows, 742.7 ms elapsed, 1349.8 ms CPU, 33251 reads: `SELECT [o_custkey],[o_orderkey] FROM [dbo].[orders] WHERE [o_orderdate]>=@1 AND [o_orderdate]<@2`
- 1 exec, 1500000 rows, 213.3 ms elapsed, 211.5 ms CPU, 557 reads: `SELECT [c_custkey], [c_nationkey] FROM [dbo].[customer]`
- 1 exec, 100000 rows, 31.5 ms elapsed, 14.5 ms CPU, 45 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier]`
- 1 exec, 5 rows, 3.0 ms elapsed, 3.0 ms CPU, 15 reads: `SELECT [n_nationkey],[n_regionkey],[n_name] FROM [dbo].[nation] WHERE [n_regionkey]=@1`
- 1 exec, 1 rows, 1.9 ms elapsed, 1.9 ms CPU, 17 reads: `SELECT [r_regionkey] FROM [dbo].[region] WHERE [r_name]=@1`

## q06: client 0.515 s, 1 statement(s), 1139264 rows to the client, server 508 ms elapsed / 1017 ms CPU

- 1 exec, 1139264 rows, 508.5 ms elapsed, 1017.2 ms CPU, 79170 reads: `SELECT [l_discount],[l_extendedprice] FROM [dbo].[lineitem] WHERE [l_shipdate]>=@1 AND [l_shipdate]<@2 AND [l_discount]>=@3 AND [l_discount]<=@4 AND [l_quantity]<@5`

## q07: client 7.531 s, 6 statement(s), 34830329 rows to the client, server 7608 ms elapsed / 7606 ms CPU

- 1 exec, 18230325 rows, 5342.6 ms elapsed, 5341.7 ms CPU, 94832 reads: `SELECT [l_suppkey],[l_orderkey],[l_shipdate],[l_extendedprice],[l_discount] FROM [dbo].[lineitem] WHERE [l_shipdate]>=@1 AND [l_shipdate]<=@2`
- 1 exec, 15000000 rows, 2053.3 ms elapsed, 2052.7 ms CPU, 12610 reads: `SELECT [o_orderkey], [o_custkey] FROM [dbo].[orders]`
- 1 exec, 1500000 rows, 194.5 ms elapsed, 194.5 ms CPU, 557 reads: `SELECT [c_custkey], [c_nationkey] FROM [dbo].[customer]`
- 1 exec, 100000 rows, 13.8 ms elapsed, 13.8 ms CPU, 45 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier]`
- 1 exec, 2 rows, 2.4 ms elapsed, 2.4 ms CPU, 13 reads: `SELECT [n_nationkey], [n_name] FROM [dbo].[nation] WHERE (([n_name] = N'GERMANY') OR ([n_name] = N'FRANCE'))`
- 1 exec, 2 rows, 1.2 ms elapsed, 1.2 ms CPU, 13 reads: `SELECT [n_nationkey], [n_name] FROM [dbo].[nation] WHERE (([n_name] = N'FRANCE') OR ([n_name] = N'GERMANY'))`

## q08: client 18.325 s, 8 statement(s), 66157048 rows to the client, server 18304 ms elapsed / 18296 ms CPU

- 1 exec, 59986052 rows, 17204.1 ms elapsed, 17200.4 ms CPU, 102517 reads: `SELECT [l_partkey], [l_suppkey], [l_orderkey], [l_extendedprice], [l_discount] FROM [dbo].[lineitem]`
- 1 exec, 4557513 rows, 861.3 ms elapsed, 861.2 ms CPU, 15636 reads: `SELECT [o_orderkey],[o_custkey],[o_orderdate] FROM [dbo].[orders] WHERE [o_orderdate]>=@1 AND [o_orderdate]<=@2`
- 1 exec, 1500000 rows, 202.1 ms elapsed, 202.0 ms CPU, 557 reads: `SELECT [c_custkey], [c_nationkey] FROM [dbo].[customer]`
- 1 exec, 100000 rows, 17.9 ms elapsed, 14.4 ms CPU, 45 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier]`
- 1 exec, 13452 rows, 14.6 ms elapsed, 14.6 ms CPU, 1220 reads: `SELECT [p_partkey] FROM [dbo].[part] WHERE [p_type]=@1`
- 1 exec, 1 rows, 2.7 ms elapsed, 2.7 ms CPU, 17 reads: `SELECT [r_regionkey] FROM [dbo].[region] WHERE [r_name]=@1`
- 1 exec, 5 rows, 0.6 ms elapsed, 0.6 ms CPU, 11 reads: `SELECT [n_nationkey],[n_regionkey] FROM [dbo].[nation] WHERE [n_regionkey]=@1`
- 1 exec, 25 rows, 0.5 ms elapsed, 0.5 ms CPU, 13 reads: `SELECT [n_nationkey], [n_name] FROM [dbo].[nation]`

## q09: client 27.841 s, 6 statement(s), 83194859 rows to the client, server 33557 ms elapsed / 29912 ms CPU

- 1 exec, 59986052 rows, 21971.2 ms elapsed, 21961.0 ms CPU, 108673 reads: `SELECT [l_suppkey], [l_partkey], [l_orderkey], [l_extendedprice], [l_discount], [l_quantity] FROM [dbo].[lineitem]`
- 1 exec, 108782 rows, 5798.4 ms elapsed, 4241.2 ms CPU, 15208 reads: `SELECT [p_partkey] FROM [dbo].[part] WHERE ([p_name] LIKE N'%green%')`
- 1 exec, 8000000 rows, 3613.8 ms elapsed, 1536.7 ms CPU, 8179 reads: `SELECT [ps_suppkey], [ps_partkey], [ps_supplycost] FROM [dbo].[partsupp]`
- 1 exec, 15000000 rows, 2158.1 ms elapsed, 2157.8 ms CPU, 10691 reads: `SELECT [o_orderkey], [o_orderdate] FROM [dbo].[orders]`
- 1 exec, 100000 rows, 14.7 ms elapsed, 14.7 ms CPU, 45 reads: `SELECT [s_suppkey], [s_nationkey] FROM [dbo].[supplier]`
- 1 exec, 25 rows, 0.4 ms elapsed, 0.4 ms CPU, 13 reads: `SELECT [n_nationkey], [n_name] FROM [dbo].[nation]`

## q10: client 7.948 s, 4 statement(s), 16881365 rows to the client, server 7892 ms elapsed / 8097 ms CPU

- 1 exec, 14808183 rows, 5351.6 ms elapsed, 5350.2 ms CPU, 63751 reads: `SELECT [l_orderkey],[l_extendedprice],[l_discount] FROM [dbo].[lineitem] WHERE [l_returnflag]=@1`
- 1 exec, 1500000 rows, 2299.5 ms elapsed, 2299.2 ms CPU, 21731 reads: `SELECT [c_custkey], [c_nationkey], [c_name], [c_acctbal], [c_phone], [c_address], [c_comment] FROM [dbo].[customer]`
- 1 exec, 573157 rows, 239.6 ms elapsed, 446.0 ms CPU, 33254 reads: `SELECT [o_custkey],[o_orderkey] FROM [dbo].[orders] WHERE [o_orderdate]>=@1 AND [o_orderdate]<@2`
- 1 exec, 25 rows, 1.2 ms elapsed, 1.2 ms CPU, 13 reads: `SELECT [n_nationkey], [n_name] FROM [dbo].[nation]`

## q11: client 1.97 s, 3 statement(s), 8004050 rows to the client, server 1942 ms elapsed / 1942 ms CPU

- 1 exec, 8000000 rows, 1935.6 ms elapsed, 1935.1 ms CPU, 10256 reads: `SELECT [ps_suppkey], [ps_partkey], [ps_supplycost], [ps_availqty] FROM [dbo].[partsupp]`
- 1 exec, 1 rows, 4.2 ms elapsed, 4.2 ms CPU, 17 reads: `SELECT [n_nationkey] FROM [dbo].[nation] WHERE [n_name]=@1`
- 1 exec, 4049 rows, 2.4 ms elapsed, 2.4 ms CPU, 45 reads: `SELECT [s_suppkey],[s_nationkey] FROM [dbo].[supplier] WHERE [s_nationkey]=@1`

## q12: client 2.812 s, 2 statement(s), 16089346 rows to the client, server 2787 ms elapsed / 4186 ms CPU

- 1 exec, 15000000 rows, 2495.1 ms elapsed, 2494.7 ms CPU, 7765 reads: `SELECT [o_orderkey], [o_orderpriority] FROM [dbo].[orders]`
- 1 exec, 1089346 rows, 292.1 ms elapsed, 1691.2 ms CPU, 103703 reads: `SELECT [l_orderkey],[l_shipmode] FROM [dbo].[lineitem] WHERE [l_commitdate]<[l_receiptdate] AND [l_shipdate]<[l_commitdate] AND [l_receiptdate]>=@1 AND [l_receiptdate]<@2 AND [l_commitdate]<@3 AND [l_shipdate]<@4`

## q13: client 10.377 s, 2 statement(s), 16500000 rows to the client, server 10333 ms elapsed / 10324 ms CPU

- 1 exec, 15000000 rows, 10187.3 ms elapsed, 10178.4 ms CPU, 189460 reads: `SELECT [o_custkey], [o_comment], [o_orderkey] FROM [dbo].[orders]`
- 1 exec, 1500000 rows, 145.6 ms elapsed, 145.6 ms CPU, 537 reads: `SELECT [c_custkey] FROM [dbo].[customer]`

## q14: client 1.198 s, 2 statement(s), 2749223 rows to the client, server 1171 ms elapsed / 1790 ms CPU

- 1 exec, 2000000 rows, 723.7 ms elapsed, 723.6 ms CPU, 961 reads: `SELECT [p_partkey], [p_type] FROM [dbo].[part]`
- 1 exec, 749223 rows, 447.1 ms elapsed, 1066.0 ms CPU, 99385 reads: `SELECT [l_partkey],[l_extendedprice],[l_discount] FROM [dbo].[lineitem] WHERE [l_shipdate]>=@1 AND [l_shipdate]<@2`

## q15: client 1.309 s, 2 statement(s), 2265715 rows to the client, server 1286 ms elapsed / 2498 ms CPU

- 1 exec, 2265714 rows, 1283.7 ms elapsed, 2495.7 ms CPU, 99405 reads: `SELECT [l_suppkey],[l_extendedprice],[l_discount] FROM [dbo].[lineitem] WHERE [l_shipdate]>=@1 AND [l_shipdate]<@2`
- 1 exec, 1 rows, 2.4 ms elapsed, 2.4 ms CPU, 780 reads: `SELECT [s_suppkey],[s_name],[s_address],[s_phone] FROM [dbo].[supplier] WHERE [s_suppkey]=@1`

## q16: client 1.997 s, 3 statement(s), 9955696 rows to the client, server 2036 ms elapsed / 2036 ms CPU

- 1 exec, 8000000 rows, 1157.7 ms elapsed, 1157.6 ms CPU, 5521 reads: `SELECT [ps_partkey], [ps_suppkey] FROM [dbo].[partsupp]`
- 1 exec, 1855696 rows, 781.1 ms elapsed, 781.0 ms CPU, 1202 reads: `SELECT [p_partkey], [p_brand], [p_type], [p_size] FROM [dbo].[part] WHERE (NOT ([p_type] LIKE N'MEDIUM POLISHED%')) AND ([p_brand] <> N'Brand#45')`
- 1 exec, 100000 rows, 97.4 ms elapsed, 97.4 ms CPU, 608 reads: `SELECT [s_comment], [s_suppkey] FROM [dbo].[supplier]`

## q17: client 21.546 s, 3 statement(s), 119974148 rows to the client, server 21438 ms elapsed / 21433 ms CPU

- 1 exec, 59986052 rows, 12256.1 ms elapsed, 12254.3 ms CPU, 56431 reads: `SELECT [l_partkey], [l_quantity], [l_extendedprice] FROM [dbo].[lineitem]`
- 1 exec, 59986052 rows, 9175.8 ms elapsed, 9172.8 ms CPU, 26955 reads: `SELECT [l_partkey], [l_quantity] FROM [dbo].[lineitem]`
- 1 exec, 2044 rows, 6.2 ms elapsed, 6.2 ms CPU, 776 reads: `SELECT [p_partkey] FROM [dbo].[part] WHERE [p_brand]=@1 AND [p_container]=@2`

## q18: client 20.14 s, 3 statement(s), 136472104 rows to the client, server 20084 ms elapsed / 20080 ms CPU

- 2 exec, 119972104 rows, 16399.7 ms elapsed, 16396.7 ms CPU, 71580 reads: `SELECT [l_orderkey], [l_quantity] FROM [dbo].[lineitem]`
- 1 exec, 15000000 rows, 3299.9 ms elapsed, 3298.8 ms CPU, 23204 reads: `SELECT [o_orderkey], [o_custkey], [o_orderdate], [o_totalprice] FROM [dbo].[orders]`
- 1 exec, 1500000 rows, 384.7 ms elapsed, 384.6 ms CPU, 4364 reads: `SELECT [c_custkey], [c_name] FROM [dbo].[customer]`

## q19: client 5.029 s, 2 statement(s), 16994611 rows to the client, server 5018 ms elapsed / 5017 ms CPU

- 1 exec, 14994611 rows, 4568.6 ms elapsed, 4568.1 ms CPU, 62186 reads: `SELECT [l_partkey],[l_quantity],[l_shipmode],[l_extendedprice],[l_discount] FROM [dbo].[lineitem] WHERE [l_shipinstruct]=@1`
- 1 exec, 2000000 rows, 449.4 ms elapsed, 449.3 ms CPU, 948 reads: `SELECT [p_partkey], [p_brand], [p_container], [p_size] FROM [dbo].[part]`

## q20: client 6.888 s, 5 statement(s), 17220717 rows to the client, server 7305 ms elapsed / 10925 ms CPU

- 1 exec, 9099165 rows, 5435.0 ms elapsed, 9058.8 ms CPU, 59294 reads: `SELECT [l_partkey],[l_suppkey],[l_quantity] FROM [dbo].[lineitem] WHERE [l_shipdate]>=@1 AND [l_shipdate]<@2`
- 1 exec, 8000000 rows, 1346.4 ms elapsed, 1346.2 ms CPU, 7598 reads: `SELECT [ps_partkey], [ps_availqty], [ps_suppkey] FROM [dbo].[partsupp]`
- 1 exec, 21551 rows, 462.6 ms elapsed, 458.6 ms CPU, 15208 reads: `SELECT [p_partkey] FROM [dbo].[part] WHERE ([p_name] LIKE N'forest%')`
- 1 exec, 100000 rows, 60.9 ms elapsed, 60.9 ms CPU, 666 reads: `SELECT [s_suppkey], [s_nationkey], [s_name], [s_address] FROM [dbo].[supplier]`
- 1 exec, 1 rows, 0.5 ms elapsed, 0.5 ms CPU, 17 reads: `SELECT [n_nationkey] FROM [dbo].[nation] WHERE [n_name]=@1`

## q21: client 20.244 s, 6 statement(s), 143253933 rows to the client, server 19573 ms elapsed / 19569 ms CPU

- 1 exec, 59986052 rows, 8047.9 ms elapsed, 8045.8 ms CPU, 49373 reads: `SELECT [l_orderkey], [l_suppkey] FROM [dbo].[lineitem]`
- 1 exec, 37929348 rows, 5482.2 ms elapsed, 5481.2 ms CPU, 73504 reads: `SELECT [l_orderkey], [l_suppkey] FROM [dbo].[lineitem] WHERE ([l_receiptdate] > [l_commitdate])`
- 1 exec, 37929348 rows, 5363.6 ms elapsed, 5362.9 ms CPU, 73504 reads: `SELECT [l_suppkey], [l_orderkey] FROM [dbo].[lineitem] WHERE ([l_receiptdate] > [l_commitdate])`
- 1 exec, 7309184 rows, 645.9 ms elapsed, 645.8 ms CPU, 7865 reads: `SELECT [o_orderkey] FROM [dbo].[orders] WHERE [o_orderstatus]=@1`
- 1 exec, 100000 rows, 32.2 ms elapsed, 32.2 ms CPU, 291 reads: `SELECT [s_suppkey], [s_nationkey], [s_name] FROM [dbo].[supplier]`
- 1 exec, 1 rows, 1.0 ms elapsed, 1.0 ms CPU, 17 reads: `SELECT [n_nationkey] FROM [dbo].[nation] WHERE [n_name]=@1`

## q22: client 1.857 s, 3 statement(s), 17863614 rows to the client, server 1843 ms elapsed / 1843 ms CPU

- 1 exec, 15000000 rows, 1280.6 ms elapsed, 1280.5 ms CPU, 5240 reads: `SELECT [o_custkey] FROM [dbo].[orders]`
- 1 exec, 1500000 rows, 316.8 ms elapsed, 316.7 ms CPU, 2789 reads: `SELECT [c_phone], [c_acctbal], [c_custkey] FROM [dbo].[customer]`
- 1 exec, 1363614 rows, 245.5 ms elapsed, 245.5 ms CPU, 2289 reads: `SELECT [c_acctbal],[c_phone] FROM [dbo].[customer] WHERE [c_acctbal]>@1`

