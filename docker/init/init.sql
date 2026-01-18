-- Initialize comprehensive test database for DuckDB MSSQL Extension
-- This script creates test schemas, tables, views with various data types and edge cases

USE master;
GO

-- =============================================================================
-- Create test database
-- =============================================================================
IF NOT EXISTS (SELECT name FROM sys.databases WHERE name = 'TestDB')
BEGIN
    CREATE DATABASE TestDB;
    PRINT 'TestDB created';
END
GO

USE TestDB;
GO

-- =============================================================================
-- Create test schemas including ones with special/reserved names
-- =============================================================================
IF NOT EXISTS (SELECT * FROM sys.schemas WHERE name = 'test')
    EXEC('CREATE SCHEMA test');
IF NOT EXISTS (SELECT * FROM sys.schemas WHERE name = 'SELECT')
    EXEC('CREATE SCHEMA [SELECT]');
IF NOT EXISTS (SELECT * FROM sys.schemas WHERE name = 'My Schema')
    EXEC('CREATE SCHEMA [My Schema]');
IF NOT EXISTS (SELECT * FROM sys.schemas WHERE name = 'schema"quote')
    EXEC('CREATE SCHEMA [schema"quote]');
GO

PRINT 'Schemas created: test, SELECT, My Schema, schema"quote';
GO

-- =============================================================================
-- Drop existing test tables if they exist
-- =============================================================================
IF OBJECT_ID('dbo.TestSimplePK', 'U') IS NOT NULL DROP TABLE dbo.TestSimplePK;
IF OBJECT_ID('dbo.TestCompositePK', 'U') IS NOT NULL DROP TABLE dbo.TestCompositePK;
IF OBJECT_ID('dbo.LargeTable', 'U') IS NOT NULL DROP TABLE dbo.LargeTable;
IF OBJECT_ID('dbo.AllDataTypes', 'U') IS NOT NULL DROP TABLE dbo.AllDataTypes;
IF OBJECT_ID('dbo.NullableTypes', 'U') IS NOT NULL DROP TABLE dbo.NullableTypes;
IF OBJECT_ID('test.test', 'U') IS NOT NULL DROP TABLE test.test;
IF OBJECT_ID('[SELECT].[TABLE]', 'U') IS NOT NULL DROP TABLE [SELECT].[TABLE];
IF OBJECT_ID('[My Schema].[My Table]', 'U') IS NOT NULL DROP TABLE [My Schema].[My Table];
IF OBJECT_ID('[schema"quote].[table"quote]', 'U') IS NOT NULL DROP TABLE [schema"quote].[table"quote];
IF OBJECT_ID('dbo.[SELECT]', 'U') IS NOT NULL DROP TABLE dbo.[SELECT];
IF OBJECT_ID('dbo.[Space Column Table]', 'U') IS NOT NULL DROP TABLE dbo.[Space Column Table];
GO

-- Drop views
IF OBJECT_ID('dbo.LargeTableView', 'V') IS NOT NULL DROP VIEW dbo.LargeTableView;
IF OBJECT_ID('dbo.AllTypesView', 'V') IS NOT NULL DROP VIEW dbo.AllTypesView;
IF OBJECT_ID('[SELECT].[VIEW]', 'V') IS NOT NULL DROP VIEW [SELECT].[VIEW];
IF OBJECT_ID('dbo.[SELECT VIEW]', 'V') IS NOT NULL DROP VIEW dbo.[SELECT VIEW];
GO

-- =============================================================================
-- Table 1: Simple PK table
-- =============================================================================
CREATE TABLE dbo.TestSimplePK (
    id INT NOT NULL PRIMARY KEY,
    name NVARCHAR(100) NOT NULL,
    value DECIMAL(10, 2) NULL,
    created_at DATETIME2 DEFAULT GETDATE()
);
GO

INSERT INTO dbo.TestSimplePK (id, name, value) VALUES
    (1, 'First Record', 100.50),
    (2, 'Second Record', 200.75),
    (3, 'Third Record', NULL),
    (4, 'Fourth Record', 400.00),
    (5, 'Fifth Record', 500.25);
GO

