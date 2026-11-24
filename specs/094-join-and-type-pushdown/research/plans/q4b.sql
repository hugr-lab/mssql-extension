CREATE TABLE big AS SELECT i AS id, [random(), random(), random()]::FLOAT[3] AS v FROM range(200000) t(i);
SET disabled_optimizers='late_materialization';
EXPLAIN SELECT id FROM big ORDER BY array_cosine_distance(v, [0.1,0.2,0.3]::FLOAT[3]) LIMIT 5;
EXPLAIN SELECT id, array_distance(v, [0.1,0.2,0.3]::FLOAT[3]) AS d FROM big WHERE array_distance(v, [0.1,0.2,0.3]::FLOAT[3]) < 0.5 ORDER BY d LIMIT 5;
SET explain_output='all';
EXPLAIN SELECT id FROM big ORDER BY array_cosine_distance(v, [0.1,0.2,0.3]::FLOAT[3]) LIMIT 5;
