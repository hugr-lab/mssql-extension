-- Spec 085 PR 2c: the partitioning fixture, for the oracle lane and local development. E7's structure
-- database (specs/085-backup-reader-core/research/experiments/E7-partitioning.md, scripts a00-a03): 19
-- partition functions of the integer, date, datetime, datetime2, datetimeoffset and string families, a
-- table on each, rows on every boundary edge (b - tick, b, b + tick) plus a spread, schemes over three
-- filegroups, a clustered columnstore, aligned and non-aligned nonclustered indexes, a partition
-- rebuilt WITH ROW compression -- and what E7 did not cover: decimal, uniqueidentifier, varbinary,
-- time, smalldatetime, tinyint, money, bit, real, float, binary, char, nchar and datetimeoffset(7)
-- boundaries, a SPLIT and a MERGE (boundary ids renumbered), and NEXT
-- USED on a scheme given as a list. Every row is generated (spec 085 T4). Run by generate.sh with the
-- sqlcmd variable DIR; generate.sh writes the server's own catalog beside the backup (catalog_golden.sql).
SET NOCOUNT ON;
SET QUOTED_IDENTIFIER ON;
GO
IF DB_ID('bak_partition') IS NOT NULL DROP DATABASE bak_partition;
GO
CREATE DATABASE bak_partition
	ON PRIMARY (NAME = bak_partition, FILENAME = '/var/opt/mssql/data/bak_partition.mdf', SIZE = 16MB, FILEGROWTH = 16MB),
	FILEGROUP fg1 (NAME = bak_partition_fg1, FILENAME = '/var/opt/mssql/data/bak_partition_fg1.ndf', SIZE = 8MB, FILEGROWTH = 8MB),
	FILEGROUP fg2 (NAME = bak_partition_fg2, FILENAME = '/var/opt/mssql/data/bak_partition_fg2.ndf', SIZE = 8MB, FILEGROWTH = 8MB)
	LOG ON (NAME = bak_partition_log, FILENAME = '/var/opt/mssql/data/bak_partition_log.ldf', SIZE = 16MB, FILEGROWTH = 16MB)
	COLLATE SQL_Latin1_General_CP1_CI_AS;
ALTER DATABASE bak_partition SET RECOVERY FULL;
GO
USE bak_partition;
GO
-- integer families
CREATE PARTITION FUNCTION pf_int_l (int) AS RANGE LEFT FOR VALUES (100, 200, 300);
CREATE PARTITION FUNCTION pf_int_r (int) AS RANGE RIGHT FOR VALUES (0, 1000, 2000, 3000);
CREATE PARTITION FUNCTION pf_int_ident (int) AS RANGE RIGHT FOR VALUES (1, 2, 3, 4, 5);
CREATE PARTITION FUNCTION pf_bigint_r (bigint) AS RANGE RIGHT FOR VALUES (10000000000, 20000000000, 30000000000);
CREATE PARTITION FUNCTION pf_empty (int) AS RANGE RIGHT FOR VALUES ();
-- date: monthly RIGHT (period starts), monthly LEFT (period ends), daily, yearly
CREATE PARTITION FUNCTION pf_date_m_r (date) AS RANGE RIGHT FOR VALUES
  ('2026-01-01', '2026-02-01', '2026-03-01', '2026-04-01', '2026-05-01', '2026-06-01', '2026-07-01');
CREATE PARTITION FUNCTION pf_date_m_l (date) AS RANGE LEFT FOR VALUES
  ('2026-01-31', '2026-02-28', '2026-03-31', '2026-04-30', '2026-05-31', '2026-06-30');
CREATE PARTITION FUNCTION pf_date_d_r (date) AS RANGE RIGHT FOR VALUES
  ('2026-03-01', '2026-03-02', '2026-03-03', '2026-03-04', '2026-03-05');
CREATE PARTITION FUNCTION pf_date_y_r (date) AS RANGE RIGHT FOR VALUES ('2023-01-01', '2024-01-01', '2025-01-01', '2026-01-01');
-- datetime2 / datetime: quarterly RIGHT, monthly LEFT ends at full precision, monthly LEFT datetime,
-- LEFT with period STARTS (misaligned), a non-aligned set crossing a year, datetimeoffset aligned only in local time
CREATE PARTITION FUNCTION pf_dt2_q_r (datetime2(3)) AS RANGE RIGHT FOR VALUES
  ('2025-01-01', '2025-04-01', '2025-07-01', '2025-10-01', '2026-01-01', '2026-04-01');
