SET NOCOUNT ON; SET QUOTED_IDENTIFIER ON;
CREATE DATABASE x094;
GO
USE x094;
GO
-- Vectors whose components are exact in float32 (multiples of 1/64): both sides start from the same bits.
CREATE TABLE dbo.vec (id int PRIMARY KEY, v vector(3) NULL);
WITH n AS (SELECT TOP (2000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.vec SELECT i, CAST(CONCAT('[', (i * 37 % 257 - 128) / 64.0, ',', (i * 91 % 263 - 131) / 64.0, ',', (i * 53 % 251 - 125) / 64.0, ']') AS vector(3)) FROM n;
INSERT INTO dbo.vec VALUES (100001, CAST('[0,0,0]' AS vector(3))), (100002, NULL);
-- Points on a 0..1000 grid, lines and polygons, one empty, one NULL, one invalid (a bow tie).
CREATE TABLE dbo.geo (id int PRIMARY KEY, g geometry NULL);
WITH n AS (SELECT TOP (100000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.geo SELECT i, geometry::Point((i * 7919 % 100000) / 100.0, (i * 104729 % 100000) / 100.0, 0) FROM n;
INSERT INTO dbo.geo VALUES
  (200001, geometry::STGeomFromText('LINESTRING(100 100, 200 300)', 0)),
  (200002, geometry::STGeomFromText('POLYGON((400 400, 500 400, 500 500, 400 500, 400 400))', 0)),
  (200003, geometry::STGeomFromText('POINT EMPTY', 0)),
  (200004, NULL),
  (200005, geometry::STGeomFromText('POLYGON((0 0, 10 10, 10 0, 0 10, 0 0))', 0)),
  (200006, geometry::STGeomFromText('LINESTRING(0 500, 1000 501)', 0)),
  (200007, geometry::STGeomFromText('CIRCULARSTRING(300 300, 310 310, 320 300)', 0));
CREATE SPATIAL INDEX six_geo ON dbo.geo (g) USING GEOMETRY_AUTO_GRID WITH (BOUNDING_BOX = (0, 0, 1000, 1000));
-- geography: points near a high-latitude box edge.
CREATE TABLE dbo.geog (id int PRIMARY KEY, g geography NULL);
INSERT INTO dbo.geog VALUES (1, geography::Point(70.0, 10.0, 4326)), (2, geography::Point(70.5, 15.0, 4326)),
  (3, geography::Point(70.9, 20.0, 4326)), (4, geography::Point(71.2, 15.0, 4326)), (5, geography::Point(69.9, 15.0, 4326));
GO
