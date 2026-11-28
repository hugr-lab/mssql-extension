CREATE TABLE big AS SELECT i AS id, (i%1000)::DOUBLE AS x, (i//1000)::DOUBLE AS y, ('POINT(' || (i%1000) || ' ' || (i//1000) || ')')::GEOMETRY AS g FROM range(200000) t(i);
CREATE TABLE small AS SELECT i::DOUBLE AS x0, (i+5)::DOUBLE AS x1, 0::DOUBLE AS y0, 5::DOUBLE AS y1, ('POLYGON((' || i || ' 0, ' || (i+5) || ' 0, ' || (i+5) || ' 5, ' || i || ' 5, ' || i || ' 0))')::GEOMETRY AS g FROM range(10) t(i);
CREATE TABLE smallf AS SELECT * FROM small WHERE x0 > (random()*0)-1;
EXPLAIN ANALYZE SELECT count(*) FROM big b JOIN smallf s ON b.x >= s.x0 AND b.x <= s.x1 AND b.y >= s.y0 AND b.y <= s.y1 AND b.g && s.g;
EXPLAIN ANALYZE SELECT count(*) FROM big b JOIN smallf s ON b.x >= s.x0 AND b.x <= s.x1 AND b.g && s.g;