-- =============================================================================
-- Table 2: Composite PK table
-- =============================================================================
CREATE TABLE dbo.TestCompositePK (
    region_id INT NOT NULL,
    product_id INT NOT NULL,
    quantity INT NOT NULL,
    unit_price DECIMAL(10, 2) NOT NULL,
    order_date DATE NOT NULL,
    CONSTRAINT PK_TestCompositePK PRIMARY KEY (region_id, product_id)
);
GO

INSERT INTO dbo.TestCompositePK (region_id, product_id, quantity, unit_price, order_date) VALUES
    (1, 100, 10, 25.50, '2024-01-15'),
    (1, 101, 5, 50.00, '2024-01-16'),
    (1, 102, 20, 12.75, '2024-01-17'),
    (2, 100, 15, 25.50, '2024-01-15'),
    (2, 101, 8, 50.00, '2024-01-18'),
    (3, 100, 25, 25.50, '2024-01-19'),
    (3, 102, 12, 12.75, '2024-01-20');
GO

-- =============================================================================
-- Table 3: Large table with 100,000+ rows
-- =============================================================================
CREATE TABLE dbo.LargeTable (
    id INT NOT NULL PRIMARY KEY,
    category INT NOT NULL,
    name VARCHAR(100) NOT NULL,
    description NVARCHAR(500) NULL,
    value DECIMAL(18, 4) NOT NULL,
    created_date DATE NOT NULL,
    is_active BIT NOT NULL DEFAULT 1
);
GO

-- Generate 150,000 rows using recursive CTE
WITH Numbers AS (
    SELECT 1 AS n
    UNION ALL
    SELECT n + 1 FROM Numbers WHERE n < 150000
)
INSERT INTO dbo.LargeTable (id, category, name, description, value, created_date, is_active)
SELECT
    n AS id,
    (n % 100) + 1 AS category,
    'Item_' + CAST(n AS VARCHAR(10)) AS name,
    CASE WHEN n % 3 = 0 THEN NULL ELSE N'Description for item ' + CAST(n AS NVARCHAR(10)) + N' with some unicode: ' END AS description,
    CAST(n AS DECIMAL(18, 4)) * 1.5 + (n % 1000) / 100.0 AS value,
    DATEADD(day, n % 365, '2024-01-01') AS created_date,
    CASE WHEN n % 10 = 0 THEN 0 ELSE 1 END AS is_active
FROM Numbers
OPTION (MAXRECURSION 0);
GO

PRINT 'LargeTable created with 150,000 rows';
GO

-- =============================================================================
-- Table 4: All supported data types
-- =============================================================================
CREATE TABLE dbo.AllDataTypes (
    id INT NOT NULL PRIMARY KEY,
    -- Integer types
    col_tinyint TINYINT NOT NULL,
    col_smallint SMALLINT NOT NULL,
    col_int INT NOT NULL,
    col_bigint BIGINT NOT NULL,
    -- Bit
    col_bit BIT NOT NULL,
    -- Floating point
    col_real REAL NOT NULL,
    col_float FLOAT NOT NULL,
    -- Money
    col_smallmoney SMALLMONEY NOT NULL,
    col_money MONEY NOT NULL,
    -- Decimal/Numeric
    col_decimal DECIMAL(18, 6) NOT NULL,
    col_numeric NUMERIC(10, 2) NOT NULL,
    -- GUID
    col_uniqueidentifier UNIQUEIDENTIFIER NOT NULL,
    -- String types
    col_char CHAR(10) NOT NULL,
    col_varchar VARCHAR(100) NOT NULL,
    col_nchar NCHAR(10) NOT NULL,
    col_nvarchar NVARCHAR(100) NOT NULL,
    -- Binary types
    col_binary BINARY(16) NOT NULL,
    col_varbinary VARBINARY(100) NOT NULL,
    -- Date/Time types
    col_date DATE NOT NULL,
    col_time TIME NOT NULL,
    col_time_scale TIME(3) NOT NULL,
    col_datetime DATETIME NOT NULL,
    col_datetime2 DATETIME2 NOT NULL,
    col_datetime2_scale DATETIME2(3) NOT NULL,
    col_smalldatetime SMALLDATETIME NOT NULL,
    col_datetimeoffset DATETIMEOFFSET NOT NULL,
    col_datetimeoffset_scale DATETIMEOFFSET(3) NOT NULL
);
GO

