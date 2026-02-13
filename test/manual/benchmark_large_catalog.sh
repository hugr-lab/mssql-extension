#!/usr/bin/env bash
# =============================================================================
# benchmark_large_catalog.sh
#
# Synthetic benchmark: 200K tables across 2 schemas, 10-30 columns each.
# Tests deferred column loading, schema/table filters, and preload performance.
#
# Usage:
#   ./test/manual/benchmark_large_catalog.sh [--generate] [--benchmark] [--cleanup]
#
# Flags:
#   --generate   Create the BenchmarkDB with 200K tables (takes ~30-60 min)
#   --benchmark  Run the benchmark scenarios (requires generated DB)
#   --cleanup    Drop BenchmarkDB
#   (no flags)   Run both --generate and --benchmark
#
# Environment:
#   MSSQL_TEST_HOST  (default: localhost)
#   MSSQL_TEST_PORT  (default: 1433)
#   MSSQL_TEST_USER  (default: sa)
#   MSSQL_TEST_PASS  (default: TestPassword1)
#   DUCKDB_CLI       (default: ./build/release/duckdb)
#   TABLE_COUNT      (default: 200000)
#   BATCH_SIZE       (default: 500)
# =============================================================================

set -euo pipefail

# --- Configuration ---
HOST="${MSSQL_TEST_HOST:-localhost}"
PORT="${MSSQL_TEST_PORT:-1433}"
USER="${MSSQL_TEST_USER:-sa}"
PASS="${MSSQL_TEST_PASS:-TestPassword1}"
DUCKDB="${DUCKDB_CLI:-./build/release/duckdb}"
DB_NAME="BenchmarkDB"
TABLE_COUNT="${TABLE_COUNT:-200000}"
BATCH_SIZE="${BATCH_SIZE:-500}"

# Tables split across 2 schemas
SCHEMA_A="bench_alpha"
SCHEMA_B="bench_beta"
TABLES_PER_SCHEMA=$((TABLE_COUNT / 2))

RESULTS_FILE="${RESULTS_FILE:-test/manual/benchmark_results_$(date '+%Y%m%d_%H%M%S').txt}"

DSN="Server=${HOST},${PORT};Database=${DB_NAME};User Id=${USER};Password=${PASS}"
MASTER_DSN="Server=${HOST},${PORT};Database=master;User Id=${USER};Password=${PASS}"

SQLCMD="docker exec mssql-dev /opt/mssql-tools18/bin/sqlcmd -S localhost -U ${USER} -P '${PASS}' -C"

# --- Helpers ---
log()  { echo "[$(date '+%H:%M:%S')] $*"; }
time_cmd() {
    local label="$1"; shift
    local start end elapsed
    start=$(date +%s%N)
    "$@"
    end=$(date +%s%N)
    elapsed=$(( (end - start) / 1000000 ))
    echo "  => ${label}: ${elapsed} ms"
}

run_sqlcmd() {
    local db="$1"; shift
    docker exec mssql-dev /opt/mssql-tools18/bin/sqlcmd \
        -S localhost -U "${USER}" -P "${PASS}" -C -d "${db}" -Q "$*" 2>/dev/null
}

run_duckdb() {
    "${DUCKDB}" -c "$1" 2>&1
}

run_duckdb_timed() {
    local label="$1"
    local sql="$2"
    local start end elapsed output
    start=$(date +%s%N)
    output=$("${DUCKDB}" -c "${sql}" 2>&1)
    end=$(date +%s%N)
    elapsed=$(( (end - start) / 1000000 ))
    echo "  => ${label}: ${elapsed} ms"
    if [ -n "${output}" ]; then
        echo "${output}" | head -20
    fi
}

