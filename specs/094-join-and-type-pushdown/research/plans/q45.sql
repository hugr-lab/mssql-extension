CREATE TABLE big AS SELECT i AS id, ('POINT(' || (i%1000) || ' ' || (i//1000) || ')')::GEOMETRY AS g, [random(), random(), random()]::FLOAT[3] AS v FROM range(200000) t(i);
EXPLAIN SELECT id FROM big WHERE g && ST_GeomFromWKB(ST_AsWKB('POLYGON((0 0, 5 0, 5 5, 0 5, 0 0))'::GEOMETRY));
EXPLAIN SELECT id FROM big WHERE ST_Intersects_Extent(g, 'POLYGON((0 0, 5 0, 5 5, 0 5, 0 0))'::GEOMETRY);
EXPLAIN SELECT id FROM big WHERE array_distance(v, [0.1,0.2,0.3]::FLOAT[3]) < 0.5;
EXPLAIN SELECT id, array_cosine_distance(v, [0.1,0.2,0.3]::FLOAT[3]) d FROM big ORDER BY array_cosine_distance(v, [0.1,0.2,0.3]::FLOAT[3]) LIMIT 5;
EXPLAIN SELECT id FROM big ORDER BY array_cosine_distance(v, [0.1,0.2,0.3]::FLOAT[3]) LIMIT 5;
EXPLAIN SELECT id FROM big ORDER BY array_distance(v, [0.1,0.2,0.3]::FLOAT[3]) LIMIT 5;
