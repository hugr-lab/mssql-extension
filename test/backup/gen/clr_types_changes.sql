-- Spec 093 T8a's follow-up (review of #621): the clr_types fixture's second stage (generate.sh runs it after
-- clr_types.sql): geometry / geography / hierarchyid values changed -- to another shape, to a curve and back, to
-- NULL, grown off row -- rows inserted and deleted, in every storage form, then a differential over the full
-- (clr_types_diff, paired as clr_types_over), which the change listing reads as two states.
SET NOCOUNT ON;
SET QUOTED_IDENTIFIER ON;
GO
USE bak_clr_types;
GO
UPDATE dbo.shapes SET g = geometry::STGeomFromText('POLYGON((0 0, 9 0, 9 9, 0 9, 0 0))', 0) WHERE id = 1;
UPDATE dbo.shapes SET g = geometry::STGeomFromText('CIRCULARSTRING(0 0, 1 1, 2 0)', 0) WHERE id = 2;
UPDATE dbo.shapes SET g = NULL WHERE id = 8;
INSERT INTO dbo.shapes (id, name, g) VALUES (1001, 'added_point', geometry::STGeomFromText('POINT(7 8)', 4326)),
	(1002, 'added_curve', geometry::STGeomFromText('COMPOUNDCURVE((0 0, 1 1), CIRCULARSTRING(1 1, 2 2, 3 1))', 0));
UPDATE dbo.places SET g = geography::STGeomFromText('POINT(30 60)', 4326) WHERE id = 1;
UPDATE dbo.nodes SET h = hierarchyid::Parse('/9/8/7/') WHERE id = 2;
UPDATE dbo.nodes SET h = NULL WHERE id = 3;
INSERT INTO dbo.nodes (id, h) VALUES (1001, hierarchyid::Parse('/1.5/-2/'));
GO
-- The four mixed tables changed alike (mixed_row stays the twin of mixed_plain, compare_rows' check), one of
-- their geometries grown past the row: its value off row from here on.
DECLARE @big nvarchar(max) = N'LINESTRING(0 0';
DECLARE @i int = 1;
WHILE @i <= 1200 BEGIN SET @big += CONCAT(N', ', @i, N' ', @i % 17); SET @i += 1; END
SET @big += N')';
DECLARE @g geometry = geometry::STGeomFromText(@big, 0);
UPDATE dbo.mixed_plain SET g = NULL, h = hierarchyid::Parse('/42/') WHERE id % 5 = 0;
UPDATE dbo.mixed_row SET g = NULL, h = hierarchyid::Parse('/42/') WHERE id % 5 = 0;
UPDATE dbo.mixed_page SET g = NULL, h = hierarchyid::Parse('/42/') WHERE id % 5 = 0;
UPDATE dbo.mixed_heap SET g = NULL, h = hierarchyid::Parse('/42/') WHERE id % 5 = 0;
UPDATE dbo.mixed_plain SET p = NULL WHERE id % 4 = 1;
UPDATE dbo.mixed_row SET p = NULL WHERE id % 4 = 1;
UPDATE dbo.mixed_page SET p = NULL WHERE id % 4 = 1;
UPDATE dbo.mixed_heap SET p = NULL WHERE id % 4 = 1;
UPDATE dbo.mixed_plain SET g = geometry::STGeomFromText('LINESTRING(1 1, 2 2)', 0) WHERE id % 3 = 0;
UPDATE dbo.mixed_row SET g = geometry::STGeomFromText('LINESTRING(1 1, 2 2)', 0) WHERE id % 3 = 0;
UPDATE dbo.mixed_page SET g = geometry::STGeomFromText('LINESTRING(1 1, 2 2)', 0) WHERE id % 3 = 0;
UPDATE dbo.mixed_heap SET g = geometry::STGeomFromText('LINESTRING(1 1, 2 2)', 0) WHERE id % 3 = 0;
UPDATE dbo.mixed_plain SET g = @g WHERE id = 1;
UPDATE dbo.mixed_row SET g = @g WHERE id = 1;
UPDATE dbo.mixed_page SET g = @g WHERE id = 1;
UPDATE dbo.mixed_heap SET g = @g WHERE id = 1;
DELETE FROM dbo.mixed_plain WHERE id IN (4, 6);
DELETE FROM dbo.mixed_row WHERE id IN (4, 6);
DELETE FROM dbo.mixed_page WHERE id IN (4, 6);
DELETE FROM dbo.mixed_heap WHERE id IN (4, 6);
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_clr_types TO DISK = '$(DIR)/clr_types_diff.bak' WITH FORMAT, INIT, CHECKSUM, COMPRESSION, DIFFERENTIAL,
	NAME = 'clr_types_diff';
GO
