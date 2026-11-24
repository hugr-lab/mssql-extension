SET NOCOUNT ON; SET QUOTED_IDENTIFIER ON;
DECLARE @q vector(3) = CAST('[0.5,-0.25,1.0]' AS vector(3));
-- The type and the values, full precision (style 3: 17 digits), for DuckDB to recompute.
SELECT TOP (1) 'type' AS what, SQL_VARIANT_PROPERTY(VECTOR_DISTANCE('cosine', v, @q), 'BaseType') AS t FROM dbo.vec;
SELECT 'row' AS what, id, CAST(v AS varchar(200)) AS v,
  CONVERT(varchar(40), VECTOR_DISTANCE('cosine', v, @q), 3) AS cosine,
  CONVERT(varchar(40), VECTOR_DISTANCE('euclidean', v, @q), 3) AS euclidean,
  CONVERT(varchar(40), VECTOR_DISTANCE('dot', v, @q), 3) AS dot
FROM dbo.vec WHERE id <= 2000 ORDER BY id;
-- NULL and the zero vector.
SELECT 'edge' AS what, id, CONVERT(varchar(40), VECTOR_DISTANCE('cosine', v, @q), 3) AS cosine,
  CONVERT(varchar(40), VECTOR_DISTANCE('euclidean', v, @q), 3) AS euclidean FROM dbo.vec WHERE id > 100000;
-- kNN: TOP k by each metric.
SELECT TOP (10) 'knn_cosine' AS what, id FROM dbo.vec ORDER BY VECTOR_DISTANCE('cosine', v, @q), id;
-- A parameter typed vector(3) through sp_executesql, and a dimension mismatch.
EXEC sp_executesql N'SELECT TOP (3) ''param'' AS what, id FROM dbo.vec ORDER BY VECTOR_DISTANCE(''euclidean'', v, @p), id', N'@p vector(3)', @p = @q;
BEGIN TRY SELECT VECTOR_DISTANCE('cosine', v, CAST('[1,2]' AS vector(2))) FROM dbo.vec WHERE id = 1; END TRY BEGIN CATCH SELECT 'mismatch' AS what, ERROR_NUMBER() AS n, ERROR_MESSAGE() AS m; END CATCH;
