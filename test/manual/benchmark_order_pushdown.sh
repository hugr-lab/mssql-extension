#!/usr/bin/env bash
# =============================================================================
# benchmark_order_pushdown.sh
#
# Benchmark: ORDER BY / TOP N pushdown vs DuckDB-side sorting.
# Creates OrderBenchDB with a 1M-row "orders" table with multiple indexes,
# simulating a real-world e-commerce order management scenario.
#
# Usage:
#   ./test/manual/benchmark_order_pushdown.sh [--generate] [--benchmark] [--cleanup]
#
# Flags:
#   --generate   Create OrderBenchDB with populated tables and indexes
#   --benchmark  Run benchmark scenarios (requires generated DB)
#   --cleanup    Drop OrderBenchDB
#   (no flags)   Run both --generate and --benchmark
#
# Environment:
#   MSSQL_TEST_HOST  (default: localhost)
#   MSSQL_TEST_PORT  (default: 1433)
#   MSSQL_TEST_USER  (default: sa)
#   MSSQL_TEST_PASS  (default: TestPassword1)
#   DUCKDB_CLI       (default: ./build/release/duckdb)
#   ROW_COUNT        (default: 1000000)
#   ITERATIONS       (default: 3)
# =============================================================================

set -euo pipefail

# --- Configuration ---
HOST="${MSSQL_TEST_HOST:-localhost}"
PORT="${MSSQL_TEST_PORT:-1433}"
USER="${MSSQL_TEST_USER:-sa}"
PASS="${MSSQL_TEST_PASS:-TestPassword1}"
DUCKDB="${DUCKDB_CLI:-./build/release/duckdb}"
DB_NAME="OrderBenchDB"
ROW_COUNT="${ROW_COUNT:-100000000}"
ITERATIONS="${ITERATIONS:-3}"

RESULTS_FILE="${RESULTS_FILE:-test/manual/benchmark_order_pushdown_results_$(date '+%Y%m%d_%H%M%S').txt}"

DSN="Server=${HOST},${PORT};Database=${DB_NAME};User Id=${USER};Password=${PASS}"

# --- Helpers ---
log()  { echo "[$(date '+%H:%M:%S')] $*"; }

run_sqlcmd() {
    local db="$1"; shift
    echo "$*" | docker exec -i mssql-dev bash -c \
        '/opt/mssql-tools18/bin/sqlcmd -S localhost -U sa -P "$MSSQL_TEST_PASS" -C -d "'"${db}"'"' 2>/dev/null
}

# Long-running sqlcmd (no timeout, for index creation on 100M+ rows)
run_sqlcmd_long() {
    local db="$1"; shift
    echo "$*" | docker exec -i mssql-dev bash -c \
        '/opt/mssql-tools18/bin/sqlcmd -S localhost -U sa -P "$MSSQL_TEST_PASS" -C -d "'"${db}"'" -t 0' 2>/dev/null
}

run_duckdb() {
    "${DUCKDB}" -c "$1" 2>&1
}

# Run a DuckDB query multiple times and report min/avg/max
# Uses heredoc to avoid printf interpreting backslashes in passwords (e.g. \0 in TPassw0rd)
run_duckdb_bench() {
    local label="$1"
    local sql="$2"
    local iters="${3:-${ITERATIONS}}"
    local timings=()
    local output=""

    for ((i=1; i<=iters; i++)); do
        local start end elapsed
        start=$(date +%s%N)
        output=$("${DUCKDB}" 2>&1 <<DUCKDB_EOF
${sql}
DUCKDB_EOF
)
        end=$(date +%s%N)
        elapsed=$(( (end - start) / 1000000 ))
        timings+=("${elapsed}")
    done

    # Compute min/avg/max
    local min=${timings[0]} max=${timings[0]} sum=0
    for t in "${timings[@]}"; do
        sum=$((sum + t))
        if (( t < min )); then min=$t; fi
        if (( t > max )); then max=$t; fi
    done
    local avg=$((sum / iters))

    printf "  %-55s  min=%5d  avg=%5d  max=%5d ms  (n=%d)\n" "${label}" "${min}" "${avg}" "${max}" "${iters}"
    if [ -n "${output}" ]; then
        echo "${output}" | head -5 | sed 's/^/    /'
    fi
}

