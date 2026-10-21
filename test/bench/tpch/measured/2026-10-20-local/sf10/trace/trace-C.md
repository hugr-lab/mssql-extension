# C: SQL Server (native T-SQL) -- tpch_sf10_columnstore

## q01: client 0.173 s, 1 statement(s), 4 rows to the client, server 161 ms elapsed / 1101 ms CPU

- 1 exec, 4 rows, 161.1 ms elapsed, 1100.6 ms CPU, 123215 reads: `SELECT     l_returnflag,     l_linestatus,     sum(l_quantity) AS sum_qty,     sum(l_extendedprice) AS sum_base_price,     sum(l_extendedprice * (1 - l_discount)) AS sum_disc_price,     sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge,     avg(l_quantity) AS avg_qty,     avg(l_extendedprice) AS avg_price,     avg(l_discount) AS avg_disc,     count(*) AS count_order FROM     lineitem WHERE     l_shipdate <= CAST('1998-09-02' AS date) GROUP BY     l_returnflag,     l_linestatus ORDER BY     l_returnflag,     l_linestatus`

## q02: client 0.194 s, 1 statement(s), 100 rows to the client, server 142 ms elapsed / 138 ms CPU

- 1 exec, 100 rows, 141.9 ms elapsed, 138.3 ms CPU, 26871 reads: `SELECT TOP 100     s_acctbal,     s_name,     n_name,     p_partkey,     p_mfgr,     s_address,     s_phone,     s_comment FROM     part,     supplier,     partsupp,     nation,     region WHERE     p_partkey = ps_partkey     AND s_suppkey = ps_suppkey     AND p_size = 15     AND p_type LIKE '%BRASS'     AND s_nationkey = n_nationkey     AND n_regionkey = r_regionkey     AND r_name = 'EUROPE'     AND ps_supplycost = (         SELECT             min(ps_supplycost)         FROM             partsupp,             supplier,             nation,             region         WHERE             p_partkey `

## q03: client 0.313 s, 1 statement(s), 10 rows to the client, server 294 ms elapsed / 1916 ms CPU

- 1 exec, 10 rows, 294.1 ms elapsed, 1916.4 ms CPU, 254057 reads: `SELECT TOP 10     l_orderkey,     sum(l_extendedprice * (1 - l_discount)) AS revenue,     o_orderdate,     o_shippriority FROM     customer,     orders,     lineitem WHERE     c_mktsegment = 'BUILDING'     AND c_custkey = o_custkey     AND l_orderkey = o_orderkey     AND o_orderdate < CAST('1995-03-15' AS date)     AND l_shipdate > CAST('1995-03-15' AS date) GROUP BY     l_orderkey,     o_orderdate,     o_shippriority ORDER BY     revenue DESC,     o_orderdate`

## q04: client 0.241 s, 1 statement(s), 5 rows to the client, server 218 ms elapsed / 1425 ms CPU

- 1 exec, 5 rows, 217.9 ms elapsed, 1425.4 ms CPU, 192953 reads: `SELECT     o_orderpriority,     count(*) AS order_count FROM     orders WHERE     o_orderdate >= CAST('1993-07-01' AS date)     AND o_orderdate < CAST('1993-10-01' AS date)     AND EXISTS (         SELECT             *         FROM             lineitem         WHERE             l_orderkey = o_orderkey             AND l_commitdate < l_receiptdate) GROUP BY     o_orderpriority ORDER BY     o_orderpriority`

## q05: client 0.4 s, 1 statement(s), 5 rows to the client, server 298 ms elapsed / 1815 ms CPU

- 1 exec, 5 rows, 298.1 ms elapsed, 1814.6 ms CPU, 295646 reads: `SELECT     n_name,     sum(l_extendedprice * (1 - l_discount)) AS revenue FROM     customer,     orders,     lineitem,     supplier,     nation,     region WHERE     c_custkey = o_custkey     AND l_orderkey = o_orderkey     AND l_suppkey = s_suppkey     AND c_nationkey = s_nationkey     AND s_nationkey = n_nationkey     AND n_regionkey = r_regionkey     AND r_name = 'ASIA'     AND o_orderdate >= CAST('1994-01-01' AS date)     AND o_orderdate < CAST('1995-01-01' AS date) GROUP BY     n_name ORDER BY     revenue DESC`

## q06: client 0.139 s, 1 statement(s), 1 rows to the client, server 132 ms elapsed / 820 ms CPU

