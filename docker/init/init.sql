-- Initialize test database for DuckDB MSSQL Extension development
-- This script creates the test database and tables used for extension testing

USE master;
GO

-- Create test database if it doesn't exist
IF NOT EXISTS (SELECT name FROM sys.databases WHERE name = 'TestDB')
BEGIN
    CREATE DATABASE TestDB;
    PRINT 'TestDB created';
END
GO

USE TestDB;
GO

-- Test table with single-column primary key
-- Used to verify basic rowid generation (scalar rowid)
IF OBJECT_ID('dbo.TestSimplePK', 'U') IS NOT NULL
    DROP TABLE dbo.TestSimplePK;
GO

CREATE TABLE dbo.TestSimplePK (
    id INT NOT NULL PRIMARY KEY,
    name NVARCHAR(100) NOT NULL,
    value DECIMAL(10, 2) NULL,
    created_at DATETIME2 DEFAULT GETDATE()
);
GO

-- Insert sample data for TestSimplePK
INSERT INTO dbo.TestSimplePK (id, name, value) VALUES
    (1, 'First Record', 100.50),
    (2, 'Second Record', 200.75),
    (3, 'Third Record', NULL),
    (4, 'Fourth Record', 400.00),
    (5, 'Fifth Record', 500.25);
GO

PRINT 'TestSimplePK table created with sample data';
GO

-- Test table with composite primary key
-- Used to verify STRUCT-based rowid generation for composite keys
IF OBJECT_ID('dbo.TestCompositePK', 'U') IS NOT NULL
    DROP TABLE dbo.TestCompositePK;
GO

CREATE TABLE dbo.TestCompositePK (
    region_id INT NOT NULL,
    product_id INT NOT NULL,
    quantity INT NOT NULL,
    unit_price DECIMAL(10, 2) NOT NULL,
    order_date DATE NOT NULL,
    CONSTRAINT PK_TestCompositePK PRIMARY KEY (region_id, product_id)
);
GO

-- Insert sample data for TestCompositePK
INSERT INTO dbo.TestCompositePK (region_id, product_id, quantity, unit_price, order_date) VALUES
    (1, 100, 10, 25.50, '2024-01-15'),
    (1, 101, 5, 50.00, '2024-01-16'),
    (1, 102, 20, 12.75, '2024-01-17'),
    (2, 100, 15, 25.50, '2024-01-15'),
    (2, 101, 8, 50.00, '2024-01-18'),
    (3, 100, 25, 25.50, '2024-01-19'),
    (3, 102, 12, 12.75, '2024-01-20');
GO

PRINT 'TestCompositePK table created with sample data';
GO

-- Verify data
SELECT 'TestSimplePK row count:' AS info, COUNT(*) AS count FROM dbo.TestSimplePK;
SELECT 'TestCompositePK row count:' AS info, COUNT(*) AS count FROM dbo.TestCompositePK;
GO

PRINT 'Database initialization complete!';
GO