# =============================================================================
# GENERATE: Create BenchmarkDB with 200K tables
# =============================================================================
do_generate() {
    log "=== GENERATE: Creating ${TABLE_COUNT} tables in ${DB_NAME} ==="

    # 1. Create database
    log "Creating database ${DB_NAME}..."
    run_sqlcmd master "
        IF DB_ID('${DB_NAME}') IS NOT NULL
        BEGIN
            ALTER DATABASE [${DB_NAME}] SET SINGLE_USER WITH ROLLBACK IMMEDIATE;
            DROP DATABASE [${DB_NAME}];
        END
        CREATE DATABASE [${DB_NAME}];
    "

    # 2. Create schemas
    log "Creating schemas ${SCHEMA_A}, ${SCHEMA_B}..."
    run_sqlcmd "${DB_NAME}" "
        IF NOT EXISTS (SELECT 1 FROM sys.schemas WHERE name = '${SCHEMA_A}')
            EXEC('CREATE SCHEMA [${SCHEMA_A}]');
        IF NOT EXISTS (SELECT 1 FROM sys.schemas WHERE name = '${SCHEMA_B}')
            EXEC('CREATE SCHEMA [${SCHEMA_B}]');
    "

    # 3. Generate tables using a stored procedure for speed
    log "Creating generator stored procedure..."
    run_sqlcmd "${DB_NAME}" "
        IF OBJECT_ID('dbo.GenerateBenchmarkTables') IS NOT NULL
            DROP PROCEDURE dbo.GenerateBenchmarkTables;
    "

    # Create the proc via a SQL file piped to sqlcmd
    local proc_sql
    proc_sql=$(cat <<'PROCSQL'
CREATE PROCEDURE dbo.GenerateBenchmarkTables
    @schema_name NVARCHAR(128),
    @start_idx INT,
    @end_idx INT
AS
BEGIN
    SET NOCOUNT ON;
    DECLARE @i INT = @start_idx;
    DECLARE @sql NVARCHAR(MAX);
    DECLARE @table_name NVARCHAR(256);
    DECLARE @num_cols INT;
    DECLARE @j INT;

    WHILE @i <= @end_idx
    BEGIN
        SET @table_name = @schema_name + N'.tbl_' + RIGHT('000000' + CAST(@i AS NVARCHAR(10)), 6);
        -- 10-30 columns: deterministic based on table index
        SET @num_cols = 10 + (@i % 21);

        SET @sql = N'CREATE TABLE ' + QUOTENAME(@schema_name) + N'.' +
                   QUOTENAME(N'tbl_' + RIGHT(N'000000' + CAST(@i AS NVARCHAR(10)), 6)) +
                   N' (id INT NOT NULL';

        SET @j = 1;
        WHILE @j <= @num_cols
        BEGIN
            SET @sql = @sql + N', col_' + CAST(@j AS NVARCHAR(10));
            -- Vary column types based on column index
            IF @j % 5 = 0
                SET @sql = @sql + N' DECIMAL(18,4)';
            ELSE IF @j % 4 = 0
                SET @sql = @sql + N' DATETIME2';
            ELSE IF @j % 3 = 0
                SET @sql = @sql + N' INT';
            ELSE IF @j % 2 = 0
                SET @sql = @sql + N' NVARCHAR(200)';
            ELSE
                SET @sql = @sql + N' VARCHAR(100)';

            SET @j = @j + 1;
        END

        SET @sql = @sql + N')';
        EXEC sp_executesql @sql;

        SET @i = @i + 1;
    END
END
PROCSQL
)
    echo "${proc_sql}" | docker exec -i mssql-dev /opt/mssql-tools18/bin/sqlcmd \
        -S localhost -U "${USER}" -P "${PASS}" -C -d "${DB_NAME}" 2>/dev/null

    # 4. Generate tables in batches
    log "Generating ${TABLES_PER_SCHEMA} tables in schema ${SCHEMA_A}..."
    local batch_start=1
    local batch_end
    local total_created=0

    while [ ${batch_start} -le ${TABLES_PER_SCHEMA} ]; do
        batch_end=$((batch_start + BATCH_SIZE - 1))
        if [ ${batch_end} -gt ${TABLES_PER_SCHEMA} ]; then
            batch_end=${TABLES_PER_SCHEMA}
        fi

        run_sqlcmd "${DB_NAME}" "EXEC dbo.GenerateBenchmarkTables '${SCHEMA_A}', ${batch_start}, ${batch_end};"
        total_created=$((total_created + batch_end - batch_start + 1))

        if [ $((total_created % 5000)) -eq 0 ] || [ ${batch_end} -eq ${TABLES_PER_SCHEMA} ]; then
            log "  ${SCHEMA_A}: ${total_created}/${TABLES_PER_SCHEMA} tables created"
        fi

        batch_start=$((batch_end + 1))
    done

    log "Generating ${TABLES_PER_SCHEMA} tables in schema ${SCHEMA_B}..."
    batch_start=1
    total_created=0

    while [ ${batch_start} -le ${TABLES_PER_SCHEMA} ]; do
        batch_end=$((batch_start + BATCH_SIZE - 1))
        if [ ${batch_end} -gt ${TABLES_PER_SCHEMA} ]; then
            batch_end=${TABLES_PER_SCHEMA}
        fi

        run_sqlcmd "${DB_NAME}" "EXEC dbo.GenerateBenchmarkTables '${SCHEMA_B}', ${batch_start}, ${batch_end};"
        total_created=$((total_created + batch_end - batch_start + 1))

        if [ $((total_created % 5000)) -eq 0 ] || [ ${batch_end} -eq ${TABLES_PER_SCHEMA} ]; then
            log "  ${SCHEMA_B}: ${total_created}/${TABLES_PER_SCHEMA} tables created"
        fi

        batch_start=$((batch_end + 1))
    done

    # 5. Verify
    log "Verifying table count..."
    run_sqlcmd "${DB_NAME}" "
        SELECT s.name AS schema_name, COUNT(*) AS table_count
        FROM sys.objects o
        JOIN sys.schemas s ON o.schema_id = s.schema_id
        WHERE o.type = 'U' AND s.name IN ('${SCHEMA_A}', '${SCHEMA_B}')
        GROUP BY s.name;
    "

    # Insert a few rows into one table for SELECT test
    run_sqlcmd "${DB_NAME}" "
        INSERT INTO [${SCHEMA_A}].[tbl_000001] (id, col_1, col_2) VALUES (1, 'hello', N'world');
        INSERT INTO [${SCHEMA_A}].[tbl_000001] (id, col_1, col_2) VALUES (2, 'bench', N'mark');
    "

    log "=== GENERATE complete ==="
}

