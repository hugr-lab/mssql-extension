CREATE TABLE big AS SELECT i AS id, md5(i::VARCHAR) AS s FROM range(200000) t(i);
CREATE TABLE one AS SELECT 'B' AS s FROM range(1) WHERE random() >= 0;
EXPLAIN ANALYZE SELECT count(*) FROM big b JOIN one o ON b.s < o.s;
