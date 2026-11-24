CREATE TABLE big AS SELECT i AS id, ('POINT(' || (i%1000) || ' ' || (i//1000) || ')')::GEOMETRY AS g, [random(), random(), random()]::FLOAT[3] AS v FROM range(200000) t(i);
CREATE TABLE small AS SELECT i AS id, ('POLYGON((' || i || ' 0, ' || (i+5) || ' 0, ' || (i+5) || ' 5, ' || i || ' 5, ' || i || ' 0))')::GEOMETRY AS g, [0.1,0.2,0.3]::FLOAT[3] AS v FROM range(10) t(i);
EXPLAIN SELECT count(*) FROM big b JOIN small s ON b.g && s.g;
EXPLAIN SELECT count(*) FROM big b JOIN small s ON ST_Intersects_Extent(b.g, s.g);
EXPLAIN SELECT count(*) FROM big b JOIN small s ON array_distance(b.v, s.v) < 0.1;
EXPLAIN SELECT count(*) FROM big b JOIN small s ON b.id = s.id AND b.g && s.g;
EXPLAIN SELECT count(*) FROM big b JOIN small s ON b.id < s.id;
EXPLAIN SELECT count(*) FROM big b JOIN small s ON b.id < s.id AND b.id > s.id - 10;