-- Insert test data with various values
INSERT INTO dbo.AllDataTypes VALUES
(1,
 255, 32767, 2147483647, 9223372036854775807,
 1,
 3.14159, 2.718281828459045,
 214748.3647, 922337203685477.5807,
 123456.789012, 12345678.90,
 NEWID(),
 'CHAR_VAL  ', 'varchar value', N'NCHAR     ', N'nvarchar value with unicode',
 0x0102030405060708090A0B0C0D0E0F10, 0xDEADBEEF,
 '2024-06-15', '13:45:30.1234567', '13:45:30.123',
 '2024-06-15 13:45:30.123', '2024-06-15 13:45:30.1234567', '2024-06-15 13:45:30.123',
 '2024-06-15 13:45:00',
 '2024-06-15 13:45:30.1234567 +05:30', '2024-06-15 13:45:30.123 +05:30'),
(2,
 0, -32768, -2147483648, -9223372036854775808,
 0,
 -3.14159, -2.718281828459045,
 -214748.3647, -922337203685477.5807,
 -123456.789012, -12345678.90,
 '00000000-0000-0000-0000-000000000000',
 'MIN_VAL   ', 'min values', N'MIN_NCHAR ', N'minimum nvarchar',
 0x00000000000000000000000000000000, 0x00,
 '1900-01-01', '00:00:00.0000000', '00:00:00.000',
 '1900-01-01 00:00:00.000', '0001-01-01 00:00:00.0000000', '0001-01-01 00:00:00.000',
 '1900-01-01 00:00:00',
 '1900-01-01 00:00:00.0000000 -08:00', '1900-01-01 00:00:00.000 -08:00'),
(3,
 128, 0, 0, 0,
 1,
 0.0, 0.0,
 0.0, 0.0,
 0.0, 0.0,
 'FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF',
 'ZERO      ', 'zero values', N'ZERO_NCHAR', N'zero nvarchar',
 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF, 0xFF,
 '2024-12-31', '23:59:59.9999999', '23:59:59.999',
 '2024-12-31 23:59:59.997', '9999-12-31 23:59:59.9999999', '9999-12-31 23:59:59.999',
 '2079-06-06 23:59:00',
 '2024-12-31 23:59:59.9999999 +00:00', '2024-12-31 23:59:59.999 +00:00');
GO

-- Add more rows for testing
INSERT INTO dbo.AllDataTypes
SELECT
    id + 3 AS id,
    col_tinyint, col_smallint, col_int, col_bigint,
    col_bit,
    col_real, col_float,
    col_smallmoney, col_money,
    col_decimal, col_numeric,
    NEWID(),
    col_char, col_varchar, col_nchar, col_nvarchar,
    col_binary, col_varbinary,
    DATEADD(day, id, col_date), col_time, col_time_scale,
    DATEADD(day, id, col_datetime), DATEADD(day, id, col_datetime2), col_datetime2_scale,
    col_smalldatetime,
    DATEADD(day, id, col_datetimeoffset), col_datetimeoffset_scale
FROM dbo.AllDataTypes WHERE id <= 3;
GO

PRINT 'AllDataTypes table created';
GO

-- =============================================================================
-- Table 5: Nullable types (all columns nullable with mix of NULL/non-NULL values)
-- =============================================================================
CREATE TABLE dbo.NullableTypes (
    id INT NOT NULL PRIMARY KEY,
    -- Integer types
    col_tinyint TINYINT NULL,
    col_smallint SMALLINT NULL,
    col_int INT NULL,
    col_bigint BIGINT NULL,
    -- Bit
    col_bit BIT NULL,
    -- Floating point
    col_real REAL NULL,
    col_float FLOAT NULL,
    -- Money
    col_smallmoney SMALLMONEY NULL,
    col_money MONEY NULL,
    -- Decimal/Numeric
    col_decimal DECIMAL(18, 6) NULL,
    col_numeric NUMERIC(10, 2) NULL,
    -- GUID
    col_uniqueidentifier UNIQUEIDENTIFIER NULL,
    -- String types
    col_char CHAR(10) NULL,
    col_varchar VARCHAR(100) NULL,
    col_nchar NCHAR(10) NULL,
    col_nvarchar NVARCHAR(100) NULL,
    -- Binary types
    col_binary BINARY(16) NULL,
    col_varbinary VARBINARY(100) NULL,
    -- Date/Time types
    col_date DATE NULL,
    col_time TIME NULL,
    col_datetime DATETIME NULL,
    col_datetime2 DATETIME2 NULL,
    col_smalldatetime SMALLDATETIME NULL
);
GO