CREATE PARTITION FUNCTION pf_dt2_m_l (datetime2(7)) AS RANGE LEFT FOR VALUES
  ('2026-01-31T23:59:59.9999999', '2026-02-28T23:59:59.9999999', '2026-03-31T23:59:59.9999999', '2026-04-30T23:59:59.9999999');
CREATE PARTITION FUNCTION pf_dt_m_l (datetime) AS RANGE LEFT FOR VALUES
  ('2026-01-31T23:59:59.997', '2026-02-28T23:59:59.997', '2026-03-31T23:59:59.997');
CREATE PARTITION FUNCTION pf_dt2_bad_l (datetime2(0)) AS RANGE LEFT FOR VALUES
  ('2025-11-01', '2025-12-01', '2026-01-01', '2026-02-01');
CREATE PARTITION FUNCTION pf_dt2_nonal_r (datetime2(3)) AS RANGE RIGHT FOR VALUES
  ('2025-12-15', '2026-01-15', '2026-02-10', '2026-04-01');
CREATE PARTITION FUNCTION pf_dto_r (datetimeoffset(0)) AS RANGE RIGHT FOR VALUES
  ('2026-01-01T00:00:00+00:00', '2026-02-01T00:00:00+02:00', '2026-03-01T00:00:00+00:00');
-- strings
CREATE PARTITION FUNCTION pf_vc_r (varchar(10)) AS RANGE RIGHT FOR VALUES ('G', 'N', 'T');
CREATE PARTITION FUNCTION pf_nvc_l (nvarchar(20)) AS RANGE LEFT FOR VALUES (N'beta', N'Жук', N'omega');

-- schemes: most ALL TO PRIMARY; two spread over fg1 / fg2 (no NEXT USED)
CREATE PARTITION SCHEME ps_int_l AS PARTITION pf_int_l ALL TO ([PRIMARY]);
CREATE PARTITION SCHEME ps_int_r_2fg AS PARTITION pf_int_r TO (fg1, fg2, fg1, fg2, fg1);
CREATE PARTITION SCHEME ps_int_ident AS PARTITION pf_int_ident ALL TO ([PRIMARY]);
CREATE PARTITION SCHEME ps_bigint_r AS PARTITION pf_bigint_r ALL TO (fg2);
CREATE PARTITION SCHEME ps_empty AS PARTITION pf_empty ALL TO ([PRIMARY]);
CREATE PARTITION SCHEME ps_date_m_r_2fg AS PARTITION pf_date_m_r TO (fg1, fg1, fg2, fg2, [PRIMARY], fg1, fg2, [PRIMARY]);
CREATE PARTITION SCHEME ps_date_m_l AS PARTITION pf_date_m_l ALL TO ([PRIMARY]);
CREATE PARTITION SCHEME ps_date_d_r AS PARTITION pf_date_d_r ALL TO ([PRIMARY]);
CREATE PARTITION SCHEME ps_date_y_r AS PARTITION pf_date_y_r ALL TO ([PRIMARY]);
CREATE PARTITION SCHEME ps_dt2_q_r AS PARTITION pf_dt2_q_r ALL TO ([PRIMARY]);
CREATE PARTITION SCHEME ps_dt2_m_l AS PARTITION pf_dt2_m_l ALL TO ([PRIMARY]);
CREATE PARTITION SCHEME ps_dt_m_l AS PARTITION pf_dt_m_l ALL TO ([PRIMARY]);
CREATE PARTITION SCHEME ps_dt2_bad_l AS PARTITION pf_dt2_bad_l ALL TO ([PRIMARY]);
CREATE PARTITION SCHEME ps_dt2_nonal_r AS PARTITION pf_dt2_nonal_r ALL TO ([PRIMARY]);
CREATE PARTITION SCHEME ps_dto_r AS PARTITION pf_dto_r ALL TO ([PRIMARY]);
CREATE PARTITION SCHEME ps_vc_r AS PARTITION pf_vc_r ALL TO ([PRIMARY]);
CREATE PARTITION SCHEME ps_nvc_l AS PARTITION pf_nvc_l ALL TO ([PRIMARY]);
-- a second scheme on the same function (two schemes may share a function)
CREATE PARTITION SCHEME ps_date_m_r_all AS PARTITION pf_date_m_r ALL TO ([PRIMARY]);
GO

