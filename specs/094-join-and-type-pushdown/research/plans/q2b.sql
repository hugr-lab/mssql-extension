CREATE TABLE big AS SELECT i AS id, (i*7)%100000 AS k, ('POINT(' || (i%1000) || ' ' || (i//1000) || ')')::GEOMETRY AS g FROM range(200000) t(i);
CREATE TABLE small AS SELECT i*37 AS k, ('POLYGON((' || i || ' 0, ' || (i+5) || ' 0, ' || (i+5) || ' 5, ' || i || ' 5, ' || i || ' 0))')::GEOMETRY AS g FROM range(10) t(i);
CREATE TABLE smallf AS SELECT * FROM small WHERE k > (random()*0)::INT;
.mode list
EXPLAIN ANALYZE SELECT count(*) FROM big b JOIN smallf s ON b.k = s.k;
EXPLAIN ANALYZE SELECT count(*) FROM big b JOIN smallf s ON b.k < s.k;
EXPLAIN ANALYZE SELECT count(*) FROM big b JOIN smallf s ON b.g && s.g;
