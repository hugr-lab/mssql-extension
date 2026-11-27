-- Spec 093 T8: the CLR system types in a backup -- geometry, geography, hierarchyid -- read by the TDS
-- path's kernels (backup_type_forms), in every storage form: a clustered table, ROW- and PAGE-compressed,
-- a heap, and values stored off row. The spatial values are T5's (test/sql/types/spatial_read.test: every
-- OGC type, EMPTY, Z / M, curves, an invalid polygon, FullGlobe), the hierarchyid ones T4's forms. Run by
-- default, so the oracle lane compares them with a RESTORE through TYPE mssql on every version; 2025's
-- committed under test/data/backup/clr_types (made by its generate.sh), with clr_types_changes.sql's differential
-- (clr_types_over: the change listing on two states). Compressed (MS_XPRESS): small.
-- Synthetic values only (T4).
SET NOCOUNT ON;
SET QUOTED_IDENTIFIER ON;
GO
IF DB_ID('bak_clr_types') IS NOT NULL DROP DATABASE bak_clr_types;
GO
CREATE DATABASE bak_clr_types;
GO
USE bak_clr_types;
GO
CREATE TABLE dbo.shapes (id int PRIMARY KEY, name varchar(40) NOT NULL, g geometry NULL);
INSERT INTO dbo.shapes (id, name, g) VALUES
	(1, 'point', geometry::STGeomFromText('POINT(1 2)', 0)),
	(2, 'point_neg', geometry::STGeomFromText('POINT(-1.5 2.25)', 0)),
	(3, 'point_empty', geometry::STGeomFromText('POINT EMPTY', 0)),
	(4, 'point_z', geometry::STGeomFromText('POINT(1 2 3)', 0)),
	(5, 'point_zm', geometry::STGeomFromText('POINT(1 2 3 4)', 0)),
	(6, 'point_m', geometry::STGeomFromText('POINT(1 2 NULL 4)', 0)),
	(7, 'point_3857', geometry::STGeomFromText('POINT(1000 2000)', 3857)),
	(8, 'line', geometry::STGeomFromText('LINESTRING(0 0, 1 1, 2 0)', 0)),
	(9, 'line_empty', geometry::STGeomFromText('LINESTRING EMPTY', 0)),
	(10, 'line_z', geometry::STGeomFromText('LINESTRING(0 0 1, 1 1 2)', 0)),
	(11, 'line_m', geometry::STGeomFromText('LINESTRING(0 0 NULL 5, 1 1 NULL 6)', 0)),
	(12, 'line_zm', geometry::STGeomFromText('LINESTRING(0 0 1 5, 1 1 2 6)', 0)),
	(13, 'line_zpartial', geometry::STGeomFromText('LINESTRING(0 0 1, 1 1 NULL)', 0)),
	(14, 'poly', geometry::STGeomFromText('POLYGON((0 0, 10 0, 10 10, 0 10, 0 0))', 0)),
	(15, 'poly_hole', geometry::STGeomFromText('POLYGON((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 2 4, 4 4, 4 2, 2 2))', 0)),
	(16, 'poly_empty', geometry::STGeomFromText('POLYGON EMPTY', 0)),
	(17, 'poly_cw', geometry::STGeomFromText('POLYGON((0 0, 0 10, 10 10, 10 0, 0 0))', 0)),
	(18, 'mpoint', geometry::STGeomFromText('MULTIPOINT((1 2), (3 4))', 0)),
	(19, 'mpoint_empty', geometry::STGeomFromText('MULTIPOINT EMPTY', 0)),
	(20, 'mpoint_with_empty', geometry::STGeomFromText('MULTIPOINT((1 2), EMPTY)', 0)),
	(21, 'mline', geometry::STGeomFromText('MULTILINESTRING((0 0, 1 1), (2 2, 3 3, 4 4))', 0)),
	(22, 'mline_empty', geometry::STGeomFromText('MULTILINESTRING EMPTY', 0)),
	(23, 'mpoly', geometry::STGeomFromText('MULTIPOLYGON(((0 0, 1 0, 1 1, 0 0)), ((5 5, 9 5, 9 9, 5 9, 5 5), (6 6, 6 7, 7 7, 6 6)))', 0)),
	(24, 'mpoly_empty', geometry::STGeomFromText('MULTIPOLYGON EMPTY', 0)),
	(25, 'gc', geometry::STGeomFromText('GEOMETRYCOLLECTION(POINT(1 2), LINESTRING(0 0, 1 1), POLYGON((0 0, 1 0, 1 1, 0 0)))', 0)),
	(26, 'gc_nested', geometry::STGeomFromText('GEOMETRYCOLLECTION(POINT(1 2), GEOMETRYCOLLECTION(POINT(3 4), LINESTRING(0 0, 5 5)), MULTIPOINT((7 8), (9 10)))', 0)),
	(27, 'gc_empty', geometry::STGeomFromText('GEOMETRYCOLLECTION EMPTY', 0)),
	(28, 'gc_with_empty', geometry::STGeomFromText('GEOMETRYCOLLECTION(POINT EMPTY, POINT(1 2))', 0)),
	(29, 'gc_z', geometry::STGeomFromText('GEOMETRYCOLLECTION(POINT(1 2 3), LINESTRING(0 0 1, 1 1 2))', 0)),
	(30, 'circ', geometry::STGeomFromText('CIRCULARSTRING(0 0, 1 1, 2 0)', 0)),
	(31, 'circ_empty', geometry::STGeomFromText('CIRCULARSTRING EMPTY', 0)),
	(32, 'compound', geometry::STGeomFromText('COMPOUNDCURVE((0 0, 1 1), CIRCULARSTRING(1 1, 2 2, 3 1))', 0)),
	(33, 'curvepoly', geometry::STGeomFromText('CURVEPOLYGON(CIRCULARSTRING(0 0, 4 0, 4 4, 0 4, 0 0))', 0)),
	(34, 'gc_curve', geometry::STGeomFromText('GEOMETRYCOLLECTION(CIRCULARSTRING(0 0, 1 1, 2 0), POINT(5 5))', 0)),
	(35, 'bowtie_invalid', geometry::STGeomFromText('POLYGON((0 0, 2 2, 2 0, 0 2, 0 0))', 0)),
	(36, 'mline_with_empty', geometry::STGeomFromText('MULTILINESTRING((0 0, 1 1), EMPTY)', 0)),
	(37, 'mpoly_with_empty', geometry::STGeomFromText('MULTIPOLYGON(((0 0, 1 0, 1 1, 0 0)), EMPTY)', 0)),
	(38, 'gc_gc_empty', geometry::STGeomFromText('GEOMETRYCOLLECTION(GEOMETRYCOLLECTION EMPTY, POINT(1 1))', 0)),
	(39, 'gc_curvepoly', geometry::STGeomFromText('GEOMETRYCOLLECTION(CURVEPOLYGON(COMPOUNDCURVE(CIRCULARSTRING(0 0, 2 2, 4 0), (4 0, 0 0))), LINESTRING(9 9, 8 8))', 0)),
	(40, 'curvepoly_hole', geometry::STGeomFromText('CURVEPOLYGON(CIRCULARSTRING(0 0, 8 0, 8 8, 0 8, 0 0), (2 2, 4 2, 4 4, 2 2))', 0)),
	(41, 'poly_z_holes', geometry::STGeomFromText('POLYGON((0 0 1, 10 0 1, 10 10 1, 0 10 1, 0 0 1), (1 1 2, 2 1 2, 2 2 2, 1 1 2), (5 5 3, 6 5 3, 6 6 3, 5 5 3))', 0)),
	(42, 'mpoint_z_empty', geometry::STGeomFromText('MULTIPOINT((1 2 3), EMPTY, (4 5 6))', 0)),
	(43, 'big_coords', geometry::STGeomFromText('POINT(1.7976931348623157E+308 -1.7976931348623157E+308)', 0)),
	(44, 'neg_zero', geometry::STGeomFromText('POINT(-0 0)', 0)),
	(45, 'gc_mpoly_in_gc', geometry::STGeomFromText('GEOMETRYCOLLECTION(MULTIPOLYGON(((0 0, 1 0, 1 1, 0 0)), ((3 3, 4 3, 4 4, 3 3))), MULTILINESTRING((0 0, 1 1)))', 0));
