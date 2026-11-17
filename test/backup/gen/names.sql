-- Spec 085 PR 4b: names DuckDB holds otherwise than SQL Server -- a case-sensitive database whose
-- tables differ only in case, and names with spaces and quotes -- for the oracle lane and local
-- development only. (Columns that differ only in case, which the backup reader reads as ID_1, wait for
-- TYPE mssql, the oracle's other side, to read them too.)
SET NOCOUNT ON;
SET QUOTED_IDENTIFIER ON;
GO
IF DB_ID('bak_names') IS NOT NULL DROP DATABASE bak_names;
GO
CREATE DATABASE bak_names COLLATE Latin1_General_100_CS_AS;
GO
USE bak_names;
GO
-- Columns with spaces and quotes in their names; a table and columns named as TYPE mssql's quoting test
-- names them (test/sql/catalog/identifier_quoting.test: ] and ' and a dot).
CREATE TABLE dbo.spaced (Id int NOT NULL PRIMARY KEY, [my col] int NULL, [Other "Col"] nvarchar(20) NULL);
INSERT INTO dbo.spaced VALUES (1, 100, N'x'), (2, NULL, NULL), (3, 300, N'жук');
CREATE TABLE dbo.[q]]t'x] ([c]]1] int NOT NULL PRIMARY KEY, [v'] nvarchar(10) NULL, [r.x] int NULL);
INSERT INTO dbo.[q]]t'x] VALUES (1, N'y', 10), (2, NULL, NULL);
-- Tables that differ only in case.
CREATE TABLE dbo.Items (k int NOT NULL PRIMARY KEY, v int NULL);
CREATE TABLE dbo.items (k int NOT NULL PRIMARY KEY, w varchar(5) NULL);
INSERT INTO dbo.Items VALUES (1, 1), (2, 2);
INSERT INTO dbo.items VALUES (1, 'a');
-- A schema and a table with spaces in their names.
GO
CREATE SCHEMA [my schema];
GO
CREATE TABLE [my schema].[my table] ([a b] int NOT NULL PRIMARY KEY, [c.d] int NULL);
INSERT INTO [my schema].[my table] VALUES (1, 2);
GO
BACKUP DATABASE bak_names TO DISK = '$(DIR)/names_full.bak' WITH FORMAT, INIT, CHECKSUM, NAME = 'names_full';
GO
