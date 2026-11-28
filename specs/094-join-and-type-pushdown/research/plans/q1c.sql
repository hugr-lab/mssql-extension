CREATE TABLE big AS SELECT i AS id, (hash(i) % 1000000000000)::BIGINT AS k, (hash(i)%1000000)::DOUBLE AS d, md5(i::VARCHAR) AS s FROM range(2000000) t(i);
CREATE TABLE b100 AS SELECT k, d, s FROM big WHERE id % 20000 = 0 AND id > (random()*0)::INT - 1;
EXPLAIN ANALYZE SELECT count(*) FROM big b JOIN b100 s ON b.k = s.k;
EXPLAIN ANALYZE SELECT count(*) FROM big b JOIN b100 s ON b.d = s.d;
EXPLAIN ANALYZE SELECT count(*) FROM big b JOIN b100 s ON b.s = s.s;
