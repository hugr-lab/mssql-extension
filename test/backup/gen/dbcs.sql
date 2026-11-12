-- Spec 085 PR 2d: varchar / char under the four double-byte code pages (932, 936, 949, 950), decoded
-- on the client (a backup has no server to convert them), for the oracle lane and local development
-- only. One row per 64 BMP code points the collation weighs, converted by the server into each page
-- (best fit or '?' where it has no mapping), and a char(10) beside it with its padding.
SET NOCOUNT ON;
SET QUOTED_IDENTIFIER ON;
GO
IF DB_ID('bak_dbcs') IS NOT NULL DROP DATABASE bak_dbcs;
GO
CREATE DATABASE bak_dbcs;
GO
USE bak_dbcs;
GO
-- One row per 64 BMP code points (U+0020..U+FFFF, surrogates skipped), converted by the server into
-- each page (best fit or '?' where it has no mapping); a char(n) beside it with padding.
DECLARE @t TABLE (k int);
WITH n AS (SELECT TOP (1024) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) - 1 AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO @t SELECT i FROM n;
CREATE TABLE dbo.src (id int PRIMARY KEY, s nvarchar(64) NULL);
INSERT INTO dbo.src
SELECT k, (SELECT CAST(STRING_AGG(NCHAR(c), N'') WITHIN GROUP (ORDER BY c) AS nvarchar(64))
           FROM (SELECT k * 64 + j AS c FROM (SELECT TOP (64) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) - 1 AS j FROM sys.all_objects) q) cc
           WHERE c >= 32 AND c NOT BETWEEN 0xD800 AND 0xDFFF)
FROM @t WHERE k * 64 + 63 >= 32;
DELETE FROM dbo.src WHERE s IS NULL OR s = N'';
CREATE TABLE dbo.ja (id int PRIMARY KEY, v varchar(200) COLLATE Japanese_CI_AS, c char(10) COLLATE Japanese_CI_AS);
CREATE TABLE dbo.zh (id int PRIMARY KEY, v varchar(200) COLLATE Chinese_PRC_CI_AS, c char(10) COLLATE Chinese_PRC_CI_AS);
CREATE TABLE dbo.ko (id int PRIMARY KEY, v varchar(200) COLLATE Korean_Wansung_CI_AS, c char(10) COLLATE Korean_Wansung_CI_AS);
CREATE TABLE dbo.tw (id int PRIMARY KEY, v varchar(200) COLLATE Chinese_Taiwan_Stroke_CI_AS, c char(10) COLLATE Chinese_Taiwan_Stroke_CI_AS);
INSERT INTO dbo.ja SELECT id, s, LEFT(s, 3) FROM dbo.src;
INSERT INTO dbo.zh SELECT id, s, LEFT(s, 3) FROM dbo.src;
INSERT INTO dbo.ko SELECT id, s, LEFT(s, 3) FROM dbo.src;
INSERT INTO dbo.tw SELECT id, s, LEFT(s, 3) FROM dbo.src;
GO
BACKUP DATABASE bak_dbcs TO DISK = '$(DIR)/dbcs_full.bak' WITH FORMAT, INIT, CHECKSUM, NAME = 'dbcs_full';
GO
