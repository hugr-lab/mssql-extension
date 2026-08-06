#!/usr/bin/env bash
# Untimed helpers around write runs. Usage: wb_util.sh drop | sizes <tag> <variant>
# Uses the static candidate binary (version-independent server ops).
set -u
SCRATCH="$(cd "$(dirname "$0")" && pwd)"
REPO="${MSSQL_BENCH_REPO:-$(git -C "$(dirname "$0")" rev-parse --show-toplevel)}"
SIZES="${SIZES:-$SCRATCH/sizes.csv}"
DSN="Server=${MSSQL_TEST_HOST},${MSSQL_TEST_PORT};Database=TestDB;User Id=${MSSQL_TEST_USER};Password=${MSSQL_TEST_PASS}"

case "$1" in
  drop)
    "$REPO/build/release/duckdb" >/dev/null 2>&1 <<EOF
ATTACH '$DSN' AS db (TYPE mssql);
SELECT mssql_exec('db', 'DROP TABLE IF EXISTS dbo.WB, dbo.WCTAS; CHECKPOINT;');
EOF
    ;;
  sizes)
    TAG="$2"; VARIANT="$3"
    "$REPO/build/release/duckdb" -csv -noheader 2>/dev/null <<EOF | grep -v '^$' | sed "s/^/$TAG,$VARIANT,/" >>"$SIZES"
ATTACH '$DSN' AS db (TYPE mssql);
SELECT tbl, reserved_mb, data_mb, rows, cci_compressed_rg, cci_open_rg FROM mssql_scan('db', '
  SELECT t.name AS tbl,
         SUM(ps.reserved_page_count)*8/1024 AS reserved_mb,
         SUM(ps.used_page_count)*8/1024 AS data_mb,
         SUM(CASE WHEN ps.index_id IN (0,1) THEN ps.row_count ELSE 0 END) AS rows,
         (SELECT COUNT(*) FROM sys.dm_db_column_store_row_group_physical_stats rg
           WHERE rg.object_id = t.object_id AND rg.state_desc = ''COMPRESSED'') AS cci_compressed_rg,
         (SELECT COUNT(*) FROM sys.dm_db_column_store_row_group_physical_stats rg
           WHERE rg.object_id = t.object_id AND rg.state_desc = ''OPEN'') AS cci_open_rg
  FROM sys.dm_db_partition_stats ps JOIN sys.tables t ON t.object_id = ps.object_id
  WHERE t.name IN (''WB'', ''WCTAS'')
  GROUP BY t.name, t.object_id');
EOF
    tail -1 "$SIZES"
    ;;
  *) echo "usage: wb_util.sh drop | sizes <tag> <variant>" >&2; exit 2;;
esac