- 1 exec, 1 rows, 132.0 ms elapsed, 820.0 ms CPU, 153870 reads: `SELECT SUM([l_extendedprice]*[l_discount]) [revenue] FROM [lineitem] WHERE [l_shipdate]>=CONVERT([date],@1) AND [l_shipdate]<CONVERT([date],@2) AND [l_discount]>=@3 AND [l_discount]<=@4 AND [l_quantity]<@5`

## q07: client 0.409 s, 1 statement(s), 4 rows to the client, server 298 ms elapsed / 2100 ms CPU

- 1 exec, 4 rows, 297.5 ms elapsed, 2099.5 ms CPU, 317981 reads: `SELECT     supp_nation,     cust_nation,     l_year,     sum(volume) AS revenue FROM (     SELECT         n1.n_name AS supp_nation,         n2.n_name AS cust_nation,         year(l_shipdate) AS l_year,         l_extendedprice * (1 - l_discount) AS volume     FROM         supplier,         lineitem,         orders,         customer,         nation n1,         nation n2     WHERE         s_suppkey = l_suppkey         AND o_orderkey = l_orderkey         AND c_custkey = o_custkey         AND s_nationkey = n1.n_nationkey         AND c_nationkey = n2.n_nationkey         AND ((n1.n_name = 'FRANCE'   `

## q08: client 0.498 s, 1 statement(s), 2 rows to the client, server 340 ms elapsed / 2002 ms CPU

- 1 exec, 2 rows, 340.2 ms elapsed, 2002.3 ms CPU, 359267 reads: `SELECT     o_year,     sum(         CASE WHEN nation = 'BRAZIL' THEN             volume         ELSE             0         END) / sum(volume) AS mkt_share FROM (     SELECT         year(o_orderdate) AS o_year,         l_extendedprice * (1 - l_discount) AS volume,         n2.n_name AS nation     FROM         part,         supplier,         lineitem,         orders,         customer,         nation n1,         nation n2,         region     WHERE         p_partkey = l_partkey         AND s_suppkey = l_suppkey         AND l_orderkey = o_orderkey         AND o_custkey = c_custkey         AND c_nati`

## q09: client 1.018 s, 1 statement(s), 175 rows to the client, server 837 ms elapsed / 6477 ms CPU

- 1 exec, 175 rows, 836.6 ms elapsed, 6477.0 ms CPU, 448551 reads: `SELECT     nation,     o_year,     sum(amount) AS sum_profit FROM (     SELECT         n_name AS nation,         year(o_orderdate) AS o_year,         l_extendedprice * (1 - l_discount) - ps_supplycost * l_quantity AS amount     FROM         part,         supplier,         lineitem,         partsupp,         orders,         nation     WHERE         s_suppkey = l_suppkey         AND ps_suppkey = l_suppkey         AND ps_partkey = l_partkey         AND p_partkey = l_partkey         AND o_orderkey = l_orderkey         AND s_nationkey = n_nationkey         AND p_name LIKE '%green%') AS profit GROUP`

## q10: client 1.036 s, 1 statement(s), 20 rows to the client, server 1008 ms elapsed / 4154 ms CPU

- 1 exec, 20 rows, 1008.5 ms elapsed, 4154.4 ms CPU, 328937 reads: `SELECT TOP 20     c_custkey,     c_name,     sum(l_extendedprice * (1 - l_discount)) AS revenue,     c_acctbal,     n_name,     c_address,     c_phone,     c_comment FROM     customer,     orders,     lineitem,     nation WHERE     c_custkey = o_custkey     AND l_orderkey = o_orderkey     AND o_orderdate >= CAST('1993-10-01' AS date)     AND o_orderdate < CAST('1994-01-01' AS date)     AND l_returnflag = 'R'     AND c_nationkey = n_nationkey GROUP BY     c_custkey,     c_name,     c_acctbal,     c_phone,     n_name,     c_address,     c_comment ORDER BY     revenue DESC`

## q11: client 0.164 s, 1 statement(s), 0 rows to the client, server 132 ms elapsed / 327 ms CPU

- 1 exec, 0 rows, 132.4 ms elapsed, 327.0 ms CPU, 26123 reads: `SELECT     ps_partkey,     sum(ps_supplycost * ps_availqty) AS value FROM     partsupp,     supplier,     nation WHERE     ps_suppkey = s_suppkey     AND s_nationkey = n_nationkey     AND n_name = 'GERMANY' GROUP BY     ps_partkey HAVING     sum(ps_supplycost * ps_availqty) > (         SELECT             sum(ps_supplycost * ps_availqty) * 0.0001000000         FROM             partsupp,             supplier,             nation         WHERE             ps_suppkey = s_suppkey             AND s_nationkey = n_nationkey             AND n_name = 'GERMANY') ORDER BY     value DESC`

