-- Spec 094, review of #622 (run by x1b_x2b.sh, database x094).
SET NOCOUNT ON; SET QUOTED_IDENTIFIER ON;
-- X2b-1: a diagonal line as the constant of `&&`. A 21 x 21 grid of points over the line's box: DuckDB's
-- `&&` (bbox meets bbox) keeps all 441; only the 21 on the diagonal meet the line itself.
CREATE TABLE dbo.geo2 (id int PRIMARY KEY, g geometry NULL);
WITH n AS (SELECT TOP (441) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) - 1 AS i FROM sys.all_objects)
INSERT INTO dbo.geo2 SELECT i, geometry::Point(100 + (i % 21) * 10, 100 + (i / 21) * 10, 0) FROM n;
-- X2b-2: mixed SRIDs. 100k points of SRID 0 and 1000 of SRID 4326 on 0..1000, an index (SRID-blind
-- grid); the constant is SRID 0.
CREATE TABLE dbo.geosrid (id int PRIMARY KEY, g geometry NULL);
WITH n AS (SELECT TOP (101000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.geosrid SELECT i, geometry::Point((i * 7919 % 100000) / 100.0, (i * 104729 % 100000) / 100.0,
  CASE WHEN i > 100000 THEN 4326 ELSE 0 END) FROM n;
CREATE SPATIAL INDEX six_geosrid ON dbo.geosrid (g) USING GEOMETRY_AUTO_GRID WITH (BOUNDING_BOX = (0, 0, 1000, 1000));
-- X3b: geography points the geodesic box (10..20 E, 70..71 N) and the planar one tell apart: its southern
-- edge bows north of 70 N at 15 E, its northern edge north of 71 N.
CREATE TABLE dbo.geog2 (id int PRIMARY KEY, g geography NULL);
INSERT INTO dbo.geog2 VALUES (1, geography::Point(70.03, 15.0, 4326)), (2, geography::Point(71.05, 15.0, 4326)),
  (3, geography::Point(70.5, 15.0, 4326)), (4, geography::Point(69.9, 15.0, 4326)), (5, geography::Point(70.9, 20.0, 4326));
GO
SET NOCOUNT ON; SET QUOTED_IDENTIFIER ON;
DECLARE @diag geometry = geometry::STGeomFromText('LINESTRING(100 100, 300 300)', 0);
SELECT 'x2b_envelope_vs_constant' AS form, COUNT(*) AS n FROM dbo.geo2 WHERE g.STEnvelope().STIntersects(@diag) = 1;
SELECT 'x2b_envelope_vs_constant_envelope' AS form, COUNT(*) AS n FROM dbo.geo2 WHERE g.STEnvelope().STIntersects(@diag.STEnvelope()) = 1;
SELECT 'x2b_filter_constant_envelope' AS form, COUNT(*) AS n FROM dbo.geo2 WHERE g.Filter(@diag.STEnvelope()) = 1;
DECLARE @box geometry = geometry::STGeomFromText('POLYGON((100 100, 300 100, 300 300, 100 300, 100 100))', 0);
SELECT 'x2b_srid_plain' AS form, COUNT(*) AS n,
  SUM(CASE WHEN g.STSrid = 4326 THEN 1 ELSE 0 END) AS n4326 FROM dbo.geosrid WHERE g.STIntersects(@box) = 1;
SELECT 'x2b_srid_plain_is_null' AS form, COUNT(*) AS n FROM dbo.geosrid WHERE g.STSrid = 4326 AND g.STIntersects(@box) IS NULL;
SELECT 'x2b_srid_per_row' AS form, COUNT(*) AS n,
  SUM(CASE WHEN g.STSrid = 4326 THEN 1 ELSE 0 END) AS n4326 FROM dbo.geosrid
  WHERE g.STIntersects(geometry::STGeomFromWKB(@box.STAsBinary(), g.STSrid)) = 1;
SELECT 'x2b_srid_or_other' AS form, COUNT(*) AS n,
  SUM(CASE WHEN g.STSrid = 4326 THEN 1 ELSE 0 END) AS n4326 FROM dbo.geosrid
  WHERE g.STIntersects(@box) = 1 OR g.STSrid <> 0;
SELECT 'x2b_srid_truth' AS form, COUNT(*) AS n,
  SUM(CASE WHEN g.STSrid = 4326 THEN 1 ELSE 0 END) AS n4326 FROM dbo.geosrid
  WHERE g.STX BETWEEN 100 AND 300 AND g.STY BETWEEN 100 AND 300;
GO
SET NOCOUNT ON; SET QUOTED_IDENTIFIER ON;
DECLARE @gbox geography = geography::STGeomFromText('POLYGON((10 70, 20 70, 20 71, 10 71, 10 70))', 4326);
DECLARE @pbox geometry = geometry::STGeomFromText('POLYGON((10 70, 20 70, 20 71, 10 71, 10 70))', 4326);
SELECT 'x3b_geodesic' AS form, STRING_AGG(CAST(id AS varchar(10)), ',') WITHIN GROUP (ORDER BY id) AS ids
  FROM dbo.geog2 WHERE g.STIntersects(@gbox) = 1;
SELECT 'x3b_planar_on_server' AS form, STRING_AGG(CAST(id AS varchar(10)), ',') WITHIN GROUP (ORDER BY id) AS ids
  FROM dbo.geog2 WHERE geometry::STGeomFromWKB(g.STAsBinary(), 4326).STIntersects(@pbox) = 1;
SELECT 'x3b_planar_envelope_on_server' AS form, STRING_AGG(CAST(id AS varchar(10)), ',') WITHIN GROUP (ORDER BY id) AS ids
  FROM dbo.geog2 WHERE geometry::STGeomFromWKB(g.STAsBinary(), 4326).STEnvelope().STIntersects(@pbox.STEnvelope()) = 1;
GO