-- 1. heap over two filegroups; per-partition ROW compression on partition 3 (below)
CREATE TABLE dbo.t_heap_int (id int NOT NULL, v varchar(60) NULL, pad char(200) NULL) ON ps_int_r_2fg (id);
WITH n AS (SELECT TOP (3601) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.t_heap_int SELECT i - 201, 'heap-' + CAST(i - 201 AS varchar(10)), 'p' FROM n;

-- 2. clustered PK over fg1/fg2/PRIMARY, an aligned NC index (partition column not in its key) and a non-aligned one
CREATE TABLE dbo.t_ci_date (d date NOT NULL, id int NOT NULL, amt decimal(12,2) NULL, v varchar(40) NULL,
  CONSTRAINT pk_t_ci_date PRIMARY KEY CLUSTERED (d, id)) ON ps_date_m_r_2fg (d);
WITH n AS (SELECT TOP (2430) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.t_ci_date SELECT DATEADD(day, (i - 1) / 10, CAST('2025-12-01' AS date)), i, CAST(i AS decimal(12,2)) / 3,
  'ci-' + CAST(i AS varchar(10)) FROM n;
CREATE NONCLUSTERED INDEX ix_t_ci_date_amt ON dbo.t_ci_date (amt) ON ps_date_m_r_2fg (d);
CREATE NONCLUSTERED INDEX ix_t_ci_date_v ON dbo.t_ci_date (v) ON [PRIMARY];

-- 3. clustered columnstore, quarterly datetime2(3)
CREATE TABLE dbo.t_cci_q (ts datetime2(3) NOT NULL, id int NOT NULL, val int NOT NULL) ON ps_dt2_q_r (ts);
CREATE CLUSTERED COLUMNSTORE INDEX cci_t_cci_q ON dbo.t_cci_q ON ps_dt2_q_r (ts);
WITH n AS (SELECT TOP (9000) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.t_cci_q SELECT DATEADD(minute, i * 37, CAST('2025-03-15T00:00:00' AS datetime2(3))), i, i % 97 FROM n;
INSERT INTO dbo.t_cci_q SELECT DATEADD(millisecond, k.k, CAST(rv.value AS datetime2(3))), 100000 + ROW_NUMBER() OVER (ORDER BY rv.boundary_id, k.k), -1
  FROM sys.partition_range_values rv JOIN sys.partition_functions f ON f.function_id = rv.function_id
  CROSS JOIN (VALUES (-1), (0), (1)) k(k) WHERE f.name = 'pf_dt2_q_r';

-- 4..: one table per function, rows on every boundary edge plus a spread
CREATE TABLE dbo.t_int_l (k int NOT NULL, v varchar(20) NULL) ON ps_int_l (k);
WITH n AS (SELECT TOP (420) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.t_int_l SELECT i - 10, 'l' + CAST(i AS varchar(10)) FROM n;

CREATE TABLE dbo.t_int_ident (k int NOT NULL, id int NOT NULL, CONSTRAINT pk_t_int_ident PRIMARY KEY CLUSTERED (k, id)) ON ps_int_ident (k);
WITH n AS (SELECT TOP (66) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.t_int_ident SELECT (i - 1) % 11 - 2, i FROM n;

CREATE TABLE dbo.t_bigint_r (k bigint NOT NULL, v varchar(20) NULL) ON ps_bigint_r (k);
INSERT INTO dbo.t_bigint_r VALUES (-5, 'a'), (9999999999, 'b'), (10000000000, 'c'), (10000000001, 'd'), (15000000000, 'e'),
  (19999999999, 'f'), (20000000000, 'g'), (25000000000, 'h'), (29999999999, 'i'), (30000000000, 'j'), (35000000000, 'k'),
  (9223372036854775807, 'max');

CREATE TABLE dbo.t_empty (k int NOT NULL, v varchar(20) NULL) ON ps_empty (k);
WITH n AS (SELECT TOP (50) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.t_empty SELECT i * 1000 - 20000, 'e' FROM n;

CREATE TABLE dbo.t_date_m_l (d date NOT NULL, id int NOT NULL, CONSTRAINT pk_t_date_m_l PRIMARY KEY CLUSTERED (d, id)) ON ps_date_m_l (d);
WITH n AS (SELECT TOP (426) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.t_date_m_l SELECT DATEADD(day, (i - 1) / 2, CAST('2025-12-15' AS date)), i FROM n;

CREATE TABLE dbo.t_date_d_r (d date NOT NULL, id int NOT NULL) ON ps_date_d_r (d);
WITH n AS (SELECT TOP (36) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.t_date_d_r SELECT DATEADD(day, (i - 1) / 3, CAST('2026-02-25' AS date)), i FROM n;

CREATE TABLE dbo.t_date_y_r (d date NOT NULL, id int NOT NULL, CONSTRAINT pk_t_date_y_r PRIMARY KEY CLUSTERED (d, id)) ON ps_date_y_r (d);
WITH n AS (SELECT TOP (400) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.t_date_y_r SELECT DATEADD(day, (i * 7919) % 1461, CAST('2022-06-01' AS date)), i FROM n;
INSERT INTO dbo.t_date_y_r SELECT DATEADD(day, k.k, CAST(rv.value AS date)), 1000 + ROW_NUMBER() OVER (ORDER BY rv.boundary_id, k.k)
  FROM sys.partition_range_values rv JOIN sys.partition_functions f ON f.function_id = rv.function_id
  CROSS JOIN (VALUES (-1), (0), (1)) k(k) WHERE f.name = 'pf_date_y_r';

CREATE TABLE dbo.t_dt2_m_l (ts datetime2(7) NOT NULL, id int NOT NULL, CONSTRAINT pk_t_dt2_m_l PRIMARY KEY CLUSTERED (ts, id)) ON ps_dt2_m_l (ts);
WITH n AS (SELECT TOP (300) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.t_dt2_m_l SELECT DATEADD(second, (i * 104729) % 15552000, CAST('2025-12-20T00:00:00' AS datetime2(7))), i FROM n;
INSERT INTO dbo.t_dt2_m_l SELECT DATEADD(nanosecond, 100 * k.k, CAST(rv.value AS datetime2(7))), 1000 + ROW_NUMBER() OVER (ORDER BY rv.boundary_id, k.k)
  FROM sys.partition_range_values rv JOIN sys.partition_functions f ON f.function_id = rv.function_id
  CROSS JOIN (VALUES (-1), (0), (1)) k(k) WHERE f.name = 'pf_dt2_m_l';

CREATE TABLE dbo.t_dt_m_l (ts datetime NOT NULL, id int NOT NULL) ON ps_dt_m_l (ts);
WITH n AS (SELECT TOP (300) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.t_dt_m_l SELECT DATEADD(second, (i * 104729) % 12960000, CAST('2025-12-20T00:00:00' AS datetime)), i FROM n;
INSERT INTO dbo.t_dt_m_l SELECT DATEADD(millisecond, 3 * k.k, CAST(rv.value AS datetime)), 1000 + ROW_NUMBER() OVER (ORDER BY rv.boundary_id, k.k)
  FROM sys.partition_range_values rv JOIN sys.partition_functions f ON f.function_id = rv.function_id
  CROSS JOIN (VALUES (-1), (0), (1)) k(k) WHERE f.name = 'pf_dt_m_l';

CREATE TABLE dbo.t_dt2_bad_l (ts datetime2(0) NOT NULL, id int NOT NULL, CONSTRAINT pk_t_dt2_bad_l PRIMARY KEY CLUSTERED (ts, id)) ON ps_dt2_bad_l (ts);
WITH n AS (SELECT TOP (300) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.t_dt2_bad_l SELECT DATEADD(second, (i * 104729) % 13000000, CAST('2025-10-15T00:00:00' AS datetime2(0))), i FROM n;
INSERT INTO dbo.t_dt2_bad_l SELECT DATEADD(second, k.k, CAST(rv.value AS datetime2(0))), 1000 + ROW_NUMBER() OVER (ORDER BY rv.boundary_id, k.k)
  FROM sys.partition_range_values rv JOIN sys.partition_functions f ON f.function_id = rv.function_id
  CROSS JOIN (VALUES (-1), (0), (1)) k(k) WHERE f.name = 'pf_dt2_bad_l';

CREATE TABLE dbo.t_dt2_nonal_r (ts datetime2(3) NOT NULL, id int NOT NULL) ON ps_dt2_nonal_r (ts);
WITH n AS (SELECT TOP (300) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.t_dt2_nonal_r SELECT DATEADD(second, (i * 104729) % 15000000, CAST('2025-11-20T00:00:00' AS datetime2(3))), i FROM n;
INSERT INTO dbo.t_dt2_nonal_r SELECT DATEADD(millisecond, k.k, CAST(rv.value AS datetime2(3))), 1000 + ROW_NUMBER() OVER (ORDER BY rv.boundary_id, k.k)
  FROM sys.partition_range_values rv JOIN sys.partition_functions f ON f.function_id = rv.function_id
  CROSS JOIN (VALUES (-1), (0), (1)) k(k) WHERE f.name = 'pf_dt2_nonal_r';

CREATE TABLE dbo.t_dto_r (ts datetimeoffset(0) NOT NULL, id int NOT NULL) ON ps_dto_r (ts);
WITH n AS (SELECT TOP (300) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.t_dto_r SELECT TODATETIMEOFFSET(DATEADD(second, (i * 104729) % 8000000, CAST('2025-12-20T00:00:00' AS datetime2(0))),
  ((i % 9) - 4) * 60), i FROM n;
INSERT INTO dbo.t_dto_r SELECT DATEADD(second, k.k, CAST(rv.value AS datetimeoffset(0))), 1000 + ROW_NUMBER() OVER (ORDER BY rv.boundary_id, k.k)
  FROM sys.partition_range_values rv JOIN sys.partition_functions f ON f.function_id = rv.function_id
  CROSS JOIN (VALUES (-1), (0), (1)) k(k) WHERE f.name = 'pf_dto_r';
-- the same instants written with other offsets
INSERT INTO dbo.t_dto_r VALUES ('2026-01-31T23:59:59+02:00', 2001), ('2026-01-31T22:00:00+00:00', 2002), ('2026-02-01T01:00:00+03:00', 2003),
  ('2026-02-28T23:30:00-01:00', 2004);

CREATE TABLE dbo.t_vc_r (code varchar(10) NOT NULL, id int NOT NULL, CONSTRAINT pk_t_vc_r PRIMARY KEY CLUSTERED (code, id)) ON ps_vc_r (code);
INSERT INTO dbo.t_vc_r VALUES ('', 1), ('A', 2), ('F', 3), ('Fz', 4), ('G', 5), ('G ', 6), ('g', 7), ('Ga', 8), ('M', 9), ('N', 10),
  ('n', 11), ('Na', 12), ('S', 13), ('T', 14), ('t', 15), ('Ta', 16), ('Z', 17), ('zz', 18), ('_x', 19), ('9', 20);

CREATE TABLE dbo.t_nvc_l (name nvarchar(20) NOT NULL, id int NOT NULL) ON ps_nvc_l (name);
INSERT INTO dbo.t_nvc_l VALUES (N'alpha', 1), (N'beta', 2), (N'Beta', 3), (N'betaa', 4), (N'Жук', 5), (N'жук', 6), (N'Жуки', 7),
  (N'omega', 8), (N'zeta', 9), (N'ярд', 10), (N'', 11), (N'Омега', 12);

CREATE TABLE dbo.t_date_m_r_all (d date NOT NULL, id int NOT NULL) ON ps_date_m_r_all (d);
WITH n AS (SELECT TOP (60) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.t_date_m_r_all SELECT DATEADD(day, i * 5, CAST('2025-12-10' AS date)), i FROM n;

-- per-partition compression; compress the columnstore delta stores, then leave one open delta store
ALTER TABLE dbo.t_heap_int REBUILD PARTITION = 3 WITH (DATA_COMPRESSION = ROW);
ALTER INDEX cci_t_cci_q ON dbo.t_cci_q REORGANIZE WITH (COMPRESS_ALL_ROW_GROUPS = ON);
INSERT INTO dbo.t_cci_q VALUES ('2025-05-05T05:05:05.005', 200001, 5), ('2025-05-06T06:06:06.006', 200002, 6);
CHECKPOINT;
GO
-- datetime2(7) RIGHT monthly (period starts): the default datetime2 precision, the most common real-world shape
CREATE PARTITION FUNCTION pf_dt2_m_r7 (datetime2(7)) AS RANGE RIGHT FOR VALUES ('2025-11-01', '2025-12-01', '2026-01-01', '2026-02-01', '2026-03-01');
CREATE PARTITION SCHEME ps_dt2_m_r7 AS PARTITION pf_dt2_m_r7 ALL TO ([PRIMARY]);
CREATE TABLE dbo.t_dt2_m_r7 (ts datetime2(7) NOT NULL, id int NOT NULL, CONSTRAINT pk_t_dt2_m_r7 PRIMARY KEY CLUSTERED (ts, id)) ON ps_dt2_m_r7 (ts);
WITH n AS (SELECT TOP (200) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS i FROM sys.all_objects a CROSS JOIN sys.all_objects b)
INSERT INTO dbo.t_dt2_m_r7 SELECT DATEADD(second, (i * 104729) % 15000000, CAST('2025-10-20T00:00:00' AS datetime2(7))), i FROM n;
-- edges: b - 600 ns, b - 500 ns, b - 100 ns, b, b + 100 ns
INSERT INTO dbo.t_dt2_m_r7 SELECT DATEADD(nanosecond, k.k, CAST(rv.value AS datetime2(7))), 1000 + ROW_NUMBER() OVER (ORDER BY rv.boundary_id, k.k)
  FROM sys.partition_range_values rv JOIN sys.partition_functions f ON f.function_id = rv.function_id
  CROSS JOIN (VALUES (-600), (-500), (-100), (0), (100)) k(k) WHERE f.name = 'pf_dt2_m_r7';
CHECKPOINT;
GO
-- Boundary types E7 did not cover.
CREATE PARTITION FUNCTION pf_dec_r (decimal(10, 2)) AS RANGE RIGHT FOR VALUES (-5.25, 10.50, 20.75);
CREATE PARTITION SCHEME ps_dec_r AS PARTITION pf_dec_r ALL TO ([PRIMARY]);
CREATE TABLE dbo.t_dec_r (amount decimal(10, 2) NOT NULL, id int NOT NULL) ON ps_dec_r (amount);
INSERT INTO dbo.t_dec_r VALUES (-10, 1), (-5.26, 2), (-5.25, 3), (0, 4), (10.49, 5), (10.50, 6), (20.74, 7), (20.75, 8), (99.99, 9);
CREATE PARTITION FUNCTION pf_guid_l (uniqueidentifier) AS RANGE LEFT FOR VALUES
	('6F9619FF-8B86-D011-B42D-00C04FC964FF', 'A0A1A2A3-B0B1-C0C1-D0D1-D2D3D4D5D6D7');
CREATE PARTITION SCHEME ps_guid_l AS PARTITION pf_guid_l ALL TO ([PRIMARY]);
CREATE TABLE dbo.t_guid_l (g uniqueidentifier NOT NULL, id int NOT NULL) ON ps_guid_l (g);
INSERT INTO dbo.t_guid_l VALUES ('00000000-0000-0000-0000-000000000000', 1), ('6F9619FF-8B86-D011-B42D-00C04FC964FF', 2),
	('A0A1A2A3-B0B1-C0C1-D0D1-D2D3D4D5D6D7', 3), ('FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF', 4);
CREATE PARTITION FUNCTION pf_bin_r (varbinary(8)) AS RANGE RIGHT FOR VALUES (0x10, 0x8000, 0xFF00FF);
CREATE PARTITION SCHEME ps_bin_r AS PARTITION pf_bin_r ALL TO ([PRIMARY]);
CREATE TABLE dbo.t_bin_r (b varbinary(8) NOT NULL, id int NOT NULL) ON ps_bin_r (b);
INSERT INTO dbo.t_bin_r VALUES (0x, 1), (0x0F, 2), (0x10, 3), (0x7FFF, 4), (0x8000, 5), (0xFF00FF, 6), (0xFFFF, 7);
CREATE PARTITION FUNCTION pf_time_r (time(3)) AS RANGE RIGHT FOR VALUES ('08:00', '12:00:00.500', '18:00');
CREATE PARTITION SCHEME ps_time_r AS PARTITION pf_time_r ALL TO ([PRIMARY]);
CREATE TABLE dbo.t_time_r (t time(3) NOT NULL, id int NOT NULL) ON ps_time_r (t);
INSERT INTO dbo.t_time_r VALUES ('00:00', 1), ('07:59:59.999', 2), ('08:00', 3), ('12:00:00.499', 4), ('12:00:00.500', 5),
	('18:00', 6), ('23:59:59.999', 7);
CREATE PARTITION FUNCTION pf_sdt_l (smalldatetime) AS RANGE LEFT FOR VALUES ('2026-01-31 23:59', '2026-02-28 23:59');
CREATE PARTITION SCHEME ps_sdt_l AS PARTITION pf_sdt_l ALL TO ([PRIMARY]);
CREATE TABLE dbo.t_sdt_l (ts smalldatetime NOT NULL, id int NOT NULL) ON ps_sdt_l (ts);
INSERT INTO dbo.t_sdt_l VALUES ('2026-01-15', 1), ('2026-01-31 23:59', 2), ('2026-02-01', 3), ('2026-02-28 23:59', 4), ('2026-03-15', 5);
CREATE PARTITION FUNCTION pf_tiny_r (tinyint) AS RANGE RIGHT FOR VALUES (0, 128, 255);
CREATE PARTITION SCHEME ps_tiny_r AS PARTITION pf_tiny_r ALL TO ([PRIMARY]);
CREATE TABLE dbo.t_tiny_r (k tinyint NOT NULL, id int NOT NULL) ON ps_tiny_r (k);
INSERT INTO dbo.t_tiny_r VALUES (0, 1), (1, 2), (127, 3), (128, 4), (254, 5), (255, 6);
CREATE PARTITION FUNCTION pf_money_r (money) AS RANGE RIGHT FOR VALUES (-1.5, 1000.2525, 922337203685477.5807);
CREATE PARTITION SCHEME ps_money_r AS PARTITION pf_money_r ALL TO ([PRIMARY]);
CREATE TABLE dbo.t_money_r (m money NOT NULL, id int NOT NULL) ON ps_money_r (m);
INSERT INTO dbo.t_money_r VALUES (-2, 1), (-1.5, 2), (0, 3), (1000.2525, 4), (922337203685477.5807, 5);
CREATE PARTITION FUNCTION pf_bit_r (bit) AS RANGE RIGHT FOR VALUES (1);
CREATE PARTITION SCHEME ps_bit_r AS PARTITION pf_bit_r ALL TO ([PRIMARY]);
CREATE TABLE dbo.t_bit_r (b bit NOT NULL, id int NOT NULL) ON ps_bit_r (b);
INSERT INTO dbo.t_bit_r VALUES (0, 1), (1, 2);
-- A NULL boundary: legal (NULL is the lowest value; the NULL keys go to partition 1, RANGE RIGHT), shown
-- NULL by sys.partition_range_values, a NULL element of the reader's boundaries (review of #551).
CREATE PARTITION FUNCTION pf_null_r (int) AS RANGE RIGHT FOR VALUES (NULL, 100);
CREATE PARTITION SCHEME ps_null_r AS PARTITION pf_null_r ALL TO ([PRIMARY]);
CREATE TABLE dbo.t_null_r (k int NULL, id int NOT NULL) ON ps_null_r (k);
INSERT INTO dbo.t_null_r VALUES (NULL, 1), (5, 2), (100, 3), (150, 4);
CREATE PARTITION FUNCTION pf_real_l (real) AS RANGE LEFT FOR VALUES (-0.5, 2.25);
CREATE PARTITION SCHEME ps_real_l AS PARTITION pf_real_l ALL TO ([PRIMARY]);
CREATE TABLE dbo.t_real_l (r real NOT NULL, id int NOT NULL) ON ps_real_l (r);
INSERT INTO dbo.t_real_l VALUES (-1, 1), (-0.5, 2), (1, 3), (2.25, 4), (3, 5);
CREATE PARTITION FUNCTION pf_float_r (float) AS RANGE RIGHT FOR VALUES (-1e10, 0.125, 6.5e100);
CREATE PARTITION SCHEME ps_float_r AS PARTITION pf_float_r ALL TO ([PRIMARY]);
CREATE TABLE dbo.t_float_r (f float NOT NULL, id int NOT NULL) ON ps_float_r (f);
INSERT INTO dbo.t_float_r VALUES (-1e11, 1), (0, 2), (0.125, 3), (1e200, 4);
CREATE PARTITION FUNCTION pf_bin4_l (binary(4)) AS RANGE LEFT FOR VALUES (0x00000010, 0x7F000000);
CREATE PARTITION SCHEME ps_bin4_l AS PARTITION pf_bin4_l ALL TO ([PRIMARY]);
CREATE TABLE dbo.t_bin4_l (b binary(4) NOT NULL, id int NOT NULL) ON ps_bin4_l (b);
INSERT INTO dbo.t_bin4_l VALUES (0x00000001, 1), (0x00000010, 2), (0x7F000000, 3), (0xFFFFFFFF, 4);
CREATE PARTITION FUNCTION pf_char_r (char(3)) AS RANGE RIGHT FOR VALUES ('b', 'mm', 'xyz');
CREATE PARTITION SCHEME ps_char_r AS PARTITION pf_char_r ALL TO ([PRIMARY]);
CREATE TABLE dbo.t_char_r (c char(3) NOT NULL, id int NOT NULL) ON ps_char_r (c);
INSERT INTO dbo.t_char_r VALUES ('a', 1), ('b', 2), ('m', 3), ('mm', 4), ('z', 5);
CREATE PARTITION FUNCTION pf_nchar_l (nchar(3)) AS RANGE LEFT FOR VALUES (N'ж', N'лес');
CREATE PARTITION SCHEME ps_nchar_l AS PARTITION pf_nchar_l ALL TO ([PRIMARY]);
CREATE TABLE dbo.t_nchar_l (c nchar(3) NOT NULL, id int NOT NULL) ON ps_nchar_l (c);
INSERT INTO dbo.t_nchar_l VALUES (N'а', 1), (N'ж', 2), (N'кот', 3), (N'лес', 4), (N'я', 5);
-- datetimeoffset(7): TYPE mssql reads it to the microsecond (D6), so the boundaries keep a 0 digit there.
CREATE PARTITION FUNCTION pf_dto7_r (datetimeoffset(7)) AS RANGE RIGHT FOR VALUES
	('2026-01-01T00:00:00.1234560+05:30', '2026-06-01T12:00:00.0000000-08:00');
CREATE PARTITION SCHEME ps_dto7_r AS PARTITION pf_dto7_r ALL TO ([PRIMARY]);
CREATE TABLE dbo.t_dto7_r (ts datetimeoffset(7) NOT NULL, id int NOT NULL) ON ps_dto7_r (ts);
INSERT INTO dbo.t_dto7_r VALUES ('2025-12-31T18:00:00+00:00', 1), ('2026-03-01T00:00:00+00:00', 2), ('2026-07-01T00:00:00+00:00', 3);
GO
-- Maintenance: a SPLIT and a MERGE renumber the boundary ids (positional), NEXT USED on a scheme
-- given as a list (ALL TO creates its own at once).
ALTER PARTITION SCHEME ps_int_l NEXT USED [PRIMARY];
ALTER PARTITION FUNCTION pf_int_l () SPLIT RANGE (150);
ALTER PARTITION FUNCTION pf_date_d_r () MERGE RANGE ('2026-03-03');
ALTER PARTITION SCHEME ps_int_r_2fg NEXT USED fg2;
GO
CHECKPOINT;
GO
BACKUP DATABASE bak_partition TO DISK = '$(DIR)/partition_full.bak' WITH FORMAT, INIT, CHECKSUM, NAME = 'partition_full';
GO