# =============================================================================
# GENERATE: Create OrderBenchDB with realistic data
# =============================================================================
do_generate() {
    log "=== GENERATE: Creating ${DB_NAME} with ${ROW_COUNT} rows ==="

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

    # 2. Create the orders table — realistic e-commerce schema
    log "Creating orders table..."
    run_sqlcmd "${DB_NAME}" "
        CREATE TABLE dbo.orders (
            order_id        INT NOT NULL IDENTITY(1,1) PRIMARY KEY,
            customer_id     INT NOT NULL,
            order_date      DATETIME2 NOT NULL,
            ship_date       DATETIME2 NULL,
            status          VARCHAR(20) NOT NULL,
            region          VARCHAR(50) NOT NULL,
            category        VARCHAR(50) NOT NULL,
            product_name    NVARCHAR(200) NOT NULL,
            quantity        INT NOT NULL,
            unit_price      DECIMAL(12,2) NOT NULL,
            total_amount    DECIMAL(14,2) NOT NULL,
            discount        DECIMAL(5,2) NULL,
            notes           NVARCHAR(500) NULL,
            created_at      DATETIME2 NOT NULL DEFAULT SYSUTCDATETIME(),
            updated_at      DATETIME2 NULL
        );
    "

    # 3. Populate data BEFORE creating indexes (bulk insert is faster without indexes)
    log "Populating ${ROW_COUNT} rows (indexes will be created after)..."

    # Create a helper proc optimized for large-scale inserts:
    # - Uses TABLOCK for minimal logging
    # - Computes total_amount inline (no separate UPDATE pass)
    # - Uses modular arithmetic on IDENTITY for deterministic variety (faster than NEWID())
    # - Large batch size (500K) with triple cross join for row generation
    run_sqlcmd "${DB_NAME}" "
        IF OBJECT_ID('dbo.PopulateOrders') IS NOT NULL DROP PROCEDURE dbo.PopulateOrders;
    "

    local proc_sql
    proc_sql=$(cat <<'PROCSQL'
CREATE PROCEDURE dbo.PopulateOrders @total_rows BIGINT
AS
BEGIN
    SET NOCOUNT ON;

    DECLARE @batch_size INT = 500000;
    DECLARE @inserted BIGINT = 0;

    -- Numbers CTE approach: generate rows without cross-joining system tables
    WHILE @inserted < @total_rows
    BEGIN
        DECLARE @this_batch INT = @batch_size;
        IF @inserted + @this_batch > @total_rows
            SET @this_batch = CAST(@total_rows - @inserted AS INT);

        -- Use a numbers CTE (no dependency on system table sizes)
        ;WITH
        E1(N) AS (SELECT 1 UNION ALL SELECT 1 UNION ALL SELECT 1 UNION ALL SELECT 1 UNION ALL
                   SELECT 1 UNION ALL SELECT 1 UNION ALL SELECT 1 UNION ALL SELECT 1 UNION ALL
                   SELECT 1 UNION ALL SELECT 1),                          -- 10
        E2(N) AS (SELECT 1 FROM E1 a CROSS JOIN E1 b),                    -- 100
        E3(N) AS (SELECT 1 FROM E2 a CROSS JOIN E2 b),                    -- 10,000
        E4(N) AS (SELECT 1 FROM E3 a CROSS JOIN E2 b),                    -- 1,000,000
        Nums AS (SELECT TOP (@this_batch) ROW_NUMBER() OVER (ORDER BY (SELECT NULL)) AS n FROM E4)
        INSERT INTO dbo.orders WITH (TABLOCK)
            (customer_id, order_date, ship_date, status, region, category,
             product_name, quantity, unit_price, total_amount, discount, notes)
        SELECT
            ABS(CHECKSUM(NEWID())) % 100000 + 1,
            DATEADD(DAY, -(ABS(CHECKSUM(NEWID())) % 1095), GETDATE()),
            CASE WHEN n % 10 < 7
                 THEN DATEADD(DAY, -(ABS(CHECKSUM(NEWID())) % 1000), GETDATE())
                 ELSE NULL END,
            CASE ABS(CHECKSUM(NEWID())) % 6
                WHEN 0 THEN 'Pending' WHEN 1 THEN 'Processing' WHEN 2 THEN 'Shipped'
                WHEN 3 THEN 'Delivered' WHEN 4 THEN 'Cancelled' ELSE 'Returned' END,
            CASE ABS(CHECKSUM(NEWID())) % 5
                WHEN 0 THEN 'North America' WHEN 1 THEN 'Europe' WHEN 2 THEN 'Asia Pacific'
                WHEN 3 THEN 'Latin America' ELSE 'Middle East' END,
            CASE ABS(CHECKSUM(NEWID())) % 8
                WHEN 0 THEN 'Electronics' WHEN 1 THEN 'Clothing' WHEN 2 THEN 'Food' WHEN 3 THEN 'Furniture'
                WHEN 4 THEN 'Software' WHEN 5 THEN 'Books' WHEN 6 THEN 'Sports' ELSE 'Automotive' END,
            N'Product-' + CAST(ABS(CHECKSUM(NEWID())) % 50000 + 1 AS NVARCHAR(10)),
            n % 100 + 1,                                                            -- quantity: 1-101
            CAST(n % 900 + 10 AS DECIMAL(12,2)) + (n % 100) * 0.01,                  -- unit_price: 10.00-909.99
            CAST(n % 100 + 1 AS BIGINT) * (n % 900 + 10) + (n % 100) * 0.01,        -- total_amount (approx qty*price)
            CASE WHEN n % 3 = 0 THEN CAST(n % 30 AS DECIMAL(5,2)) + 0.50
                 ELSE NULL END,                                                       -- discount
            CASE WHEN n % 5 = 0
                 THEN N'Note for order ' + CAST(@inserted + n AS NVARCHAR(20))
                 ELSE NULL END
        FROM Nums;

        SET @inserted = @inserted + @this_batch;
        RAISERROR('  Inserted %I64d / %I64d rows', 0, 1, @inserted, @total_rows) WITH NOWAIT;
    END
END
PROCSQL
)
    echo "${proc_sql}" | docker exec -i mssql-dev bash -c '/opt/mssql-tools18/bin/sqlcmd -S localhost -U sa -P "$MSSQL_TEST_PASS" -C -d OrderBenchDB' 2>/dev/null

    docker exec mssql-dev bash -c "/opt/mssql-tools18/bin/sqlcmd -S localhost -U sa -P \"\$MSSQL_TEST_PASS\" -C -d ${DB_NAME} -Q \"EXEC dbo.PopulateOrders ${ROW_COUNT};\" -t 0" 2>&1 | while IFS= read -r line; do
        if [[ "${line}" == *"Inserted"* ]]; then
            log "${line}"
        fi
    done

    # 4. Create indexes AFTER bulk insert (much faster than inserting with indexes)
    log "Creating indexes (this may take several minutes on 100M rows)..."
    run_sqlcmd_long "${DB_NAME}" "CREATE INDEX IX_orders_customer   ON dbo.orders(customer_id, order_date DESC);"
    log "  IX_orders_customer done"
    run_sqlcmd_long "${DB_NAME}" "CREATE INDEX IX_orders_date        ON dbo.orders(order_date DESC);"
    log "  IX_orders_date done"
    run_sqlcmd_long "${DB_NAME}" "CREATE INDEX IX_orders_status_date ON dbo.orders(status, order_date DESC);"
    log "  IX_orders_status_date done"
    run_sqlcmd_long "${DB_NAME}" "CREATE INDEX IX_orders_region_cat  ON dbo.orders(region, category, total_amount DESC);"
    log "  IX_orders_region_cat done"
    run_sqlcmd_long "${DB_NAME}" "CREATE INDEX IX_orders_total       ON dbo.orders(total_amount DESC);"
    log "  IX_orders_total done"
    run_sqlcmd_long "${DB_NAME}" "SET QUOTED_IDENTIFIER ON; CREATE INDEX IX_orders_ship_date ON dbo.orders(ship_date) WHERE ship_date IS NOT NULL;"
    log "  IX_orders_ship_date done"

    # 5. Update statistics for the indexes
    log "Updating statistics..."
    run_sqlcmd_long "${DB_NAME}" "UPDATE STATISTICS dbo.orders WITH FULLSCAN;"

    # 6. Verify
    log "Verifying..."
    run_sqlcmd "${DB_NAME}" "
        SELECT COUNT(*) AS total_rows FROM dbo.orders;
        SELECT
            i.name AS index_name,
            i.type_desc,
            STUFF((SELECT ', ' + c.name
                   FROM sys.index_columns ic
                   JOIN sys.columns c ON c.object_id = ic.object_id AND c.column_id = ic.column_id
                   WHERE ic.object_id = i.object_id AND ic.index_id = i.index_id
                   ORDER BY ic.key_ordinal
                   FOR XML PATH('')), 1, 2, '') AS columns
        FROM sys.indexes i
        WHERE i.object_id = OBJECT_ID('dbo.orders') AND i.type > 0
        ORDER BY i.index_id;
    "

    log "=== GENERATE complete ==="
}

