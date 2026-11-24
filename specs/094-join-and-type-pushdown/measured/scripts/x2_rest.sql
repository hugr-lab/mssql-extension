SET NOCOUNT ON; SET QUOTED_IDENTIFIER ON;
DECLARE @box geometry = geometry::STGeomFromText('POLYGON((100 100, 300 100, 300 300, 100 300, 100 100))', 0);
DECLARE @far geometry = geometry::STGeomFromText('POLYGON((5 5, 20 5, 20 20, 5 20, 5 5))', 0);
SELECT 'edge' AS what, id, g.STIsValid() AS valid, g.STGeometryType() AS t FROM dbo.geo WHERE id > 200000 ORDER BY id;
GO
SET NOCOUNT ON; SET QUOTED_IDENTIFIER ON;
DECLARE @box geometry = geometry::STGeomFromText('POLYGON((100 100, 300 100, 300 300, 100 300, 100 100))', 0);
DECLARE @far geometry = geometry::STGeomFromText('POLYGON((5 5, 20 5, 20 20, 5 20, 5 5))', 0);
SELECT 'row' AS what, id, CONVERT(varchar(max), g.AsBinaryZM(), 2) AS wkb,
  CASE WHEN g.STIsValid() = 1 THEN CASE WHEN g.STEnvelope().STIntersects(@box) = 1 THEN 1 ELSE 0 END END AS env_box,
  CASE WHEN g.STIsValid() = 1 THEN CASE WHEN g.STIntersects(@box) = 1 THEN 1 ELSE 0 END END AS int_box
FROM dbo.geo WHERE id % 50 = 0 OR id > 200000;
GO
DECLARE @gbox geography = geography::STGeomFromText('POLYGON((10 70, 20 70, 20 71, 10 71, 10 70))', 4326);
SELECT 'geog' AS what, id, g.Lat AS lat, g.Long AS lon, g.STIntersects(@gbox) AS geodesic_in FROM dbo.geog ORDER BY id;
GO