# =============================================================================
# BENCHMARK: Run performance scenarios
# =============================================================================
do_benchmark_inner() {
    log "=== BENCHMARK: Testing against ${DB_NAME} ==="
    log "Table count: ${TABLE_COUNT} (${TABLES_PER_SCHEMA} per schema)"
    log "Schemas: ${SCHEMA_A}, ${SCHEMA_B}"
    log "DuckDB: ${DUCKDB}"
    echo ""

    # -------------------------------------------------------------------------
    # Scenario 1: Attach + SELECT one table + preload
    # -------------------------------------------------------------------------
    echo "========================================================================"
    echo "SCENARIO 1: Attach (no filters) → SELECT one table → preload"
    echo "========================================================================"
    echo ""

    run_duckdb_timed "1a. ATTACH (no filters)" "
        CREATE SECRET bench_secret (TYPE mssql, HOST '${HOST}', PORT ${PORT}, DATABASE '${DB_NAME}', USER '${USER}', PASSWORD '${PASS}');
        ATTACH '' AS bench (TYPE mssql, SECRET bench_secret);
    "

    run_duckdb_timed "1b. SELECT from one table" "
        CREATE SECRET bench_secret (TYPE mssql, HOST '${HOST}', PORT ${PORT}, DATABASE '${DB_NAME}', USER '${USER}', PASSWORD '${PASS}');
        ATTACH '' AS bench (TYPE mssql, SECRET bench_secret);
        SELECT * FROM bench.${SCHEMA_A}.tbl_000001 LIMIT 1;
    "

    run_duckdb_timed "1c. ATTACH + preload (all schemas)" "
        CREATE SECRET bench_secret (TYPE mssql, HOST '${HOST}', PORT ${PORT}, DATABASE '${DB_NAME}', USER '${USER}', PASSWORD '${PASS}');
        ATTACH '' AS bench (TYPE mssql, SECRET bench_secret);
        SELECT mssql_preload_catalog('bench');
    "

    run_duckdb_timed "1d. ATTACH + preload + SELECT one table" "
        CREATE SECRET bench_secret (TYPE mssql, HOST '${HOST}', PORT ${PORT}, DATABASE '${DB_NAME}', USER '${USER}', PASSWORD '${PASS}');
        ATTACH '' AS bench (TYPE mssql, SECRET bench_secret);
        SELECT mssql_preload_catalog('bench');
        SELECT * FROM bench.${SCHEMA_A}.tbl_000001 LIMIT 1;
    "

    echo ""

    # -------------------------------------------------------------------------
    # Scenario 2: Attach with schema_filter + SELECT one table + preload
    # -------------------------------------------------------------------------
    echo "========================================================================"
    echo "SCENARIO 2: Attach (schema_filter='^${SCHEMA_A}\$') → SELECT → preload"
    echo "========================================================================"
    echo ""

    run_duckdb_timed "2a. ATTACH (schema_filter)" "
        CREATE SECRET bench_secret (TYPE mssql, HOST '${HOST}', PORT ${PORT}, DATABASE '${DB_NAME}', USER '${USER}', PASSWORD '${PASS}');
        ATTACH '' AS bench (TYPE mssql, SECRET bench_secret, schema_filter '^${SCHEMA_A}\$');
    "

    run_duckdb_timed "2b. SELECT from one table (schema-filtered)" "
        CREATE SECRET bench_secret (TYPE mssql, HOST '${HOST}', PORT ${PORT}, DATABASE '${DB_NAME}', USER '${USER}', PASSWORD '${PASS}');
        ATTACH '' AS bench (TYPE mssql, SECRET bench_secret, schema_filter '^${SCHEMA_A}\$');
        SELECT * FROM bench.${SCHEMA_A}.tbl_000001 LIMIT 1;
    "

    run_duckdb_timed "2c. ATTACH + preload (schema-filtered)" "
        CREATE SECRET bench_secret (TYPE mssql, HOST '${HOST}', PORT ${PORT}, DATABASE '${DB_NAME}', USER '${USER}', PASSWORD '${PASS}');
        ATTACH '' AS bench (TYPE mssql, SECRET bench_secret, schema_filter '^${SCHEMA_A}\$');
        SELECT mssql_preload_catalog('bench');
    "

    run_duckdb_timed "2d. ATTACH + preload + SELECT (schema-filtered)" "
        CREATE SECRET bench_secret (TYPE mssql, HOST '${HOST}', PORT ${PORT}, DATABASE '${DB_NAME}', USER '${USER}', PASSWORD '${PASS}');
        ATTACH '' AS bench (TYPE mssql, SECRET bench_secret, schema_filter '^${SCHEMA_A}\$');
        SELECT mssql_preload_catalog('bench');
        SELECT * FROM bench.${SCHEMA_A}.tbl_000001 LIMIT 1;
    "

    echo ""

    # -------------------------------------------------------------------------
    # Scenario 3: Attach with schema+table filter + SHOW ALL TABLES + preload
    # -------------------------------------------------------------------------
    echo "========================================================================"
    echo "SCENARIO 3: Attach (schema+table filter) → SHOW ALL TABLES → preload"
    echo "========================================================================"
    echo ""

    run_duckdb_timed "3a. ATTACH (schema+table filter: tbl_0000[0-1])" "
        CREATE SECRET bench_secret (TYPE mssql, HOST '${HOST}', PORT ${PORT}, DATABASE '${DB_NAME}', USER '${USER}', PASSWORD '${PASS}');
        ATTACH '' AS bench (TYPE mssql, SECRET bench_secret, schema_filter '^${SCHEMA_A}\$', table_filter '^tbl_0000[0-1]');
    "

    run_duckdb_timed "3b. SHOW ALL TABLES (schema+table filtered, ~20 tables)" "
        CREATE SECRET bench_secret (TYPE mssql, HOST '${HOST}', PORT ${PORT}, DATABASE '${DB_NAME}', USER '${USER}', PASSWORD '${PASS}');
        ATTACH '' AS bench (TYPE mssql, SECRET bench_secret, schema_filter '^${SCHEMA_A}\$', table_filter '^tbl_0000[0-1]');
        SELECT COUNT(*) AS visible_tables FROM (SHOW ALL TABLES);
    "

    run_duckdb_timed "3c. Preload (schema+table filtered)" "
        CREATE SECRET bench_secret (TYPE mssql, HOST '${HOST}', PORT ${PORT}, DATABASE '${DB_NAME}', USER '${USER}', PASSWORD '${PASS}');
        ATTACH '' AS bench (TYPE mssql, SECRET bench_secret, schema_filter '^${SCHEMA_A}\$', table_filter '^tbl_0000[0-1]');
        SELECT mssql_preload_catalog('bench');
    "

    run_duckdb_timed "3d. Preload + SHOW ALL TABLES (schema+table filtered)" "
        CREATE SECRET bench_secret (TYPE mssql, HOST '${HOST}', PORT ${PORT}, DATABASE '${DB_NAME}', USER '${USER}', PASSWORD '${PASS}');
        ATTACH '' AS bench (TYPE mssql, SECRET bench_secret, schema_filter '^${SCHEMA_A}\$', table_filter '^tbl_0000[0-1]');
        SELECT mssql_preload_catalog('bench');
        SELECT COUNT(*) AS visible_tables FROM (SHOW ALL TABLES);
    "

    echo ""

    # -------------------------------------------------------------------------
    # Scenario 4: No filters — SHOW ALL TABLES (worst case)
    # -------------------------------------------------------------------------
    echo "========================================================================"
    echo "SCENARIO 4: No filters — SHOW ALL TABLES (worst case, all ${TABLE_COUNT} tables)"
    echo "========================================================================"
    echo ""

    run_duckdb_timed "4a. Preload + SHOW ALL TABLES (no filters, all tables)" "
        CREATE SECRET bench_secret (TYPE mssql, HOST '${HOST}', PORT ${PORT}, DATABASE '${DB_NAME}', USER '${USER}', PASSWORD '${PASS}');
        ATTACH '' AS bench (TYPE mssql, SECRET bench_secret);
        SELECT mssql_preload_catalog('bench');
        SELECT COUNT(*) AS visible_tables FROM (SHOW ALL TABLES);
    "

    run_duckdb_timed "4b. SHOW ALL TABLES without preload (no filters)" "
        CREATE SECRET bench_secret (TYPE mssql, HOST '${HOST}', PORT ${PORT}, DATABASE '${DB_NAME}', USER '${USER}', PASSWORD '${PASS}');
        ATTACH '' AS bench (TYPE mssql, SECRET bench_secret);
        SELECT COUNT(*) AS visible_tables FROM (SHOW ALL TABLES);
    "

    echo ""
    log "=== BENCHMARK complete ==="
}

