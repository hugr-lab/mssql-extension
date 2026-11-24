CREATE TABLE big AS SELECT i AS id, ('POINT(' || (i%1000) || ' ' || (i//1000) || ')')::GEOMETRY AS g, [random(), random(), random()]::FLOAT[3] AS v FROM range(200000) t(i);
CREATE TABLE small AS SELECT i AS id, ('POINT(' || i || ' 0)')::GEOMETRY AS g, [0.1,0.2,0.3]::FLOAT[3] AS v FROM range(10) t(i);
SET explain_output='optimized_only';
EXPLAIN SELECT count(*) FROM big b JOIN small s ON b.g && s.g;
EXPLAIN SELECT count(*) FROM big b JOIN small s ON array_distance(b.v, s.v) < 0.1;
EXPLAIN SELECT count(*) FROM big b, small s WHERE ST_Intersects_Extent(b.g, s.g);
