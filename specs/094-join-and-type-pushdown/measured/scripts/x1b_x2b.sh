#!/usr/bin/env bash
# Spec 094, review of #622: X1b (VECTOR_DISTANCE against DuckDB on random float32 vectors, 384 and 1536
# dimensions), X2b (a non-rectangular constant for `&&`, mixed SRIDs) and X3b (geography: points the
# geodesic and the planar box tell apart, and a planar form on the server), on a throwaway SQL Server 2025
# reached from DuckDB through TYPE mssql. Usage: x1b_x2b.sh <duckdb binary> <extension> <scratch dir>.
# The password is generated here and kept in a 0600 file under the scratch dir, removed on exit.
set -euo pipefail
DUCKDB="$1"
EXT="$2"
SCRATCH="$3"
HERE="$(cd "$(dirname "$0")" && pwd)"
IMAGE="$(awk '$1 == 2025 { print $2 }' "${HERE}/../../../../test/backup/gen/images.txt")"
CONTAINER=mssql-x094b-probe
PORT=14394
umask 077
PW="Bk$(openssl rand -hex 16)!a1"
SECRET_SQL="${SCRATCH}/x094b_secret.sql"
id=""
cleanup() {
  [ -n "${id}" ] && docker rm -f "${id}" >/dev/null 2>&1 || true
  rm -f "${SECRET_SQL}"
}
trap cleanup EXIT
printf "LOAD '%s';\nCREATE SECRET x094 (TYPE mssql, host '127.0.0.1', port %s, database 'x094', user 'sa', password '%s', trust_server_certificate true);\n" \
  "${EXT}" "${PORT}" "${PW}" >"${SECRET_SQL}"
export MSSQL_SA_PASSWORD="${PW}" SQLCMDPASSWORD="${PW}"
id="$(docker create --name "${CONTAINER}" -p "127.0.0.1:${PORT}:1433" -e ACCEPT_EULA=Y -e MSSQL_SA_PASSWORD "${IMAGE}")"
docker start "${id}" >/dev/null
sql() { docker exec -e SQLCMDPASSWORD "${CONTAINER}" /opt/mssql-tools18/bin/sqlcmd -C -S localhost -U sa "$@"; }
for _ in $(seq 1 90); do sql -Q "SELECT 1" >/dev/null 2>&1 && break; sleep 2; done
docker exec -u 0 "${CONTAINER}" mkdir -p /tmp/x
docker cp "${HERE}/x2b_x3b.sql" "${CONTAINER}:/tmp/x/x2b_x3b.sql"
docker exec -u 0 "${CONTAINER}" chmod -R a+r /tmp/x
sql -b -Q "CREATE DATABASE x094"
duck() { "${DUCKDB}" -unsigned -init "${SECRET_SQL}" -c "$1" 2>&1 | grep -v "^-- Loading"; }

echo "##### X1b: random float32 vectors"
for n in 384 1536; do
  duck "
ATTACH '' AS x (TYPE mssql, SECRET x094);
CREATE TABLE vr AS SELECT i AS id,
  list_transform(range(${n}), lambda k: (random() * 2 - 1)::FLOAT)::FLOAT[${n}] AS v FROM range(2000) t(i);
