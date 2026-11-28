SET NOCOUNT ON; SET QUOTED_IDENTIFIER ON;
DECLARE @box geometry = geometry::STGeomFromText('POLYGON((100 100, 300 100, 300 300, 100 300, 100 100))', 0);
DECLARE @far geometry = geometry::STGeomFromText('POLYGON((5 5, 20 5, 20 20, 5 20, 5 5))', 0);
SET STATISTICS XML ON;
BEGIN TRY SELECT 'stintersects' AS form, COUNT(*) AS n FROM dbo.geo WHERE g.STIntersects(@box) = 1; END TRY BEGIN CATCH SELECT 'stintersects' AS form, -ERROR_NUMBER() AS n; END CATCH;
SET STATISTICS XML OFF;
GO
SET NOCOUNT ON; SET QUOTED_IDENTIFIER ON;
DECLARE @box geometry = geometry::STGeomFromText('POLYGON((100 100, 300 100, 300 300, 100 300, 100 100))', 0);
DECLARE @far geometry = geometry::STGeomFromText('POLYGON((5 5, 20 5, 20 20, 5 20, 5 5))', 0);
BEGIN TRY SELECT 'stintersects_on_invalid' AS form, COUNT(*) AS n FROM dbo.geo WHERE id = 200005 AND g.STIntersects(@far) = 1; END TRY BEGIN CATCH SELECT 'stintersects_on_invalid' AS form, -ERROR_NUMBER() AS n; END CATCH;
GO
SET NOCOUNT ON; SET QUOTED_IDENTIFIER ON;
DECLARE @box geometry = geometry::STGeomFromText('POLYGON((100 100, 300 100, 300 300, 100 300, 100 100))', 0);
DECLARE @far geometry = geometry::STGeomFromText('POLYGON((5 5, 20 5, 20 20, 5 20, 5 5))', 0);
SET STATISTICS XML ON;
BEGIN TRY SELECT 'filter' AS form, COUNT(*) AS n FROM dbo.geo WHERE g.Filter(@box) = 1; END TRY BEGIN CATCH SELECT 'filter' AS form, -ERROR_NUMBER() AS n; END CATCH;
SET STATISTICS XML OFF;
GO
SET NOCOUNT ON; SET QUOTED_IDENTIFIER ON;
DECLARE @box geometry = geometry::STGeomFromText('POLYGON((100 100, 300 100, 300 300, 100 300, 100 100))', 0);
DECLARE @far geometry = geometry::STGeomFromText('POLYGON((5 5, 20 5, 20 20, 5 20, 5 5))', 0);
BEGIN TRY SELECT 'filter_on_invalid' AS form, COUNT(*) AS n FROM dbo.geo WHERE id = 200005 AND g.Filter(@far) = 1; END TRY BEGIN CATCH SELECT 'filter_on_invalid' AS form, -ERROR_NUMBER() AS n; END CATCH;
GO
SET NOCOUNT ON; SET QUOTED_IDENTIFIER ON;
DECLARE @box geometry = geometry::STGeomFromText('POLYGON((100 100, 300 100, 300 300, 100 300, 100 100))', 0);
DECLARE @far geometry = geometry::STGeomFromText('POLYGON((5 5, 20 5, 20 20, 5 20, 5 5))', 0);
SET STATISTICS XML ON;
BEGIN TRY SELECT 'envelope' AS form, COUNT(*) AS n FROM dbo.geo WHERE g.STEnvelope().STIntersects(@box) = 1; END TRY BEGIN CATCH SELECT 'envelope' AS form, -ERROR_NUMBER() AS n; END CATCH;
SET STATISTICS XML OFF;
GO
SET NOCOUNT ON; SET QUOTED_IDENTIFIER ON;
DECLARE @box geometry = geometry::STGeomFromText('POLYGON((100 100, 300 100, 300 300, 100 300, 100 100))', 0);
DECLARE @far geometry = geometry::STGeomFromText('POLYGON((5 5, 20 5, 20 20, 5 20, 5 5))', 0);
BEGIN TRY SELECT 'envelope_on_invalid' AS form, COUNT(*) AS n FROM dbo.geo WHERE id = 200005 AND g.STEnvelope().STIntersects(@far) = 1; END TRY BEGIN CATCH SELECT 'envelope_on_invalid' AS form, -ERROR_NUMBER() AS n; END CATCH;
GO
SET NOCOUNT ON; SET QUOTED_IDENTIFIER ON;
DECLARE @box geometry = geometry::STGeomFromText('POLYGON((100 100, 300 100, 300 300, 100 300, 100 100))', 0);
DECLARE @far geometry = geometry::STGeomFromText('POLYGON((5 5, 20 5, 20 20, 5 20, 5 5))', 0);
SET STATISTICS XML ON;
BEGIN TRY SELECT 'filter_and_envelope' AS form, COUNT(*) AS n FROM dbo.geo WHERE g.Filter(@box) = 1 AND g.STEnvelope().STIntersects(@box) = 1; END TRY BEGIN CATCH SELECT 'filter_and_envelope' AS form, -ERROR_NUMBER() AS n; END CATCH;
SET STATISTICS XML OFF;
GO
SET NOCOUNT ON; SET QUOTED_IDENTIFIER ON;
DECLARE @box geometry = geometry::STGeomFromText('POLYGON((100 100, 300 100, 300 300, 100 300, 100 100))', 0);
DECLARE @far geometry = geometry::STGeomFromText('POLYGON((5 5, 20 5, 20 20, 5 20, 5 5))', 0);
BEGIN TRY SELECT 'filter_and_envelope_on_invalid' AS form, COUNT(*) AS n FROM dbo.geo WHERE id = 200005 AND g.Filter(@far) = 1 AND g.STEnvelope().STIntersects(@far) = 1; END TRY BEGIN CATCH SELECT 'filter_and_envelope_on_invalid' AS form, -ERROR_NUMBER() AS n; END CATCH;
GO
SET NOCOUNT ON; SET QUOTED_IDENTIFIER ON;
DECLARE @box geometry = geometry::STGeomFromText('POLYGON((100 100, 300 100, 300 300, 100 300, 100 100))', 0);
DECLARE @far geometry = geometry::STGeomFromText('POLYGON((5 5, 20 5, 20 20, 5 20, 5 5))', 0);
SET STATISTICS XML ON;
BEGIN TRY SELECT 'reversed' AS form, COUNT(*) AS n FROM dbo.geo WHERE @box.STIntersects(g) = 1; END TRY BEGIN CATCH SELECT 'reversed' AS form, -ERROR_NUMBER() AS n; END CATCH;
SET STATISTICS XML OFF;
GO
SET NOCOUNT ON; SET QUOTED_IDENTIFIER ON;
DECLARE @box geometry = geometry::STGeomFromText('POLYGON((100 100, 300 100, 300 300, 100 300, 100 100))', 0);
DECLARE @far geometry = geometry::STGeomFromText('POLYGON((5 5, 20 5, 20 20, 5 20, 5 5))', 0);
BEGIN TRY SELECT 'reversed_on_invalid' AS form, COUNT(*) AS n FROM dbo.geo WHERE id = 200005 AND @far.STIntersects(g) = 1; END TRY BEGIN CATCH SELECT 'reversed_on_invalid' AS form, -ERROR_NUMBER() AS n; END CATCH;
GO
SET NOCOUNT ON; SET QUOTED_IDENTIFIER ON;
DECLARE @box geometry = geometry::STGeomFromText('POLYGON((100 100, 300 100, 300 300, 100 300, 100 100))', 0);
DECLARE @far geometry = geometry::STGeomFromText('POLYGON((5 5, 20 5, 20 20, 5 20, 5 5))', 0);
SET STATISTICS XML ON;
BEGIN TRY SELECT 'within' AS form, COUNT(*) AS n FROM dbo.geo WHERE g.STWithin(@box) = 1; END TRY BEGIN CATCH SELECT 'within' AS form, -ERROR_NUMBER() AS n; END CATCH;
SET STATISTICS XML OFF;
GO
SET NOCOUNT ON; SET QUOTED_IDENTIFIER ON;
DECLARE @box geometry = geometry::STGeomFromText('POLYGON((100 100, 300 100, 300 300, 100 300, 100 100))', 0);
DECLARE @far geometry = geometry::STGeomFromText('POLYGON((5 5, 20 5, 20 20, 5 20, 5 5))', 0);
BEGIN TRY SELECT 'within_on_invalid' AS form, COUNT(*) AS n FROM dbo.geo WHERE id = 200005 AND g.STWithin(@far) = 1; END TRY BEGIN CATCH SELECT 'within_on_invalid' AS form, -ERROR_NUMBER() AS n; END CATCH;
GO
SET NOCOUNT ON; SET QUOTED_IDENTIFIER ON;
DECLARE @box geometry = geometry::STGeomFromText('POLYGON((100 100, 300 100, 300 300, 100 300, 100 100))', 0);
DECLARE @far geometry = geometry::STGeomFromText('POLYGON((5 5, 20 5, 20 20, 5 20, 5 5))', 0);
SET STATISTICS XML ON;
BEGIN TRY SELECT 'dwithin' AS form, COUNT(*) AS n FROM dbo.geo WHERE g.STDistance(@box) <= 10; END TRY BEGIN CATCH SELECT 'dwithin' AS form, -ERROR_NUMBER() AS n; END CATCH;
SET STATISTICS XML OFF;
GO
SET NOCOUNT ON; SET QUOTED_IDENTIFIER ON;
DECLARE @box geometry = geometry::STGeomFromText('POLYGON((100 100, 300 100, 300 300, 100 300, 100 100))', 0);
DECLARE @far geometry = geometry::STGeomFromText('POLYGON((5 5, 20 5, 20 20, 5 20, 5 5))', 0);
BEGIN TRY SELECT 'dwithin_on_invalid' AS form, COUNT(*) AS n FROM dbo.geo WHERE id = 200005 AND g.STDistance(@far) <= 10; END TRY BEGIN CATCH SELECT 'dwithin_on_invalid' AS form, -ERROR_NUMBER() AS n; END CATCH;
GO
SET NOCOUNT ON; SET QUOTED_IDENTIFIER ON;
DECLARE @box geometry = geometry::STGeomFromText('POLYGON((100 100, 300 100, 300 300, 100 300, 100 100))', 0);
DECLARE @far geometry = geometry::STGeomFromText('POLYGON((5 5, 20 5, 20 20, 5 20, 5 5))', 0);
SET STATISTICS XML ON;
BEGIN TRY SELECT 'two_boxes_or' AS form, COUNT(*) AS n FROM dbo.geo WHERE g.STIntersects(@box) = 1 OR g.STIntersects(@far) = 1; END TRY BEGIN CATCH SELECT 'two_boxes_or' AS form, -ERROR_NUMBER() AS n; END CATCH;
SET STATISTICS XML OFF;
GO
SET NOCOUNT ON; SET QUOTED_IDENTIFIER ON;
DECLARE @box geometry = geometry::STGeomFromText('POLYGON((100 100, 300 100, 300 300, 100 300, 100 100))', 0);
DECLARE @far geometry = geometry::STGeomFromText('POLYGON((5 5, 20 5, 20 20, 5 20, 5 5))', 0);
BEGIN TRY SELECT 'two_boxes_or_on_invalid' AS form, COUNT(*) AS n FROM dbo.geo WHERE id = 200005 AND (g.STIntersects(@box) = 1 OR g.STIntersects(@far) = 1); END TRY BEGIN CATCH SELECT 'two_boxes_or_on_invalid' AS form, -ERROR_NUMBER() AS n; END CATCH;
GO
SET NOCOUNT ON; SET QUOTED_IDENTIFIER ON;
DECLARE @box geometry = geometry::STGeomFromText('POLYGON((100 100, 300 100, 300 300, 100 300, 100 100))', 0);
DECLARE @far geometry = geometry::STGeomFromText('POLYGON((5 5, 20 5, 20 20, 5 20, 5 5))', 0);
SET STATISTICS XML ON;
BEGIN TRY SELECT 'two_boxes_union' AS form, COUNT(*) AS n FROM dbo.geo WHERE g.STIntersects(@box.STUnion(@far)) = 1; END TRY BEGIN CATCH SELECT 'two_boxes_union' AS form, -ERROR_NUMBER() AS n; END CATCH;
SET STATISTICS XML OFF;
GO
SET NOCOUNT ON; SET QUOTED_IDENTIFIER ON;
DECLARE @box geometry = geometry::STGeomFromText('POLYGON((100 100, 300 100, 300 300, 100 300, 100 100))', 0);
DECLARE @far geometry = geometry::STGeomFromText('POLYGON((5 5, 20 5, 20 20, 5 20, 5 5))', 0);
BEGIN TRY SELECT 'two_boxes_union_on_invalid' AS form, COUNT(*) AS n FROM dbo.geo WHERE id = 200005 AND g.STIntersects(@box.STUnion(@far)) = 1; END TRY BEGIN CATCH SELECT 'two_boxes_union_on_invalid' AS form, -ERROR_NUMBER() AS n; END CATCH;
GO
SET NOCOUNT ON; SET QUOTED_IDENTIFIER ON;
DECLARE @box geometry = geometry::STGeomFromText('POLYGON((100 100, 300 100, 300 300, 100 300, 100 100))', 0);
DECLARE @far geometry = geometry::STGeomFromText('POLYGON((5 5, 20 5, 20 20, 5 20, 5 5))', 0);
SET STATISTICS XML ON;
BEGIN TRY SELECT 'valid_guard_case' AS form, COUNT(*) AS n FROM dbo.geo WHERE CASE WHEN g.STIsValid() = 1 THEN g.STEnvelope().STIntersects(@box) END = 1; END TRY BEGIN CATCH SELECT 'valid_guard_case' AS form, -ERROR_NUMBER() AS n; END CATCH;
SET STATISTICS XML OFF;
GO
SET NOCOUNT ON; SET QUOTED_IDENTIFIER ON;
DECLARE @box geometry = geometry::STGeomFromText('POLYGON((100 100, 300 100, 300 300, 100 300, 100 100))', 0);
DECLARE @far geometry = geometry::STGeomFromText('POLYGON((5 5, 20 5, 20 20, 5 20, 5 5))', 0);
BEGIN TRY SELECT 'valid_guard_case_on_invalid' AS form, COUNT(*) AS n FROM dbo.geo WHERE id = 200005 AND CASE WHEN g.STIsValid() = 1 THEN g.STEnvelope().STIntersects(@far) END = 1; END TRY BEGIN CATCH SELECT 'valid_guard_case_on_invalid' AS form, -ERROR_NUMBER() AS n; END CATCH;
GO
SET NOCOUNT ON; SET QUOTED_IDENTIFIER ON;
DECLARE @box geometry = geometry::STGeomFromText('POLYGON((100 100, 300 100, 300 300, 100 300, 100 100))', 0);
DECLARE @far geometry = geometry::STGeomFromText('POLYGON((5 5, 20 5, 20 20, 5 20, 5 5))', 0);
SET STATISTICS XML ON;
BEGIN TRY SELECT 'filter_then_valid_case' AS form, COUNT(*) AS n FROM dbo.geo WHERE g.Filter(@box) = 1 AND CASE WHEN g.STIsValid() = 1 THEN g.STEnvelope().STIntersects(@box) ELSE 1 END = 1; END TRY BEGIN CATCH SELECT 'filter_then_valid_case' AS form, -ERROR_NUMBER() AS n; END CATCH;
SET STATISTICS XML OFF;
GO
SET NOCOUNT ON; SET QUOTED_IDENTIFIER ON;
DECLARE @box geometry = geometry::STGeomFromText('POLYGON((100 100, 300 100, 300 300, 100 300, 100 100))', 0);
DECLARE @far geometry = geometry::STGeomFromText('POLYGON((5 5, 20 5, 20 20, 5 20, 5 5))', 0);
BEGIN TRY SELECT 'filter_then_valid_case_on_invalid' AS form, COUNT(*) AS n FROM dbo.geo WHERE id = 200005 AND g.Filter(@far) = 1 AND CASE WHEN g.STIsValid() = 1 THEN g.STEnvelope().STIntersects(@far) ELSE 1 END = 1; END TRY BEGIN CATCH SELECT 'filter_then_valid_case_on_invalid' AS form, -ERROR_NUMBER() AS n; END CATCH;
GO