-- Insert rows with various NULL patterns
INSERT INTO dbo.NullableTypes VALUES
(1, 1, 1, 1, 1, 1, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, NEWID(), 'a', 'a', N'a', N'a', 0x01, 0x01, '2024-01-01', '12:00:00', '2024-01-01', '2024-01-01', '2024-01-01'),
(2, NULL, 2, NULL, 2, NULL, 2.0, NULL, 2.0, NULL, 2.0, NULL, NULL, 'b', NULL, N'b', NULL, 0x02, NULL, '2024-01-02', NULL, '2024-01-02', NULL, '2024-01-02'),
(3, 3, NULL, 3, NULL, 1, NULL, 3.0, NULL, 3.0, NULL, 3.0, NEWID(), NULL, 'c', NULL, N'c', NULL, 0x03, NULL, '12:00:00', NULL, '2024-01-03', NULL),
(4, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL),
(5, 5, 5, 5, 5, 1, 5.0, 5.0, 5.0, 5.0, 5.0, 5.0, NEWID(), 'e', 'e', N'e', N'e', 0x05, 0x05, '2024-01-05', '12:00:00', '2024-01-05', '2024-01-05', '2024-01-05');
GO

PRINT 'NullableTypes table created';
GO

-- =============================================================================
-- Table 6: test.test table (for backward compatibility with existing tests)
-- =============================================================================
CREATE TABLE test.test (
    id INT NOT NULL PRIMARY KEY,
    name VARCHAR(100) NOT NULL,
    [SELECT "] VARCHAR(10) NULL  -- Column with special name
);
GO

INSERT INTO test.test (id, name) VALUES
(1, 'A'), (2, 'B'), (3, 'C'), (4, 'D'), (5, 'E');
GO

PRINT 'test.test table created';
GO

-- =============================================================================
-- Table 7: Tables with reserved word names
-- =============================================================================
-- Schema [SELECT] with table [TABLE]
CREATE TABLE [SELECT].[TABLE] (
    [COLUMN] INT NOT NULL PRIMARY KEY,
    [INDEX] VARCHAR(50) NOT NULL,
    [SELECT] NVARCHAR(100) NULL,
    [FROM] DATE NULL,
    [WHERE] DECIMAL(10,2) NULL
);
GO

INSERT INTO [SELECT].[TABLE] ([COLUMN], [INDEX], [SELECT], [FROM], [WHERE]) VALUES
(1, 'first', N'select value 1', '2024-01-01', 100.00),
(2, 'second', N'select value 2', '2024-01-02', 200.50),
(3, 'third', NULL, '2024-01-03', NULL);
GO

-- dbo.[SELECT] table
CREATE TABLE dbo.[SELECT] (
    id INT NOT NULL PRIMARY KEY,
    [FROM] VARCHAR(50) NOT NULL,
    [WHERE] INT NULL
);
GO

INSERT INTO dbo.[SELECT] (id, [FROM], [WHERE]) VALUES
(1, 'from_value_1', 10),
(2, 'from_value_2', 20),
(3, 'from_value_3', NULL);
GO

PRINT 'Reserved word tables created';
GO

-- =============================================================================
-- Table 8: Tables with space and quote characters in names
-- =============================================================================
CREATE TABLE [My Schema].[My Table] (
    [My Column] INT NOT NULL PRIMARY KEY,
    [Another Column] VARCHAR(50) NOT NULL,
    [Column With Spaces] NVARCHAR(100) NULL
);
GO

INSERT INTO [My Schema].[My Table] ([My Column], [Another Column], [Column With Spaces]) VALUES
(1, 'value1', N'space value 1'),
(2, 'value2', N'space value 2'),
(3, 'value3', NULL);
GO

CREATE TABLE [schema"quote].[table"quote] (
    [col"quote] INT NOT NULL PRIMARY KEY,
    [normal_col] VARCHAR(50) NOT NULL
);
GO

INSERT INTO [schema"quote].[table"quote] ([col"quote], [normal_col]) VALUES
(1, 'quote_value_1'),
(2, 'quote_value_2');
GO

CREATE TABLE dbo.[Space Column Table] (
    id INT NOT NULL PRIMARY KEY,
    [Column With Space] VARCHAR(100) NOT NULL,
    [Another Space Col] INT NULL,
    [Col"With"Quotes] NVARCHAR(50) NULL
);
GO

INSERT INTO dbo.[Space Column Table] (id, [Column With Space], [Another Space Col], [Col"With"Quotes]) VALUES
(1, 'space test 1', 10, N'quote test 1'),
(2, 'space test 2', 20, N'quote test 2'),
(3, 'space test 3', NULL, NULL);
GO

PRINT 'Special character tables created';
GO

-- =============================================================================
-- Table 9: Collation test table (different collations for filter pushdown tests)
-- =============================================================================
CREATE TABLE dbo.CollationTest (
    id INT NOT NULL PRIMARY KEY,
    -- Default collation (database default, typically SQL_Latin1_General_CP1_CI_AS)
    col_default VARCHAR(100) NOT NULL,
    -- Case-sensitive collation
    col_case_sensitive VARCHAR(100) COLLATE Latin1_General_CS_AS NOT NULL,
    -- Case-insensitive collation
    col_case_insensitive VARCHAR(100) COLLATE Latin1_General_CI_AS NOT NULL,
    -- Binary collation (exact byte comparison)
    col_binary VARCHAR(100) COLLATE Latin1_General_BIN NOT NULL,
    -- Windows-1252 (CP1252) collation
    col_win1252 VARCHAR(100) COLLATE SQL_Latin1_General_CP1_CI_AS NOT NULL,
    -- Unicode with different collations
    col_unicode_cs NVARCHAR(100) COLLATE Latin1_General_CS_AS NOT NULL,
    col_unicode_ci NVARCHAR(100) COLLATE Latin1_General_CI_AS NOT NULL
);
GO

INSERT INTO dbo.CollationTest (id, col_default, col_case_sensitive, col_case_insensitive, col_binary, col_win1252, col_unicode_cs, col_unicode_ci) VALUES
(1, 'Apple', 'Apple', 'Apple', 'Apple', 'Apple', N'Apple', N'Apple'),
(2, 'apple', 'apple', 'apple', 'apple', 'apple', N'apple', N'apple'),
(3, 'APPLE', 'APPLE', 'APPLE', 'APPLE', 'APPLE', N'APPLE', N'APPLE'),
(4, 'Banana', 'Banana', 'Banana', 'Banana', 'Banana', N'Banana', N'Banana'),
(5, 'café', 'café', 'café', 'café', 'café', N'café', N'café'),
(6, 'CAFÉ', 'CAFÉ', 'CAFÉ', 'CAFÉ', 'CAFÉ', N'CAFÉ', N'CAFÉ'),
(7, 'naïve', 'naïve', 'naïve', 'naïve', 'naïve', N'naïve', N'naïve'),
(8, 'Test123', 'Test123', 'Test123', 'Test123', 'Test123', N'Test123', N'Test123');
GO

PRINT 'Collation test table created';
GO

-- =============================================================================
-- Table 10: MAX data types (VARCHAR(MAX), NVARCHAR(MAX), VARBINARY(MAX))
-- These use PLP (Partially Length-Prefixed) encoding in TDS protocol
-- =============================================================================
IF OBJECT_ID('dbo.MaxTypes', 'U') IS NOT NULL DROP TABLE dbo.MaxTypes;
GO

CREATE TABLE dbo.MaxTypes (
    id INT NOT NULL PRIMARY KEY,
    col_varchar_max VARCHAR(MAX) NULL,
    col_nvarchar_max NVARCHAR(MAX) NULL,
    col_varbinary_max VARBINARY(MAX) NULL
);
GO

-- Insert test data with various sizes and patterns
INSERT INTO dbo.MaxTypes (id, col_varchar_max, col_nvarchar_max, col_varbinary_max) VALUES
-- Row 1: Small values
(1, 'small varchar', N'small nvarchar', 0x0102030405),
-- Row 2: Empty strings
(2, '', N'', 0x),
-- Row 3: NULL values
(3, NULL, NULL, NULL),
-- Row 4: Medium-sized values
(4, 'Medium length varchar value that contains some text', N'Medium length nvarchar with unicode: café naïve résumé', 0xDEADBEEFCAFEBABE0102030405060708090A0B0C0D0E0F),
-- Row 5: Values with special characters
(5, 'varchar with "quotes" and ''apostrophes''', N'nvarchar with unicode: 日本語 中文 한국어 العربية', 0xFF00FF00FF00FF00),
-- Row 6: Single character
(6, 'x', N'y', 0xAB);
GO

-- Add a row with larger data within SQL Server limits
-- VARCHAR(MAX) limit: 8192 bytes for TDS streaming
-- NVARCHAR(MAX) limit: 4096 characters (8192 bytes in UTF-16) for TDS streaming
DECLARE @large_varchar VARCHAR(MAX) = REPLICATE('A', 8000);
DECLARE @large_nvarchar NVARCHAR(MAX) = REPLICATE(N'X', 4000);
DECLARE @large_varbinary VARBINARY(MAX) = CAST(REPLICATE('Z', 8000) AS VARBINARY(MAX));

INSERT INTO dbo.MaxTypes (id, col_varchar_max, col_nvarchar_max, col_varbinary_max)
VALUES (7, @large_varchar, @large_nvarchar, @large_varbinary);
GO

PRINT 'MaxTypes table created (VARCHAR(MAX), NVARCHAR(MAX), VARBINARY(MAX))';
GO

-- =============================================================================
-- Views
-- =============================================================================
CREATE VIEW dbo.LargeTableView AS
SELECT
    id,
    category,
    name,
    value,
    created_date,
    is_active,
    CASE WHEN is_active = 1 THEN 'Active' ELSE 'Inactive' END AS status
FROM dbo.LargeTable;
GO

CREATE VIEW dbo.AllTypesView AS
SELECT
    id,
    col_int,
    col_bigint,
    col_varchar,
    col_nvarchar,
    col_decimal,
    col_date,
    col_datetime2
FROM dbo.AllDataTypes;
GO

CREATE VIEW [SELECT].[VIEW] AS
SELECT
    [COLUMN] AS [SELECT],
    [INDEX] AS [FROM],
    [SELECT] AS [WHERE]
FROM [SELECT].[TABLE];
GO

CREATE VIEW dbo.[SELECT VIEW] AS
SELECT id, [FROM], [WHERE]
FROM dbo.[SELECT];
GO

PRINT 'Views created';
GO

-- =============================================================================
-- Also create tables in master.dbo for existing tests compatibility
-- =============================================================================
USE master;
GO

IF OBJECT_ID('dbo.test', 'U') IS NOT NULL DROP TABLE dbo.test;
GO

CREATE TABLE dbo.test (
    id INT NOT NULL PRIMARY KEY,
    name VARCHAR(50) NOT NULL
);
GO

INSERT INTO dbo.test (id, name) VALUES
(1, 'A'), (2, 'B'), (3, 'C');
GO

PRINT 'master.dbo.test table created';
GO

-- =============================================================================
-- Summary
-- =============================================================================
USE TestDB;
GO

SELECT 'Database: TestDB' AS info;
SELECT 'Schema count:' AS info, COUNT(*) AS count FROM sys.schemas WHERE schema_id > 4 AND name NOT LIKE 'db_%';
SELECT 'Table count:' AS info, COUNT(*) AS count FROM sys.tables;
SELECT 'View count:' AS info, COUNT(*) AS count FROM sys.views;
SELECT 'LargeTable row count:' AS info, COUNT(*) AS count FROM dbo.LargeTable;
GO

PRINT 'Database initialization complete!';
GO