CREATE TABLE q AS SELECT list_transform(range(${n}), lambda k: (random() * 2 - 1)::FLOAT)::FLOAT[${n}] AS v;
CREATE TABLE x.dbo.vr${n} AS SELECT * FROM vr;
CREATE TABLE x.dbo.q${n} AS SELECT * FROM q;
CREATE TABLE srv AS SELECT * FROM mssql_scan('x', 'SELECT r.id,
  VECTOR_DISTANCE(''euclidean'', r.v, q.v) AS e, VECTOR_DISTANCE(''dot'', r.v, q.v) AS d,
  VECTOR_DISTANCE(''cosine'', r.v, q.v) AS c FROM dbo.vr${n} r CROSS JOIN dbo.q${n} q');
CREATE TABLE loc AS SELECT r.id, array_distance(r.v, q.v) AS e, array_negative_inner_product(r.v, q.v) AS d,
  array_cosine_distance(r.v, q.v) AS c FROM vr r, q;
SELECT ${n} AS dims, count(*) AS rows,
  count(*) FILTER (WHERE s.e::FLOAT = l.e) AS euclidean_equal,
  count(*) FILTER (WHERE s.d::FLOAT = l.d) AS dot_equal,
  count(*) FILTER (WHERE s.c::FLOAT = l.c) AS cosine_equal,
  count(*) FILTER (WHERE s.e = s.e::FLOAT::DOUBLE AND s.d = s.d::FLOAT::DOUBLE AND s.c = s.c::FLOAT::DOUBLE) AS server_is_float32,
  max(abs(s.e - l.e) / l.e) AS euclidean_max_rel, max(abs(s.d - l.d)) AS dot_max_abs,
  max(abs(s.c - l.c)) AS cosine_max_abs
FROM srv s JOIN loc l USING (id);
CREATE TABLE ranks AS
  SELECT 'srv' AS side, m.metric, id, row_number() OVER (PARTITION BY m.metric ORDER BY CASE m.metric WHEN 'e' THEN e WHEN 'd' THEN d ELSE c END, id) AS r
    FROM srv, (VALUES ('e'), ('d'), ('c')) m(metric)
  UNION ALL
  SELECT 'loc', m.metric, id, row_number() OVER (PARTITION BY m.metric ORDER BY CASE m.metric WHEN 'e' THEN e WHEN 'd' THEN d ELSE c END, id)
    FROM loc, (VALUES ('e'), ('d'), ('c')) m(metric);
SELECT ${n} AS dims, a.metric,
  count(*) FILTER (WHERE a.r <> b.r) AS rank_disagreements,
  count(*) FILTER (WHERE a.r <= 10 AND b.r <= 10) AS top10_shared,
  count(*) FILTER (WHERE a.r <= 100 AND b.r <= 100) AS top100_shared
FROM ranks a JOIN ranks b ON a.metric = b.metric AND a.id = b.id AND a.side = 'srv' AND b.side = 'loc'
GROUP BY a.metric ORDER BY a.metric;
"
done

echo "##### X2b / X3b on the server"
sql -d x094 -s '|' -W -w 65535 -i /tmp/x/x2b_x3b.sql 2>&1 || true

echo "##### X2b: does each SRID form use the spatial index (the plan names six_geosrid)"
BOX="geometry::STGeomFromText('POLYGON((100 100, 300 100, 300 300, 100 300, 100 100))', 0)"
for form in "g.STIntersects(${BOX}) = 1" \
  "g.STIntersects(geometry::STGeomFromWKB(${BOX}.STAsBinary(), g.STSrid)) = 1" \
  "g.STIntersects(${BOX}) = 1 OR g.STSrid <> 0" \
  "g.Filter(${BOX}) = 1 OR g.STSrid <> 0"; do
  uses=$(sql -d x094 -y 0 -Q "SET STATISTICS XML ON; SELECT COUNT(*) FROM dbo.geosrid WHERE ${form};" 2>&1 |
    grep -c 'six_geosrid' || true)
  echo "index_used=$([ "${uses}" -gt 0 ] && echo yes || echo no) | ${form}"
done

echo "##### X2b / X3b in DuckDB (TYPE mssql, no spatial extension: core && only)"
duck "
ATTACH '' AS x (TYPE mssql, SECRET x094);
SELECT 'duckdb_and_and_diag' AS form, count(*) AS n FROM x.dbo.geo2
  WHERE g && 'LINESTRING(100 100, 300 300)'::GEOMETRY;
SELECT 'duckdb_and_and_srid_mixed' AS form, count(*) AS n FROM x.dbo.geosrid
  WHERE g && 'POLYGON((100 100, 300 100, 300 300, 100 300, 100 100))'::GEOMETRY;
SELECT 'duckdb_geog_planar' AS form, list(id ORDER BY id) AS ids FROM x.dbo.geog2
  WHERE g && 'POLYGON((10 70, 20 70, 20 71, 10 71, 10 70))'::GEOMETRY('OGC:CRS84');
"