-- A value stored off row: a line of 2000 points (about 32 KB).
INSERT INTO dbo.shapes (id, name, g) SELECT 100, 'line_2000', geometry::STGeomFromText(CONCAT('LINESTRING(',
	STRING_AGG(CAST(CONCAT(n, ' ', n % 7) AS varchar(max)), ', ') WITHIN GROUP (ORDER BY n), ')'), 0)
FROM (SELECT TOP (2000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS n FROM sys.all_objects a CROSS JOIN sys.all_objects b) x;
INSERT INTO dbo.shapes (id, name, g) VALUES (101, 'null', NULL);
CREATE TABLE dbo.places (id int PRIMARY KEY, name varchar(40) NOT NULL, g geography NULL);
INSERT INTO dbo.places (id, name, g) VALUES
	(1, 'g_point', geography::STGeomFromText('POINT(10 50)', 4326)),
	(2, 'g_point_z', geography::STGeomFromText('POINT(10 50 100)', 4326)),
	(3, 'g_point_empty', geography::STGeomFromText('POINT EMPTY', 4326)),
	(4, 'g_line', geography::STGeomFromText('LINESTRING(10 50, 11 51)', 4326)),
	(5, 'g_poly', geography::STGeomFromText('POLYGON((0 0, 10 0, 10 10, 0 10, 0 0))', 4326)),
	(6, 'g_poly_big', geography::STGeomFromText('POLYGON((0 0, 0 10, 10 10, 10 0, 0 0))', 4326)),
	(7, 'g_mpoly', geography::STGeomFromText('MULTIPOLYGON(((0 0, 1 0, 1 1, 0 0)), ((5 5, 9 5, 9 9, 5 9, 5 5)))', 4326)),
	(8, 'g_gc', geography::STGeomFromText('GEOMETRYCOLLECTION(POINT(10 50), LINESTRING(10 50, 11 51))', 4326)),
	(9, 'g_fullglobe', geography::STGeomFromText('FULLGLOBE', 4326)),
	(10, 'g_circ', geography::STGeomFromText('CIRCULARSTRING(0 0, 1 1, 2 0)', 4326)),
	(11, 'g_srid4269', geography::STGeomFromText('POINT(10 50)', 4269)),
	(12, 'g_point_api', geography::Point(50, 10, 4326)),
	(13, 'g_poly_hole', geography::STGeomFromText('POLYGON((0 0, 10 0, 10 10, 0 10, 0 0), (2 2, 2 4, 4 4, 4 2, 2 2))', 4326)),
	(14, 'g_point_m', geography::STGeomFromText('POINT(10 50 NULL 7)', 4326)),
	(15, 'g_line_zm', geography::STGeomFromText('LINESTRING(10 50 1 2, 11 51 3 4)', 4326)),
	(16, 'g_mpoly_big', geography::STGeomFromText('MULTIPOLYGON(((0 0, 0 10, 10 10, 10 0, 0 0)))', 4326)),
	(17, 'g_compound', geography::STGeomFromText('COMPOUNDCURVE((0 0, 1 1), CIRCULARSTRING(1 1, 2 2, 3 1))', 4326)),
	(18, 'g_curvepoly', geography::STGeomFromText('CURVEPOLYGON(CIRCULARSTRING(0 0, 4 0, 4 4, 0 4, 0 0))', 4326)),
	(19, 'g_unit_sphere', geography::STGeomFromText('POINT(10 50)', 104001));
INSERT INTO dbo.places (id, name, g) VALUES (101, 'null', NULL);
CREATE TABLE dbo.nodes (id int PRIMARY KEY, h hierarchyid NULL);
INSERT INTO dbo.nodes (id, h) VALUES
	(1, hierarchyid::GetRoot()), (2, '/1/'), (3, '/1/2/'), (4, '/-1/'), (5, '/1.5/'), (6, '/1.-2/'),
	(7, '/0/'), (8, '/3/'), (9, '/4/'), (10, '/7/'), (11, '/8/'), (12, '/15/'), (13, '/16/'), (14, '/79/'),
	(15, '/80/'), (16, '/1103/'), (17, '/1104/'), (18, '/5199/'), (19, '/5200/'), (20, '/-8/'), (21, '/-9/'),
	(22, '/-72/'), (23, '/-73/'), (24, '/-4168/'), (25, '/-4169/'),
	(26, '/1/2/3/4/5/6/7/8/9/10/11/12/13/14/15/16/17/18/19/20/'), (27, '/2.1.1/'), (28, NULL);
-- The three types side by side, in every storage form: clustered, ROW, PAGE, and a heap.
CREATE TABLE dbo.mixed_plain (id int PRIMARY KEY, g geometry NULL, p geography NULL, h hierarchyid NULL);
CREATE TABLE dbo.mixed_row (id int PRIMARY KEY, g geometry NULL, p geography NULL, h hierarchyid NULL)
	WITH (DATA_COMPRESSION = ROW);
CREATE TABLE dbo.mixed_page (id int PRIMARY KEY, g geometry NULL, p geography NULL, h hierarchyid NULL)
	WITH (DATA_COMPRESSION = PAGE);
CREATE TABLE dbo.mixed_heap (id int NOT NULL, g geometry NULL, p geography NULL, h hierarchyid NULL);
INSERT INTO dbo.mixed_plain SELECT s.id, s.g, p.g, n.h FROM dbo.shapes s
	LEFT JOIN dbo.places p ON p.id = s.id LEFT JOIN dbo.nodes n ON n.id = s.id;
INSERT INTO dbo.mixed_row SELECT * FROM dbo.mixed_plain;
INSERT INTO dbo.mixed_page SELECT * FROM dbo.mixed_plain;
INSERT INTO dbo.mixed_heap SELECT * FROM dbo.mixed_plain;
ALTER TABLE dbo.mixed_page REBUILD;
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_clr_types TO DISK = '$(DIR)/clr_types_full.bak' WITH FORMAT, INIT, CHECKSUM, COMPRESSION,
	NAME = 'clr_types_full';
GO
