#!/usr/bin/env bash
# Post-bench content verification (RUN ONLY AFTER ALL TIMED BLOCKS).
# For each requested write variant: candidate COPYs the parquet fixture into
# dbo.WBV, reads it back through the extension, and compares a per-column
# signature (count + sum(hash(col)) for all 44 columns) against the parquet.
# Usage: verify.sh plain|vc-utf8|cci
set -u
SCRATCH="$(cd "$(dirname "$0")" && pwd)"
FIXTURE="${F38_FIXTURE:-$SCRATCH/f38_fixture.parquet}"
REPO="${MSSQL_BENCH_REPO:-$(git -C "$(dirname "$0")" rev-parse --show-toplevel)}"
BIN="${MSSQL_BENCH_CANDIDATE:-$REPO/build/release/duckdb}"
DSN="Server=${MSSQL_TEST_HOST},${MSSQL_TEST_PORT};Database=TestDB;User Id=${MSSQL_TEST_USER};Password=${MSSQL_TEST_PASS}"
VARIANT="$1"

COLS=(i8 i16 i32 i64 cv_i64_i32 cv_i32_i16 cv_i16_i8 cv_i8_i64 f4 f8 cv_f8_f4 bl \
      d4 d9 d18 d28 d38 cv_dec mny smny dt ts0 ts3 ts7 tm3 dto dtl sdt cv_date_ts gu \
      s_ann_nv s_ann_vc s_plain s_cyr s_cjk s_max s_vcmax s_char b_fix b_max \
      n_few n_half n_most n_all)
SIG="count(*)::VARCHAR"
for c in "${COLS[@]}"; do SIG="$SIG || '|' || sum(hash($c))::VARCHAR"; done

VC_ALL="s_ann_nv::MSSQL_VARCHAR(200) AS s_ann_nv, s_ann_vc::MSSQL_VARCHAR(200) AS s_ann_vc, s_plain::MSSQL_VARCHAR(200) AS s_plain, s_cyr::MSSQL_VARCHAR(200) AS s_cyr, s_cjk::MSSQL_VARCHAR(200) AS s_cjk, s_max::MSSQL_VARCHAR(200) AS s_max, s_vcmax::MSSQL_VARCHAR(200) AS s_vcmax, s_char::MSSQL_VARCHAR(200) AS s_char, n_half::MSSQL_VARCHAR(200) AS n_half"
case "$VARIANT" in
  plain)   SEL="FROM read_parquet('$FIXTURE')"; OPTS="";;
  vc-utf8) SEL="SELECT * REPLACE ($VC_ALL) FROM read_parquet('$FIXTURE')"; OPTS="";;
  cci)     SEL="FROM read_parquet('$FIXTURE')"; OPTS=", table_kind 'columnstore'";;
  *) echo "usage: verify.sh plain|vc-utf8|cci" >&2; exit 2;;
esac

echo "=== verify $VARIANT: write fixture -> dbo.WBV ==="
"$BIN" <<EOF || exit 1
ATTACH '$DSN' AS db (TYPE mssql);
SELECT mssql_exec('db', 'DROP TABLE IF EXISTS dbo.WBV; CHECKPOINT;');
SET mssql_copy_parallel_writers = 8;
COPY ($SEL) TO 'db.dbo.WBV' (FORMAT bcp, CREATE_TABLE true, REPLACE true$OPTS);
EOF

echo "=== signatures ==="
"$BIN" -csv -noheader >"$SCRATCH/sig_parquet.txt" 2>/dev/null <<EOF
SELECT $SIG FROM read_parquet('$FIXTURE');
EOF
"$BIN" -csv -noheader >"$SCRATCH/sig_server_$VARIANT.txt" 2>/dev/null <<EOF
ATTACH '$DSN' AS db (TYPE mssql);
SELECT $SIG FROM db.dbo.WBV;
EOF
if diff -q "$SCRATCH/sig_parquet.txt" "$SCRATCH/sig_server_$VARIANT.txt" >/dev/null; then
  echo "VERIFY $VARIANT: OK — all 44 column signatures match ($(cut -d'|' -f1 "$SCRATCH/sig_parquet.txt") rows)"
else
  echo "VERIFY $VARIANT: MISMATCH — differing columns:"
  IFS='|' read -ra P <"$SCRATCH/sig_parquet.txt"
  IFS='|' read -ra S <"$SCRATCH/sig_server_$VARIANT.txt"
  [ "${P[0]}" != "${S[0]}" ] && echo "  rowcount: parquet=${P[0]} server=${S[0]}"
  for k in "${!COLS[@]}"; do
    if [ "${P[$((k+1))]:-?}" != "${S[$((k+1))]:-?}" ]; then
      echo "  ${COLS[$k]}: parquet=${P[$((k+1))]:-?} server=${S[$((k+1))]:-?}"
    fi
  done
  exit 1
fi
