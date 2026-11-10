-- Spec 085 PR 2b: the catalog fixture, for the oracle lane and local development only (the committed
-- samples are samples.sql's and stay as they are). A database of the shapes the catalog reader must
-- name: several schemas, the common types under several collations, a heap with a nonclustered index,
-- a clustered primary key, a clustered columnstore, an empty table, a dropped column, columns added
-- after rows were written (one NOT NULL with a default: a metadata-only add), an identity, a computed
-- column, a rowversion, and two views, one WITH ENCRYPTION. Every row is generated; nothing comes from
-- anyone's database (spec 085 T4). Run by generate.sh with the sqlcmd variable DIR.
--
-- After the backup, the server's own catalog of the database is written beside it
-- (catalog_golden.sql: catalog_full.tables.txt, .columns.txt, .views.txt).
SET NOCOUNT ON;
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
GO
IF OBJECT_ID('dbo.utf8_text') IS NOT NULL
	EXEC ('INSERT INTO dbo.utf8_text (id, txt) SELECT TOP (50) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)), N''текст'' FROM sys.all_objects');
GO
CREATE VIEW sales.v_big_orders AS SELECT order_id, customer, amount FROM sales.orders WHERE amount > 100;
GO
CREATE VIEW dbo.v_hidden WITH ENCRYPTION AS SELECT id FROM dbo.empty_table;
GO
BACKUP DATABASE bak_catalog TO DISK = '$(DIR)/catalog_full.bak' WITH FORMAT, INIT, CHECKSUM, NAME = 'catalog_full';
GO
-- The server's catalog of the database, as the backup holds it (nothing changed since).
:setvar SET catalog_full
:r $(GEN)/catalog_golden.sql
