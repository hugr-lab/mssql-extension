-- Transaction test tables for MSSQL Extension
-- Spec: 001-mssql-transactions
-- These tables are used for testing DML transaction commit/rollback, mssql_scan, and mssql_exec

USE TestDB;
GO

-- =============================================================================
-- Drop existing transaction test tables if they exist
-- =============================================================================
IF OBJECT_ID('dbo.TxTestOrders', 'U') IS NOT NULL DROP TABLE dbo.TxTestOrders;
IF OBJECT_ID('dbo.TxTestProducts', 'U') IS NOT NULL DROP TABLE dbo.TxTestProducts;
IF OBJECT_ID('dbo.TxTestLogs', 'U') IS NOT NULL DROP TABLE dbo.TxTestLogs;
IF OBJECT_ID('dbo.TxTestCounter', 'U') IS NOT NULL DROP TABLE dbo.TxTestCounter;
GO

-- =============================================================================
-- Table 1: TxTestOrders - For INSERT/UPDATE/DELETE transaction tests
-- =============================================================================
CREATE TABLE dbo.TxTestOrders (
    id INT NOT NULL PRIMARY KEY,
    customer NVARCHAR(100) NOT NULL,
    amount DECIMAL(10, 2) NOT NULL DEFAULT 0.00,
    status VARCHAR(20) NOT NULL DEFAULT 'pending',
    created_at DATETIME2 NOT NULL DEFAULT GETDATE()
);
GO

-- Initial data for testing
INSERT INTO dbo.TxTestOrders (id, customer, amount, status) VALUES
    (1, N'Alice', 100.00, 'completed'),
    (2, N'Bob', 200.50, 'pending'),
    (3, N'Charlie', 75.25, 'pending');
GO

PRINT 'TxTestOrders table created with 3 rows';
GO

-- =============================================================================
-- Table 2: TxTestProducts - For combined DML transaction tests
-- =============================================================================
CREATE TABLE dbo.TxTestProducts (
    id INT NOT NULL PRIMARY KEY,
    name NVARCHAR(100) NOT NULL,
    price DECIMAL(10, 2) NOT NULL,
    stock INT NOT NULL DEFAULT 0,
    category VARCHAR(50) NULL
);
GO

-- Initial data for testing
INSERT INTO dbo.TxTestProducts (id, name, price, stock, category) VALUES
    (1, N'Widget A', 19.99, 100, 'Widgets'),
    (2, N'Widget B', 29.99, 50, 'Widgets'),
    (3, N'Gadget X', 99.99, 25, 'Gadgets'),
    (4, N'Gadget Y', 149.99, 10, 'Gadgets'),
    (5, N'Tool Z', 49.99, 75, 'Tools');
GO

PRINT 'TxTestProducts table created with 5 rows';
GO

-- =============================================================================
-- Table 3: TxTestLogs - For mssql_exec tests (no PK for simple logging)
-- =============================================================================
CREATE TABLE dbo.TxTestLogs (
    log_id INT IDENTITY(1,1) PRIMARY KEY,
    log_time DATETIME2 NOT NULL DEFAULT GETDATE(),
    log_level VARCHAR(10) NOT NULL,
    message NVARCHAR(500) NOT NULL
);
GO

PRINT 'TxTestLogs table created';
GO

-- =============================================================================
-- Table 4: TxTestCounter - For testing UPDATE with numeric operations
-- =============================================================================
CREATE TABLE dbo.TxTestCounter (
    id INT NOT NULL PRIMARY KEY,
    name VARCHAR(50) NOT NULL,
    counter INT NOT NULL DEFAULT 0
);
GO

INSERT INTO dbo.TxTestCounter (id, name, counter) VALUES
    (1, 'counter_a', 0),
    (2, 'counter_b', 100);
GO

PRINT 'TxTestCounter table created with 2 rows';
GO

-- =============================================================================
-- Summary
-- =============================================================================
SELECT 'Transaction test tables:' AS info;
SELECT TABLE_NAME,
       (SELECT COUNT(*) FROM INFORMATION_SCHEMA.COLUMNS c WHERE c.TABLE_NAME = t.TABLE_NAME AND c.TABLE_SCHEMA = 'dbo') as column_count
FROM INFORMATION_SCHEMA.TABLES t
WHERE TABLE_SCHEMA = 'dbo' AND TABLE_NAME LIKE 'TxTest%'
ORDER BY TABLE_NAME;
GO

PRINT 'Transaction test tables initialization complete!';
GO