## q12: client 0.248 s, 1 statement(s), 2 rows to the client, server 233 ms elapsed / 1585 ms CPU

- 1 exec, 2 rows, 232.7 ms elapsed, 1585.2 ms CPU, 226420 reads: `SELECT     l_shipmode,     sum(         CASE WHEN o_orderpriority = '1-URGENT'             OR o_orderpriority = '2-HIGH' THEN             1         ELSE             0         END) AS high_line_count,     sum(         CASE WHEN o_orderpriority <> '1-URGENT'             AND o_orderpriority <> '2-HIGH' THEN             1         ELSE             0         END) AS low_line_count FROM     orders,     lineitem WHERE     o_orderkey = l_orderkey     AND l_shipmode IN ('MAIL', 'SHIP')     AND l_commitdate < l_receiptdate     AND l_shipdate < l_commitdate     AND l_receiptdate >= CAST('1994-01-01' AS da`

## q13: client 1.188 s, 1 statement(s), 45 rows to the client, server 1165 ms elapsed / 9776 ms CPU

- 1 exec, 45 rows, 1164.9 ms elapsed, 9776.3 ms CPU, 221257 reads: `SELECT     c_count,     count(*) AS custdist FROM (     SELECT         c_custkey,         count(o_orderkey)     FROM         customer     LEFT OUTER JOIN orders ON c_custkey = o_custkey     AND o_comment NOT LIKE '%special%requests%' GROUP BY     c_custkey) AS c_orders (c_custkey,         c_count) GROUP BY     c_count ORDER BY     custdist DESC,     c_count DESC`

## q14: client 0.217 s, 1 statement(s), 1 rows to the client, server 208 ms elapsed / 1185 ms CPU

- 1 exec, 1 rows, 207.7 ms elapsed, 1184.8 ms CPU, 197739 reads: `SELECT     100.00 * sum(         CASE WHEN p_type LIKE 'PROMO%' THEN             l_extendedprice * (1 - l_discount)         ELSE             0         END) / sum(l_extendedprice * (1 - l_discount)) AS promo_revenue FROM     lineitem,     part WHERE     l_partkey = p_partkey     AND l_shipdate >= CAST('1995-09-01' AS date)     AND l_shipdate < CAST('1995-10-01' AS date)`

## q15: client 0.285 s, 1 statement(s), 1 rows to the client, server 249 ms elapsed / 1346 ms CPU

- 1 exec, 1 rows, 249.0 ms elapsed, 1346.2 ms CPU, 194811 reads: `WITH revenue AS (     SELECT         l_suppkey AS supplier_no,         sum(l_extendedprice * (1 - l_discount)) AS total_revenue     FROM         lineitem     WHERE         l_shipdate >= CAST('1996-01-01' AS date)       AND l_shipdate < CAST('1996-04-01' AS date)     GROUP BY         l_suppkey ) SELECT     s_suppkey,     s_name,     s_address,     s_phone,     total_revenue FROM     supplier,     revenue WHERE     s_suppkey = supplier_no     AND total_revenue = (         SELECT             max(total_revenue)         FROM revenue) ORDER BY     s_suppkey`

## q16: client 0.351 s, 1 statement(s), 27840 rows to the client, server 312 ms elapsed / 1321 ms CPU

- 1 exec, 27840 rows, 311.7 ms elapsed, 1321.3 ms CPU, 19449 reads: `SELECT     p_brand,     p_type,     p_size,     count(DISTINCT ps_suppkey) AS supplier_cnt FROM     partsupp,     part WHERE     p_partkey = ps_partkey     AND p_brand <> 'Brand#45'     AND p_type NOT LIKE 'MEDIUM POLISHED%'     AND p_size IN (49, 14, 23, 45, 19, 3, 36, 9)     AND ps_suppkey NOT IN (         SELECT             s_suppkey         FROM             supplier         WHERE             s_comment LIKE '%Customer%Complaints%') GROUP BY     p_brand,     p_type,     p_size ORDER BY     supplier_cnt DESC,     p_brand,     p_type,     p_size`

## q17: client 0.239 s, 1 statement(s), 1 rows to the client, server 209 ms elapsed / 1356 ms CPU

- 1 exec, 1 rows, 208.7 ms elapsed, 1355.8 ms CPU, 237758 reads: `SELECT     sum(l_extendedprice) / 7.0 AS avg_yearly FROM     lineitem,     part WHERE     p_partkey = l_partkey     AND p_brand = 'Brand#23'     AND p_container = 'MED BOX'     AND l_quantity < (         SELECT             0.2 * avg(l_quantity)         FROM             lineitem         WHERE             l_partkey = p_partkey)`

