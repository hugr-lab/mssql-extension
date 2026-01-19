-- smoke_test.sql - SQL Server integration smoke test
-- This script tests the extension's ability to connect to SQL Server and perform basic operations
--
-- Prerequisites:
--   - Extension must be loaded
--   - SQL Server must be running and accessible
--   - Environment variables must be set:
--     MSSQL_TEST_HOST, MSSQL_TEST_PORT, MSSQL_TEST_USER, MSSQL_TEST_PASS, MSSQL_TEST_DB

-- ============================================================================
-- Test 1: Create secret and attach to SQL Server
-- ============================================================================
.print '=== Test 1: Creating secret and attaching to SQL Server ==='

CREATE SECRET smoke_test_secret (
    TYPE mssql,
    host getenv('MSSQL_TEST_HOST'),
    port CAST(getenv('MSSQL_TEST_PORT') AS INTEGER),
    database getenv('MSSQL_TEST_DB'),
    user getenv('MSSQL_TEST_USER'),
    password getenv('MSSQL_TEST_PASS')
);

ATTACH '' AS mssql_smoke (TYPE mssql, SECRET smoke_test_secret);

.print 'Attached to SQL Server successfully'

-- ============================================================================
-- Test 2: Basic SELECT query
-- ============================================================================
.print ''
.print '=== Test 2: Basic SELECT query ==='

SELECT 1 AS smoke_test_value;

.print 'SELECT query successful'

-- ============================================================================
-- Test 3: Query system information via mssql_scan
-- ============================================================================
.print ''
.print '=== Test 3: Query system information ==='

FROM mssql_scan('mssql_smoke', 'SELECT @@VERSION AS sql_server_version');

.print 'System query successful'

-- ============================================================================
-- Test 4: List schemas via DuckDB meta functions
-- ============================================================================
.print ''
.print '=== Test 4: List schemas ==='

SELECT schema_name FROM duckdb_schemas() WHERE database_name = 'mssql_smoke' LIMIT 5;

.print 'Schema listing successful'

-- ============================================================================
-- Test 5: DDL via DuckDB catalog - CREATE TABLE
-- ============================================================================
.print ''
.print '=== Test 5: DDL via DuckDB catalog ==='

-- Cleanup any existing test table using mssql_exec (SQL Server specific IF EXISTS)
SELECT mssql_exec('mssql_smoke', 'IF OBJECT_ID(''dbo.ci_smoke_test'', ''U'') IS NOT NULL DROP TABLE dbo.ci_smoke_test') AS cleanup;

-- Create table using DuckDB DDL syntax through catalog
CREATE TABLE mssql_smoke.dbo.ci_smoke_test (id INTEGER, name VARCHAR(100), value INTEGER);

.print 'Table created via DuckDB catalog'

-- ============================================================================
-- Test 6: DML via DuckDB catalog - INSERT
-- ============================================================================
.print ''
.print '=== Test 6: DML via DuckDB catalog ==='

-- Insert test data using DuckDB INSERT syntax
INSERT INTO mssql_smoke.dbo.ci_smoke_test (id, name, value) VALUES (1, 'test_row_1', 42), (2, 'test_row_2', 100);

.print 'Data inserted via DuckDB catalog'

-- ============================================================================
-- Test 7: Verify inserted data via DuckDB query
-- ============================================================================
.print ''
.print '=== Test 7: Verify inserted data ==='

FROM mssql_smoke.dbo.ci_smoke_test ORDER BY id;

.print 'Data verification successful'

-- ============================================================================
-- Test 8: DDL cleanup via DuckDB catalog - DROP TABLE
-- ============================================================================
.print ''
.print '=== Test 8: Cleanup ==='

DROP TABLE mssql_smoke.dbo.ci_smoke_test;

DETACH mssql_smoke;
DROP SECRET smoke_test_secret;

.print 'Cleanup completed'

-- ============================================================================
-- Final result
-- ============================================================================
.print ''
.print '============================================'
.print '=== ALL SMOKE TESTS PASSED SUCCESSFULLY ==='
.print '============================================'
