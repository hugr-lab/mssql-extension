CREATE TABLE big AS SELECT i AS id, (i*7)%1000000 AS k, 'x' || i AS s FROM range(2000000) t(i);
CREATE TABLE b100 AS SELECT i*37 AS k, 'x' || (i*37) AS s FROM range(100) t(i) WHERE i > (random()*0)::INT - 1;
CREATE TABLE b100k AS SELECT i*3 AS k FROM range(100000) t(i) WHERE i > (random()*0)::INT - 1;
EXPLAIN ANALYZE SELECT count(*) FROM big b JOIN b100 s ON b.k = s.k;
EXPLAIN ANALYZE SELECT count(*) FROM big b JOIN b100k s ON b.k = s.k;
EXPLAIN ANALYZE SELECT count(*) FROM big b JOIN b100 s ON b.s = s.s;