## q18: client 1.057 s, 1 statement(s), 100 rows to the client, server 1018 ms elapsed / 5923 ms CPU

- 1 exec, 100 rows, 1018.1 ms elapsed, 5923.1 ms CPU, 256629 reads: `SELECT TOP 100     c_name,     c_custkey,     o_orderkey,     o_orderdate,     o_totalprice,     sum(l_quantity) FROM     customer,     orders,     lineitem WHERE     o_orderkey IN (         SELECT             l_orderkey         FROM             lineitem         GROUP BY             l_orderkey         HAVING             sum(l_quantity) > 300)     AND c_custkey = o_custkey     AND o_orderkey = l_orderkey GROUP BY     c_name,     c_custkey,     o_orderkey,     o_orderdate,     o_totalprice ORDER BY     o_totalprice DESC,     o_orderdate`

## q19: client 0.196 s, 1 statement(s), 1 rows to the client, server 177 ms elapsed / 1091 ms CPU

- 1 exec, 1 rows, 177.3 ms elapsed, 1090.7 ms CPU, 195167 reads: `SELECT     sum(l_extendedprice * (1 - l_discount)) AS revenue FROM     lineitem,     part WHERE (p_partkey = l_partkey     AND p_brand = 'Brand#12'     AND p_container IN ('SM CASE', 'SM BOX', 'SM PACK', 'SM PKG')     AND l_quantity >= 1     AND l_quantity <= 1 + 10     AND p_size BETWEEN 1 AND 5     AND l_shipmode IN ('AIR', 'AIR REG')     AND l_shipinstruct = 'DELIVER IN PERSON')     OR (p_partkey = l_partkey         AND p_brand = 'Brand#23'         AND p_container IN ('MED BAG', 'MED BOX', 'MED PKG', 'MED PACK')         AND l_quantity >= 10         AND l_quantity <= 10 + 10         AND p_si`

## q20: client 1.425 s, 1 statement(s), 1804 rows to the client, server 1097 ms elapsed / 1088 ms CPU

- 1 exec, 1804 rows, 1096.6 ms elapsed, 1088.1 ms CPU, 174836 reads: `SELECT     s_name,     s_address FROM     supplier,     nation WHERE     s_suppkey IN (         SELECT             ps_suppkey         FROM             partsupp         WHERE             ps_partkey IN (                 SELECT                     p_partkey                 FROM                     part                 WHERE                     p_name LIKE 'forest%')                 AND ps_availqty > (                     SELECT                         0.5 * sum(l_quantity)                     FROM                         lineitem                     WHERE                         l_partkey = ps_pa`

## q21: client 0.694 s, 1 statement(s), 100 rows to the client, server 617 ms elapsed / 4532 ms CPU

- 1 exec, 100 rows, 617.0 ms elapsed, 4531.5 ms CPU, 600192 reads: `SELECT TOP 100     s_name,     count(*) AS numwait FROM     supplier,     lineitem l1,     orders,     nation WHERE     s_suppkey = l1.l_suppkey     AND o_orderkey = l1.l_orderkey     AND o_orderstatus = 'F'     AND l1.l_receiptdate > l1.l_commitdate     AND EXISTS (         SELECT             *         FROM             lineitem l2         WHERE             l2.l_orderkey = l1.l_orderkey             AND l2.l_suppkey <> l1.l_suppkey)     AND NOT EXISTS (         SELECT             *         FROM             lineitem l3         WHERE             l3.l_orderkey = l1.l_orderkey             AND l3.l_`

## q22: client 0.166 s, 1 statement(s), 7 rows to the client, server 139 ms elapsed / 610 ms CPU

- 1 exec, 7 rows, 138.9 ms elapsed, 609.6 ms CPU, 21549 reads: `SELECT     cntrycode,     count(*) AS numcust,     sum(c_acctbal) AS totacctbal FROM (     SELECT         substring(c_phone, 1, 2) AS cntrycode,         c_acctbal     FROM         customer     WHERE         substring(c_phone, 1, 2) IN ('13', '31', '23', '29', '30', '18', '17')         AND c_acctbal > (             SELECT                 avg(c_acctbal)             FROM                 customer             WHERE                 c_acctbal > 0.00                 AND substring(c_phone, 1, 2) IN ('13', '31', '23', '29', '30', '18', '17'))             AND NOT EXISTS (                 SELECT          `

