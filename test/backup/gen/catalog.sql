-- Spec 085 PR 2b: the catalog fixture, for the oracle lane and local development only (the committed
-- samples are samples.sql's and stay as they are). A database of the shapes the catalog reader must
-- name: several schemas, the common types under several collations, a heap with a nonclustered index,
-- a clustered primary key, a clustered columnstore, an empty table, a dropped column, columns added
-- after rows were written (one NOT NULL with a default: a metadata-only add), a column whose type was
-- altered, identities, computed columns (one persisted), a rowversion, a ROWGUIDCOL, an alias type,
-- sparse columns and a column set, a non-unique clustered index, defaults and a check, and views: two
-- WITH ENCRYPTION, one SCHEMABINDING, one with non-ASCII text, two whose definitions pass 8000 bytes
-- (single-byte and Cyrillic). Every row is generated; nothing comes from
-- anyone's database (spec 085 T4). Run by generate.sh with the sqlcmd variable DIR.
--
-- After the backup, generate.sh writes the server's own catalog of the database beside it
-- (catalog_golden.sql: catalog_full.tables.txt, .columns.txt, .views.txt).
SET NOCOUNT ON;
-- sqlcmd starts with QUOTED_IDENTIFIER off (-I turns it on); an index over a computed column needs it on.
SET QUOTED_IDENTIFIER ON;
GO
IF DB_ID('bak_catalog') IS NOT NULL DROP DATABASE bak_catalog;
GO
-- A data file past one GAM interval (511,232 pages, E17): 4100 MB is 524,800 pages, so opening the
-- backup reads the second interval's GAM (at 511232) and SGAM; the backup copies only the allocated
-- extents, so it stays small.
CREATE DATABASE bak_catalog ON (NAME = bak_catalog, FILENAME = '/var/opt/mssql/data/bak_catalog.mdf', SIZE = 4100MB)
	LOG ON (NAME = bak_catalog_log, FILENAME = '/var/opt/mssql/data/bak_catalog_log.ldf', SIZE = 64MB)
	COLLATE SQL_Latin1_General_CP1_CI_AS;
ALTER DATABASE bak_catalog SET RECOVERY FULL;
GO
USE bak_catalog;
GO
EXEC ('CREATE SCHEMA sales');
EXEC ('CREATE SCHEMA hr');
GO
CREATE TABLE sales.orders (
	order_id int NOT NULL IDENTITY(1, 1) PRIMARY KEY,
	customer nvarchar(100) COLLATE Cyrillic_General_CI_AS NOT NULL,
	code varchar(20) COLLATE Latin1_General_CS_AS NULL,
	note_sc nvarchar(200) COLLATE Latin1_General_100_CI_AS_SC NULL,
	amount decimal(18, 4) NOT NULL,
	fee money NULL,
	small_fee smallmoney NULL,
	placed datetime2(7) NOT NULL,
	placed_at datetimeoffset(3) NULL,
	day date NULL,
	at_time time(3) NULL,
	legacy_dt datetime NULL,
	legacy_sdt smalldatetime NULL,
	ref uniqueidentifier NULL,
	payload varbinary(max) NULL,
	fixed_bin binary(8) NULL,
	flag bit NOT NULL,
	tiny tinyint NULL,
	small smallint NULL,
	big bigint NULL,
	ratio real NULL,
	score float NULL,
	padded char(10) NULL,
	npadded nchar(5) NULL,
	doc xml NULL,
	total AS (amount + ISNULL(fee, 0)),
	version rowversion
);
CREATE TABLE hr.employees (	-- a heap, with a nonclustered index
	employee_id bigint NOT NULL,
	name nvarchar(80) NOT NULL,
	dept char(4) NOT NULL
);
CREATE NONCLUSTERED INDEX ix_employees_dept ON hr.employees (dept);
CREATE TABLE dbo.facts (	-- a clustered columnstore
	fact_id int NOT NULL,
	value float NOT NULL,
	label varchar(30) NULL
);
CREATE CLUSTERED COLUMNSTORE INDEX cci_facts ON dbo.facts;
CREATE TABLE dbo.empty_table (id int NOT NULL PRIMARY KEY, v varchar(10) NULL);
CREATE TABLE dbo.evolved (	-- a column dropped and two added after rows were written
	id int NOT NULL PRIMARY KEY,
	keep_me varchar(20) NOT NULL,
	drop_me int NULL
);
GO
-- A UTF-8 collation exists from 2019 (major 15).
IF CAST(SERVERPROPERTY('ProductMajorVersion') AS int) >= 15
	EXEC ('CREATE TABLE dbo.utf8_text (id int NOT NULL PRIMARY KEY, txt varchar(100) COLLATE Latin1_General_100_CI_AS_SC_UTF8 NULL)');
GO
WITH n AS (SELECT TOP (300) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO sales.orders (customer, code, note_sc, amount, fee, small_fee, placed, placed_at, day, at_time, legacy_dt,
						  legacy_sdt, ref, payload, fixed_bin, flag, tiny, small, big, ratio, score, padded, npadded, doc)
SELECT CONCAT(N'Клиент ', i), CONCAT('C', i), CONCAT(N'note ', i), i * 1.5, i * 0.25, i % 100, DATEADD(SECOND, i, '2026-01-01'),
	   DATEADD(MINUTE, i, CAST('2026-01-01T00:00:00+03:00' AS datetimeoffset(3))), DATEADD(DAY, i, '2026-01-01'),
	   DATEADD(MILLISECOND, i, CAST('10:00' AS time(3))), DATEADD(HOUR, i, '2025-01-01'), DATEADD(HOUR, i, '2025-01-01'),
	   NEWID(), CAST(REPLICATE(CAST('x' AS varchar(max)), i * 10) AS varbinary(max)), CAST(i AS binary(8)), i % 2, i % 256,
	   i, i * 1000000, i / 3.0, i / 7.0, CONCAT('p', i), CONCAT(N'n', i % 10), CAST(CONCAT('<r i="', i, '"/>') AS xml)
FROM n;
WITH n AS (SELECT TOP (500) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO hr.employees (employee_id, name, dept) SELECT i, CONCAT(N'employee ', i), CONCAT('D', i % 10) FROM n;
WITH n AS (SELECT TOP (1000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.facts (fact_id, value, label) SELECT i, i * 1.25, CONCAT('L', i % 50) FROM n;
WITH n AS (SELECT TOP (200) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.evolved (id, keep_me, drop_me) SELECT i, CONCAT('k', i), i FROM n;
GO
ALTER TABLE dbo.evolved DROP COLUMN drop_me;
ALTER TABLE dbo.evolved ADD added_default int NOT NULL CONSTRAINT df_evolved_added DEFAULT 42;
ALTER TABLE dbo.evolved ADD added_null nvarchar(10) NULL;
-- More metadata-only adds: their defaults are sql_variant values of each form (sysseobjvalues).
ALTER TABLE dbo.evolved ADD added_text varchar(10) NOT NULL CONSTRAINT df_evolved_text DEFAULT 'abc';
ALTER TABLE dbo.evolved ADD added_ntext nvarchar(10) NOT NULL CONSTRAINT df_evolved_ntext DEFAULT N'жж';
ALTER TABLE dbo.evolved ADD added_dec decimal(9, 2) NOT NULL CONSTRAINT df_evolved_dec DEFAULT 1.25;
ALTER TABLE dbo.evolved ADD added_dt2 datetime2(3) NOT NULL CONSTRAINT df_evolved_dt2 DEFAULT '2026-01-02T03:04:05.678';
ALTER TABLE dbo.evolved ADD added_bin varbinary(8) NOT NULL CONSTRAINT df_evolved_bin DEFAULT 0x0102;
GO
IF OBJECT_ID('dbo.utf8_text') IS NOT NULL
	EXEC ('INSERT INTO dbo.utf8_text (id, txt) SELECT TOP (50) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)), N''текст'' FROM sys.all_objects');
GO
-- More column shapes: an alias type, an identity of another seed and step, a ROWGUIDCOL, a named
-- default and a check, a persisted computed column, sparse columns; a non-unique clustered index
-- (a uniquifier) whose key does not start at the first column.
CREATE TYPE dbo.code_t FROM varchar(12) NOT NULL;
GO
CREATE TABLE dbo.shapes (
	id bigint NOT NULL IDENTITY(100, 5),
	guid uniqueidentifier ROWGUIDCOL NOT NULL CONSTRAINT df_shapes_guid DEFAULT NEWSEQUENTIALID(),
	code dbo.code_t,
	qty int NOT NULL CONSTRAINT df_shapes_qty DEFAULT 1 CONSTRAINT ck_shapes_qty CHECK (qty >= 0),
	price decimal(10, 2) NOT NULL,
	total AS (qty * price) PERSISTED,
	sp1 int SPARSE NULL,
	sp2 nvarchar(20) SPARSE NULL
);
CREATE CLUSTERED INDEX cx_shapes ON dbo.shapes (qty, code);
-- Sparse columns behind a column set.
CREATE TABLE dbo.colset (id int NOT NULL PRIMARY KEY, a int SPARSE NULL, b varchar(10) SPARSE NULL,
						 cs xml COLUMN_SET FOR ALL_SPARSE_COLUMNS);
-- A column whose type is changed after rows were written (a new physical column), then one added.
CREATE TABLE dbo.altered (id int NOT NULL PRIMARY KEY, v int NOT NULL);
GO
WITH n AS (SELECT TOP (100) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.shapes (code, qty, price, sp1, sp2)
SELECT CONCAT('S', i % 7), i % 5, i * 2.5, CASE WHEN i % 3 = 0 THEN i END, CASE WHEN i % 4 = 0 THEN CONCAT(N'с', i) END FROM n;
WITH n AS (SELECT TOP (40) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.colset (id, a, b) SELECT i, CASE WHEN i % 2 = 0 THEN i END, CASE WHEN i % 3 = 0 THEN CONCAT('b', i) END FROM n;
WITH n AS (SELECT TOP (60) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects)
INSERT INTO dbo.altered (id, v) SELECT i, i * 10 FROM n;
GO
ALTER TABLE dbo.altered ALTER COLUMN v bigint NOT NULL;
ALTER TABLE dbo.altered ADD w int NULL;
GO
CREATE VIEW sales.v_big_orders AS SELECT order_id, customer, amount FROM sales.orders WHERE amount > 100;
GO
CREATE VIEW dbo.v_hidden WITH ENCRYPTION AS SELECT id FROM dbo.empty_table;
GO
CREATE VIEW dbo.v_hidden2 WITH ENCRYPTION AS SELECT id, qty FROM dbo.shapes;
GO
CREATE VIEW dbo.v_bound WITH SCHEMABINDING AS SELECT id, qty FROM dbo.shapes;
GO
CREATE VIEW dbo.v_unicode AS SELECT N'Клиент 字 ü' AS label, id FROM dbo.shapes;
GO
-- Text in Latin-1's range only, and with a character code page 1252 has and Latin-1 has not: which
-- single-byte form a definition is stored in (sysobjvalues.value 0x2), if any.
CREATE VIEW dbo.v_latin1 AS SELECT N'café ü' AS label, id FROM dbo.shapes;
GO
CREATE VIEW dbo.v_cp1252 AS SELECT N'price €' AS label, id FROM dbo.shapes;
GO
-- Definitions past 8000 bytes: one of single-byte characters, one of Cyrillic.
DECLARE @x nvarchar(max) = REPLICATE(CAST(N'x' AS nvarchar(max)), 9000);
EXEC (N'CREATE VIEW dbo.v_long AS SELECT id /* ' + @x + N' */ FROM dbo.shapes');
DECLARE @y nvarchar(max) = REPLICATE(CAST(N'ж' AS nvarchar(max)), 5000);
EXEC (N'CREATE VIEW dbo.v_long_unicode AS SELECT id /* ' + @y + N' */ FROM dbo.shapes');
GO
BACKUP DATABASE bak_catalog TO DISK = '$(DIR)/catalog_full.bak' WITH FORMAT, INIT, CHECKSUM, NAME = 'catalog_full';
GO
