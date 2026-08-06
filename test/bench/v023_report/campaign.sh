#!/usr/bin/env bash
# v0.2.3 report campaign blocks. Usage: campaign.sh <B|C|B2|C2>
# B  = round-1 candidate write sweep + type/kind variants + CTAS pair
# C  = round-1 reads (old,new per variant)
# B2 = round-2 writes (binary order flipped where paired)
# C2 = round-2 reads (new,old per variant)
# Env: MSSQL_TEST_* set by caller; RESULTS/SIZES exported by caller.
set -u
R="$(cd "$(dirname "$0")" && pwd)"
SCRATCH="$(dirname "$R")"
OLD="${MSSQL_BENCH_BASELINE:-$SCRATCH/stock/duckdb}"
NEW=./build/release/duckdb
cd "${MSSQL_BENCH_REPO:-$(git -C "$(dirname "$0")" rev-parse --show-toplevel)}"

w() { # w <bin> <variant> <tag> <size-label>
  "$R/wb_util.sh" drop
  "$R/run_variant.sh" "$1" "$2" full "$3" && "$R/wb_util.sh" sizes "$3" "$4"
}
r() { "$R/run_variant.sh" "$1" "$2" full "$3"; }

case "$1" in
  B)
    for n in 2 4 8; do w $NEW w-plain-$n r1 new-w-plain-$n; done
    w $NEW w-maxlen  r1 new-w-maxlen
    w $NEW w-nv      r1 new-w-nv
    w $NEW w-vc-cp   r1 new-w-vc-cp
    w $NEW w-vc-utf8 r1 new-w-vc-utf8
    w $NEW w-cci     r1 new-w-cci
    w $NEW w-cci-vc  r1 new-w-cci-vc
    # old ctas from parquet would hit F0 (segfault) — the old/new CTAS pair is
    # measured from the generated source in block G instead.
    w $NEW  ctas     r1 new-ctas
    "$R/wb_util.sh" drop
    ;;
  C)
    r "$OLD" r-grp r1;      r $NEW r-grp r1
    r "$OLD" r-cols r1;     r $NEW r-cols r1
    r "$OLD" r-parquet r1;  r $NEW r-parquet r1
    ;;
  B2)
    w $NEW  w-plain-1 r2 new-w-plain-1
    for n in 2 4 8; do w $NEW w-plain-$n r2 new-w-plain-$n; done
    w $NEW w-maxlen  r2 new-w-maxlen
    w $NEW w-nv      r2 new-w-nv
    w $NEW w-vc-cp   r2 new-w-vc-cp
    w $NEW w-vc-utf8 r2 new-w-vc-utf8
    w $NEW w-cci     r2 new-w-cci
    w $NEW w-cci-vc  r2 new-w-cci-vc
    w $NEW  ctas     r2 new-ctas
    # old round-2 runs use the generated source (F0) — launched separately via gen_run.sh
    "$R/wb_util.sh" drop
    ;;
  C2)
    r $NEW r-grp r2;      r "$OLD" r-grp r2
    r $NEW r-cols r2;     r "$OLD" r-cols r2
    r $NEW r-parquet r2;  r "$OLD" r-parquet r2
    ;;
  *) echo "usage: campaign.sh B|C|B2|C2" >&2; exit 2;;
esac
echo "BLOCK_$1_DONE"