# =============================================================================
# BENCHMARK: Compare pushdown ON vs OFF
# =============================================================================
do_benchmark_inner() {
    log "=== BENCHMARK: ORDER BY / TOP N pushdown (100M rows) ==="
    log "Database: ${DB_NAME} | Rows: ~${ROW_COUNT} | Iterations: ${ITERATIONS}"
    log "DuckDB: ${DUCKDB}"
    echo ""

    # Common ATTACH SQL — .output /dev/null discards large result sets
    local ATTACH_OFF
    ATTACH_OFF="ATTACH '${DSN}' AS bench (TYPE mssql);
.output /dev/null
"
    local ATTACH_ON
    ATTACH_ON="ATTACH '${DSN}' AS bench (TYPE mssql, order_pushdown true);
.output /dev/null
"

    # =========================================================================
    echo "========================================================================"
    echo "SCENARIO 1: TOP 100 — most common real-world pattern (indexed column)"
    echo "  SELECT order_id, customer_id, order_date, total_amount"
    echo "  FROM orders ORDER BY order_date DESC LIMIT 100"
    echo "========================================================================"
    echo ""

    local SQL_S1="SELECT order_id, customer_id, order_date, total_amount FROM bench.dbo.orders ORDER BY order_date DESC LIMIT 100;"

    run_duckdb_bench "1a. Pushdown OFF (scan 100M rows, sort, take 100)" \
        "${ATTACH_OFF} ${SQL_S1}"

    run_duckdb_bench "1b. Pushdown ON  (SELECT TOP 100 ... ORDER BY, IX_orders_date)" \
        "${ATTACH_ON} ${SQL_S1}"

    echo ""

    # =========================================================================
    echo "========================================================================"
    echo "SCENARIO 2: TOP 50 with WHERE filter (index seek + TOP)"
    echo "  SELECT order_id, customer_id, total_amount"
    echo "  FROM orders WHERE status = 'Shipped'"
    echo "  ORDER BY order_date DESC LIMIT 50"
    echo "========================================================================"
    echo ""

    local SQL_S2="SELECT order_id, customer_id, total_amount FROM bench.dbo.orders WHERE status = 'Shipped' ORDER BY order_date DESC LIMIT 50;"

    run_duckdb_bench "2a. Pushdown OFF (scan + filter ~17M rows + sort + limit)" \
        "${ATTACH_OFF} ${SQL_S2}"

    run_duckdb_bench "2b. Pushdown ON  (WHERE + TOP 50 ORDER BY, IX_orders_status_date)" \
        "${ATTACH_ON} ${SQL_S2}"

    echo ""

    # =========================================================================
    echo "========================================================================"
    echo "SCENARIO 3: TOP 20 on non-output column (projection pruning)"
    echo "  SELECT order_id, customer_id, product_name"
    echo "  FROM orders ORDER BY total_amount DESC LIMIT 20"
    echo "  (total_amount used for ORDER only — pruned from scan after pushdown)"
    echo "========================================================================"
    echo ""

    local SQL_S3="SELECT order_id, customer_id, product_name FROM bench.dbo.orders ORDER BY total_amount DESC LIMIT 20;"

    run_duckdb_bench "3a. Pushdown OFF (scan 100M rows + total_amount, sort, take 20)" \
        "${ATTACH_OFF} ${SQL_S3}"

    run_duckdb_bench "3b. Pushdown ON  (TOP 20 ORDER BY, total_amount pruned)" \
        "${ATTACH_ON} ${SQL_S3}"

    echo ""

    # =========================================================================
    echo "========================================================================"
    echo "SCENARIO 4: TOP 10000 — large LIMIT, streaming benefit"
    echo "  SELECT order_id, customer_id, order_date, status, total_amount"
    echo "  FROM orders ORDER BY order_date DESC LIMIT 10000"
    echo "========================================================================"
    echo ""

    local SQL_S4="SELECT order_id, customer_id, order_date, status, total_amount FROM bench.dbo.orders ORDER BY order_date DESC LIMIT 10000;"

    run_duckdb_bench "4a. Pushdown OFF (scan 100M, sort, take 10K)" \
        "${ATTACH_OFF} ${SQL_S4}"

    run_duckdb_bench "4b. Pushdown ON  (SELECT TOP 10000 ... ORDER BY)" \
        "${ATTACH_ON} ${SQL_S4}"

    echo ""

    # =========================================================================
    echo "========================================================================"
    echo "SCENARIO 5: Multi-column ORDER BY + LIMIT (composite index)"
    echo "  SELECT order_id, region, category, total_amount"
    echo "  FROM orders ORDER BY region, category, total_amount DESC LIMIT 1000"
    echo "========================================================================"
    echo ""

    local SQL_S5="SELECT order_id, region, category, total_amount FROM bench.dbo.orders ORDER BY region ASC, category ASC, total_amount DESC LIMIT 1000;"

    run_duckdb_bench "5a. Pushdown OFF (scan 100M, multi-col sort, take 1K)" \
        "${ATTACH_OFF} ${SQL_S5}"

    run_duckdb_bench "5b. Pushdown ON  (TOP 1000 ORDER BY, IX_orders_region_cat)" \
        "${ATTACH_ON} ${SQL_S5}"

    echo ""

    # =========================================================================
    echo "========================================================================"
    echo "SCENARIO 6: ORDER BY non-output column, no LIMIT (projection pruning)"
    echo "  SELECT order_id, customer_id"
    echo "  FROM orders WHERE status = 'Cancelled'"
    echo "  ORDER BY total_amount DESC"
    echo "  (total_amount only for sorting — should be pruned; ~17M row subset)"
    echo "========================================================================"
    echo ""

    local SQL_S6="SELECT order_id, customer_id FROM bench.dbo.orders WHERE status = 'Cancelled' ORDER BY total_amount DESC;"

    run_duckdb_bench "6a. Pushdown OFF (scan + filter + sort on total_amount)" \
        "${ATTACH_OFF} ${SQL_S6}"

    run_duckdb_bench "6b. Pushdown ON  (ORDER BY pushed, total_amount pruned)" \
        "${ATTACH_ON} ${SQL_S6}"

    echo ""

    # =========================================================================
    echo "========================================================================"
    echo "SCENARIO 7: No usable index (ORDER BY notes — no index)"
    echo "  SELECT order_id, notes FROM orders"
    echo "  WHERE notes IS NOT NULL ORDER BY notes ASC LIMIT 100"
    echo "  (pushdown still avoids DuckDB sort, but SQL Server does full sort)"
    echo "========================================================================"
    echo ""

    local SQL_S7="SELECT order_id, notes FROM bench.dbo.orders WHERE notes IS NOT NULL ORDER BY notes ASC LIMIT 100;"

    run_duckdb_bench "7a. Pushdown OFF (scan + filter + sort in DuckDB)" \
        "${ATTACH_OFF} ${SQL_S7}"

    run_duckdb_bench "7b. Pushdown ON  (SQL Server sorts ~20M rows, no index)" \
        "${ATTACH_ON} ${SQL_S7}"

    echo ""

    # =========================================================================
    echo "========================================================================"
    echo "SCENARIO 8: TOP 100 by customer (high cardinality seek)"
    echo "  SELECT order_id, order_date, total_amount"
    echo "  FROM orders WHERE customer_id = 42"
    echo "  ORDER BY order_date DESC LIMIT 100"
    echo "========================================================================"
    echo ""

    local SQL_S8="SELECT order_id, order_date, total_amount FROM bench.dbo.orders WHERE customer_id = 42 ORDER BY order_date DESC LIMIT 100;"

    run_duckdb_bench "8a. Pushdown OFF (scan 100M, filter ~1K rows, sort, limit)" \
        "${ATTACH_OFF} ${SQL_S8}"

    run_duckdb_bench "8b. Pushdown ON  (TOP 100, IX_orders_customer seek)" \
        "${ATTACH_ON} ${SQL_S8}"

    echo ""

    # =========================================================================
    echo "========================================================================"
    echo "SCENARIO 9: Plain JOIN + ORDER BY + LIMIT (pushdown blocked by JOIN)"
    echo "  Local CTAS from MSSQL aggregation, then JOIN + ORDER BY + LIMIT"
    echo "  JOIN blocks ORDER BY pushdown — both OFF/ON should perform similarly."
    echo "========================================================================"
    echo ""

    local CTAS_SETUP="CREATE TABLE customer_summary AS
    SELECT customer_id, COUNT(*) AS order_count,
           CAST(SUM(total_amount) AS DECIMAL(18,2)) AS total_spent
    FROM bench.dbo.orders GROUP BY customer_id;"

    local SQL_S9="SELECT o.order_id, o.order_date, o.total_amount, c.order_count, c.total_spent
FROM bench.dbo.orders o JOIN customer_summary c ON o.customer_id = c.customer_id
ORDER BY o.total_amount DESC LIMIT 100;"

    run_duckdb_bench "9a. Pushdown OFF (JOIN blocks pushdown — baseline)" \
        "${ATTACH_OFF} ${CTAS_SETUP} ${SQL_S9}"

    run_duckdb_bench "9b. Pushdown ON  (JOIN blocks pushdown — same perf)" \
        "${ATTACH_ON} ${CTAS_SETUP} ${SQL_S9}"

    echo ""

    # =========================================================================
    echo "========================================================================"
    echo "SCENARIO 10: CTE pre-sort + JOIN (CTE inner query may get pushdown)"
    echo "  CTE pre-sorts/filters MSSQL side with ORDER BY + LIMIT,"
    echo "  then joins locally against customer_summary."
    echo "========================================================================"
    echo ""

    local SQL_S10="WITH top_orders AS (
    SELECT order_id, customer_id, order_date, total_amount
    FROM bench.dbo.orders ORDER BY total_amount DESC LIMIT 1000
)
SELECT t.order_id, t.order_date, t.total_amount, c.order_count, c.total_spent
FROM top_orders t JOIN customer_summary c ON t.customer_id = c.customer_id;"

    run_duckdb_bench "10a. Pushdown OFF (CTE not pushed, scan 100M + sort)" \
        "${ATTACH_OFF} ${CTAS_SETUP} ${SQL_S10}"

    run_duckdb_bench "10b. Pushdown ON  (CTE TOP 1000 pushed to SQL Server)" \
        "${ATTACH_ON} ${CTAS_SETUP} ${SQL_S10}"

    echo ""

    # =========================================================================
    echo "========================================================================"
    echo "SCENARIO 11: CTE AS NOT MATERIALIZED + JOIN (DuckDB inlines CTE)"
    echo "  DuckDB inlines the CTE (default behavior) — pushdown depends on"
    echo "  whether the optimizer can see through to the MSSQL scan."
    echo "========================================================================"
    echo ""

    local SQL_S11="WITH top_orders AS NOT MATERIALIZED (
    SELECT order_id, customer_id, order_date, total_amount
    FROM bench.dbo.orders ORDER BY total_amount DESC LIMIT 1000
)
SELECT t.order_id, t.order_date, t.total_amount, c.order_count, c.total_spent
FROM top_orders t JOIN customer_summary c ON t.customer_id = c.customer_id;"

    run_duckdb_bench "11a. Pushdown OFF (NOT MATERIALIZED, inlined CTE)" \
        "${ATTACH_OFF} ${CTAS_SETUP} ${SQL_S11}"

    run_duckdb_bench "11b. Pushdown ON  (NOT MATERIALIZED, inlined CTE)" \
        "${ATTACH_ON} ${CTAS_SETUP} ${SQL_S11}"

    echo ""

    # =========================================================================
    echo "========================================================================"
    echo "SCENARIO 12: CTE AS MATERIALIZED + JOIN (DuckDB materializes first)"
    echo "  DuckDB materializes the CTE first, then joins."
    echo "  ORDER BY pushdown applies inside the materialized CTE."
    echo "========================================================================"
    echo ""

    local SQL_S12="WITH top_orders AS MATERIALIZED (
    SELECT order_id, customer_id, order_date, total_amount
    FROM bench.dbo.orders ORDER BY total_amount DESC LIMIT 1000
)
SELECT t.order_id, t.order_date, t.total_amount, c.order_count, c.total_spent
FROM top_orders t JOIN customer_summary c ON t.customer_id = c.customer_id;"

    run_duckdb_bench "12a. Pushdown OFF (MATERIALIZED CTE, no pushdown)" \
        "${ATTACH_OFF} ${CTAS_SETUP} ${SQL_S12}"

    run_duckdb_bench "12b. Pushdown ON  (MATERIALIZED CTE, pushdown inside)" \
        "${ATTACH_ON} ${CTAS_SETUP} ${SQL_S12}"

    echo ""
    log "=== BENCHMARK complete ==="
}

do_benchmark() {
    log "Results will be saved to: ${RESULTS_FILE}"
    do_benchmark_inner 2>&1 | tee "${RESULTS_FILE}"
    log "Results saved to: ${RESULTS_FILE}"
}

# =============================================================================
# CLEANUP: Drop OrderBenchDB
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
                echo "  --generate   Create ${DB_NAME} with ${ROW_COUNT}-row orders table + indexes"
                echo "  --benchmark  Run ORDER BY pushdown benchmark (ON vs OFF)"
                echo "  --cleanup    Drop ${DB_NAME}"
                echo "  (no flags)   Run generate + benchmark"
                echo ""
                echo "Environment: MSSQL_TEST_HOST, MSSQL_TEST_PORT, MSSQL_TEST_USER, MSSQL_TEST_PASS"
                echo "             DUCKDB_CLI, ROW_COUNT (default: 1000000), ITERATIONS (default: 3)"
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