do_benchmark() {
    log "Results will be saved to: ${RESULTS_FILE}"
    do_benchmark_inner 2>&1 | tee "${RESULTS_FILE}"
    log "Results saved to: ${RESULTS_FILE}"
}

# =============================================================================
# CLEANUP: Drop BenchmarkDB
# =============================================================================
do_cleanup() {
    log "=== CLEANUP: Dropping ${DB_NAME} ==="
    run_sqlcmd master "
        IF DB_ID('${DB_NAME}') IS NOT NULL
        BEGIN
            ALTER DATABASE [${DB_NAME}] SET SINGLE_USER WITH ROLLBACK IMMEDIATE;
            DROP DATABASE [${DB_NAME}];
        END
    "
    log "=== CLEANUP complete ==="
}

# =============================================================================
# Main
# =============================================================================
main() {
    local do_gen=false
    local do_bench=false
    local do_clean=false

    if [ $# -eq 0 ]; then
        do_gen=true
        do_bench=true
    fi

    for arg in "$@"; do
        case "${arg}" in
            --generate)  do_gen=true ;;
            --benchmark) do_bench=true ;;
            --cleanup)   do_clean=true ;;
            --help|-h)
                echo "Usage: $0 [--generate] [--benchmark] [--cleanup]"
                echo ""
                echo "  --generate   Create BenchmarkDB with ${TABLE_COUNT} tables"
                echo "  --benchmark  Run benchmark scenarios"
                echo "  --cleanup    Drop BenchmarkDB"
                echo "  (no flags)   Run generate + benchmark"
                echo ""
                echo "Environment: MSSQL_TEST_HOST, MSSQL_TEST_PORT, MSSQL_TEST_USER, MSSQL_TEST_PASS"
                echo "             DUCKDB_CLI, TABLE_COUNT (default: 200000), BATCH_SIZE (default: 500)"
                exit 0
                ;;
            *)
                echo "Unknown flag: ${arg}. Use --help for usage."
                exit 1
                ;;
        esac
    done

    if ${do_gen}; then
        do_generate
        echo ""
    fi

    if ${do_bench}; then
        do_benchmark
    fi

    if ${do_clean}; then
        do_cleanup
    fi
}

main "$@"
