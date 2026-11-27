-- Spec 093 T8b, experiment S1d: S1's 50 sql_variant values (measured/S1.md) as a record stores them, against
-- the server's own CAST(v AS varbinary(max)) and SQL_VARIANT_PROPERTY. Run by s1d.sh on a throwaway
-- container; synthetic values only.
SET NOCOUNT ON;
GO
IF DB_ID('s1d') IS NOT NULL DROP DATABASE s1d;
GO
CREATE DATABASE s1d;
GO
USE s1d;
GO
CREATE TABLE dbo.s1 (id int PRIMARY KEY, v sql_variant NULL);
INSERT INTO dbo.s1 VALUES
	(1, CAST(CAST(1 AS bit) AS sql_variant)), (2, CAST(CAST(0 AS bit) AS sql_variant)), (3, CAST(CAST(255 AS tinyint) AS sql_variant)), (4, CAST(CAST(-32768 AS smallint) AS sql_variant)),
	(5, CAST(CAST(-2147483648 AS int) AS sql_variant)), (6, CAST(CAST(-9223372036854775808 AS bigint) AS sql_variant)), (7, CAST(CAST(1.5 AS real) AS sql_variant)),
	(8, CAST(CAST(-1.25e300 AS float) AS sql_variant)), (9, CAST(CAST(0.5 AS decimal(1, 1)) AS sql_variant)), (10, CAST(CAST(-1234567.89 AS decimal(9, 2)) AS sql_variant)),
	(11, CAST(CAST(123456789012345678 AS decimal(18, 0)) AS sql_variant)), (12, CAST(CAST(1234567890.123456789 AS decimal(19, 9)) AS sql_variant)),
	(13, CAST(CAST(-12345678901234567890.12345678 AS decimal(28, 8)) AS sql_variant)),
	(14, CAST(CAST(99999999999999999999999999999999999999 AS decimal(38, 0)) AS sql_variant)),
	(15, CAST(CAST(-0.00000000000000000000000000000000000001 AS decimal(38, 38)) AS sql_variant)), (16, CAST(CAST(12.5 AS numeric(10, 3)) AS sql_variant)),
	(17, CAST(CAST(-922337203685477.5808 AS money) AS sql_variant)), (18, CAST(CAST(214748.3647 AS smallmoney) AS sql_variant)),
	(19, CAST(CAST('0001-01-01' AS date) AS sql_variant)), (20, CAST(CAST('9999-12-31' AS date) AS sql_variant)), (21, CAST(CAST('23:59:59' AS time(0)) AS sql_variant)),
	(22, CAST(CAST('10:00:00.123' AS time(3)) AS sql_variant)), (23, CAST(CAST('10:00:00.123456' AS time(6)) AS sql_variant)),
	(24, CAST(CAST('10:00:00.1234567' AS time(7)) AS sql_variant)), (25, CAST(CAST('2026-01-01 10:00:00' AS datetime2(0)) AS sql_variant)),
	(26, CAST(CAST('2026-01-01 10:00:00.123' AS datetime2(3)) AS sql_variant)), (27, CAST(CAST('2026-01-01 10:00:00.123456' AS datetime2(6)) AS sql_variant)),
	(28, CAST(CAST('2026-01-01 10:00:00.1234567' AS datetime2(7)) AS sql_variant)), (29, CAST(CAST('9999-12-31 23:59:59.9999999' AS datetime2(7)) AS sql_variant)),
	(30, CAST(CAST('0001-01-01 00:00:00' AS datetime2(7)) AS sql_variant)), (31, CAST(CAST('2026-11-13 07:08:09.123' AS datetime) AS sql_variant)),
	(32, CAST(CAST('2026-11-13 07:08:00' AS smalldatetime) AS sql_variant)), (33, CAST(CAST('2026-01-01 10:00:00 +14:00' AS datetimeoffset(0)) AS sql_variant)),
	(34, CAST(CAST('2026-01-01 10:00:00.123456 -14:00' AS datetimeoffset(6)) AS sql_variant)),
	(35, CAST(CAST('2026-01-01 10:00:00.1234567 +05:30' AS datetimeoffset(7)) AS sql_variant)),
	(36, CAST(CAST('9999-12-31 23:59:59.9999999 +00:00' AS datetimeoffset(7)) AS sql_variant)),
	(37, CAST(CAST('6F9619FF-8B86-D011-B42D-00C04FC964FF' AS uniqueidentifier) AS sql_variant)), (38, CAST(CAST(0x0102 AS binary(4)) AS sql_variant)),
	(39, CAST(CAST(0x010203 AS varbinary(10)) AS sql_variant)), (40, CAST(CAST('ab' COLLATE SQL_Latin1_General_CP1_CI_AS AS char(5)) AS sql_variant)),
	(41, CAST(CAST(N'café' COLLATE SQL_Latin1_General_CP1_CI_AS AS varchar(10)) AS sql_variant)),
	(42, CAST(CAST(N'дом' COLLATE Cyrillic_General_CI_AS AS varchar(10)) AS sql_variant)),
	(43, CAST(CAST(N'漢字' COLLATE Japanese_CI_AS AS varchar(10)) AS sql_variant)),
	(44, CAST(CAST(N'😀é' COLLATE Latin1_General_100_CI_AS_SC_UTF8 AS varchar(20)) AS sql_variant)),
	(45, CAST(CAST(N'ab' COLLATE SQL_Latin1_General_CP1_CI_AS AS nchar(10)) AS sql_variant)),
	(46, CAST(CAST(N'😀x' COLLATE Latin1_General_100_CI_AS_SC AS nvarchar(20)) AS sql_variant)),
	(47, CAST(CAST(N'дом' COLLATE Cyrillic_General_CI_AS AS char(6)) AS sql_variant)), (48, NULL),
	(49, CAST(CAST('' COLLATE SQL_Latin1_General_CP1_CI_AS AS varchar(5)) AS sql_variant)), (50, CAST(CAST(0x AS varbinary(5)) AS sql_variant));
GO
SELECT id, CAST(SQL_VARIANT_PROPERTY(v, 'BaseType') AS varchar(20)) AS base_type,
	CAST(SQL_VARIANT_PROPERTY(v, 'Precision') AS int) AS prec, CAST(SQL_VARIANT_PROPERTY(v, 'Scale') AS int) AS scale,
	CAST(SQL_VARIANT_PROPERTY(v, 'MaxLength') AS int) AS max_length,
	CAST(SQL_VARIANT_PROPERTY(v, 'Collation') AS varchar(60)) AS collation,
	CAST(COLLATIONPROPERTY(CAST(SQL_VARIANT_PROPERTY(v, 'Collation') AS varchar(60)), 'CollationId') AS int) AS collation_id,
	CONVERT(varchar(max), CAST(v AS varbinary(max)), 2) AS disk
FROM dbo.s1 ORDER BY id;
GO
CHECKPOINT;
BACKUP DATABASE s1d TO DISK = '/var/opt/mssql/data/s1d.bak' WITH FORMAT, INIT, NAME = 's1d';
GO
