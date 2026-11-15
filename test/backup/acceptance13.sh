#!/usr/bin/env bash
# Spec 085 acceptance 13 (task 16b): a backup much larger than memory, every table scanned under
# DuckDB's limits, and every table equal to the server's.
#
#   OUT=/tmp/huge test/backup/acceptance13.sh                 # about 20 GB of data (HUGE_SCALE=1)
#   OUT=/tmp/huge HUGE_SCALE=0.05 test/backup/acceptance13.sh # a quick check of the pipeline
#
# On a throwaway SQL Server (VERSION, default 2022, images.txt's digest; a password generated here,
# reaching docker and DuckDB by environment variable only), gen/huge.sql makes the database and backs it
# up plain and MS_XPRESS; the plain backup is restored as the oracle, as the lane restores. Three runs:
# each backup with memory_limit '1GB' and every thread, and the plain one with max_memory '512MB',
# max_threads 2 (scan_memory.py: the peak DuckDB accounts stays under the limit); then compare_rows.py
# compares every table of both backups with the restored database, and the three runs' counts and
# hashes are compared with each other (one database, so every table hashes alike in both backups). Needs
# about 5x HUGE_SCALE x 20 GB in Docker's disk and 2x in OUT. KEEP=1 leaves the container running (for
# docker exec; its sa password is not kept anywhere).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
: "${OUT:?set OUT to the directory the backups go to}"
VERSION="${VERSION:-2022}"
HUGE_SCALE="${HUGE_SCALE:-1}"
DUCKDB="${DUCKDB:-build/release/duckdb}"
CONTAINER="${CONTAINER:-mssql-bak-huge}"
PORT="${PORT:-14341}"
IMAGE="$(awk -v v="${VERSION}" '$1 == v { print $2 }' "${HERE}/gen/images.txt")"
[ -n "${IMAGE}" ] || { echo "ERROR: no image for VERSION=${VERSION} in images.txt" >&2; exit 1; }
DIR=/var/opt/mssql/backup/huge
mkdir -p "${OUT}"
OUT="$(cd "${OUT}" && pwd)"

if docker ps -a --format '{{.Names}}' | grep -qx "${CONTAINER}"; then
	echo "ERROR: a container named ${CONTAINER} exists; remove it or set CONTAINER." >&2
	exit 1
fi
SA_PASSWORD="Bk$(openssl rand -hex 16)!a1"
export MSSQL_SA_PASSWORD="${SA_PASSWORD}" SQLCMDPASSWORD="${SA_PASSWORD}"
export MSSQL_ROWS_PASSWORD="${SA_PASSWORD}" MSSQL_ROWS_PORT="${PORT}" MSSQL_ROWS_DATABASE=bak_huge_cmp

echo ">> starting ${IMAGE} as ${CONTAINER} on port ${PORT}"
id="$(docker create --name "${CONTAINER}" -e ACCEPT_EULA=Y -e MSSQL_SA_PASSWORD -p "127.0.0.1:${PORT}:1433" "${IMAGE}")"
cleanup() { if [ "${KEEP:-0}" != 1 ]; then docker rm -f -v "${id}" >/dev/null 2>&1 || true; fi; }
trap cleanup EXIT
docker start "${id}" >/dev/null
SQLCMD=()
for _ in $(seq 1 90); do
	if [ "${#SQLCMD[@]}" -eq 0 ] && docker exec "${CONTAINER}" test -x /opt/mssql-tools18/bin/sqlcmd 2>/dev/null; then
		SQLCMD=(/opt/mssql-tools18/bin/sqlcmd -C)
	elif [ "${#SQLCMD[@]}" -eq 0 ] && docker exec "${CONTAINER}" test -x /opt/mssql-tools/bin/sqlcmd 2>/dev/null; then
		SQLCMD=(/opt/mssql-tools/bin/sqlcmd)  # 2017 / 2019 images
	fi
	if [ "${#SQLCMD[@]}" -gt 0 ] &&
		docker exec -e SQLCMDPASSWORD "${CONTAINER}" "${SQLCMD[@]}" -S localhost -U sa -b -Q "SELECT 1" >/dev/null 2>&1; then
		break
	fi
	sleep 2
done
[ "${#SQLCMD[@]}" -gt 0 ] || { echo "ERROR: no sqlcmd in ${IMAGE}" >&2; exit 1; }
sql() { docker exec -e SQLCMDPASSWORD "${CONTAINER}" "${SQLCMD[@]}" -S localhost -U sa -b "$@"; }
sql -Q "SELECT 1" >/dev/null
docker exec -u 0 "${CONTAINER}" sh -c "mkdir -p '${DIR}' && chown mssql '${DIR}'"
echo ">> free in the container: $(docker exec "${CONTAINER}" df -h /var/opt/mssql | awk 'NR == 2 { print $4 }')"
docker cp "${HERE}/gen/huge.sql" "${CONTAINER}:/tmp/huge.sql"

started=$(date +%s)
echo ">> generating (HUGE_SCALE ${HUGE_SCALE})"
sql -v DIR="${DIR}" HUGE_SCALE="${HUGE_SCALE}" -i /tmp/huge.sql
echo ">> generated and backed up in $(($(date +%s) - started)) s"
for f in huge_full.bak huge_xpress.bak; do
	docker cp "${CONTAINER}:${DIR}/${f}" "${OUT}/${f}"
done
ls -l "${OUT}"/huge_*.bak
echo ">> restoring the plain backup as the oracle"
sql -Q "RESTORE DATABASE bak_huge_cmp FROM DISK = '${DIR}/huge_full.bak' WITH REPLACE, RECOVERY,
	MOVE 'bak_huge' TO '/var/opt/mssql/data/bak_huge_cmp.mdf', MOVE 'bak_huge_log' TO '/var/opt/mssql/data/bak_huge_cmp.ldf'" >/dev/null

failed=0
echo ">> run 1: plain, memory_limit 1GB, every thread"
python3 -I "${HERE}/scan_memory.py" --duckdb "${DUCKDB}" --memory-limit 1GB --hashes "${OUT}/run1.json" \
	"${OUT}/huge_full.bak" || failed=$((failed + 1))
echo ">> run 2: MS_XPRESS, memory_limit 1GB, every thread"
python3 -I "${HERE}/scan_memory.py" --duckdb "${DUCKDB}" --memory-limit 1GB --hashes "${OUT}/run2.json" \
	"${OUT}/huge_xpress.bak" || failed=$((failed + 1))
echo ">> run 3: plain, memory_limit 1GB, max_memory 512MB, max_threads 2"
python3 -I "${HERE}/scan_memory.py" --duckdb "${DUCKDB}" --memory-limit 1GB --hashes "${OUT}/run3.json" \
	--scan-args "max_memory := '512MB', max_threads := 2" "${OUT}/huge_full.bak" || failed=$((failed + 1))
for run in run2 run3; do
	if cmp -s "${OUT}/run1.json" "${OUT}/${run}.json"; then
		echo ">> ${run}: every table's count and hash as run 1's"
	else
		echo "FAIL ${run}: counts or hashes differ from run 1's (${OUT}/run1.json, ${OUT}/${run}.json)"
		failed=$((failed + 1))
	fi
done
for f in huge_full.bak huge_xpress.bak; do
	echo ">> rows of ${f} against the restored database"
	python3 -I "${HERE}/compare_rows.py" --duckdb "${DUCKDB}" --min-tables 4 "${OUT}/${f}" || failed=$((failed + 1))
done
echo ">> acceptance 13: ${failed} failed step(s), $(($(date +%s) - started)) s"
[ "${failed}" = 0 ]
