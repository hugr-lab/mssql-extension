#!/usr/bin/env bash
# Spec 085 PR 2d: every quiet full set of one version's oracle output restored on a throwaway server of
# that version, one at a time, and its tables compared with mssql_backup_scan (compare_rows.py).
#
#   VERSION=2022 DUCKDB=build/release/duckdb test/backup/compare_rows.sh /tmp/bak/2022
#
# DIR is generate.sh's OUT for that version (sets.txt, the backups, <set>.filelistonly.txt); in the lane
# it is downloaded from R2 first. The sets compared are the quiet full ones (QUIET below, those present):
# the load fixture's backups taken under its workload are fuzzy, and their tables differ from a RESTORE
# (which replays the tail) until spec 090 -- only load_base, taken before the workload, is compared. The
# server's image is images.txt's for VERSION, by digest; its password is generated here and reaches
# docker and DuckDB by environment variable, never a command line.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
DIR="$(cd "${1:?usage: compare_rows.sh DIR}" && pwd)"
: "${VERSION:?set VERSION (2017, 2019, 2022 or 2025)}"
DUCKDB="${DUCKDB:-build/release/duckdb}"
QUIET="${QUIET:-full catalog_full partition_full files_full files_removed large_xpress load_base rows_full dbcs_full lob_full lob_xpress}"
IMAGE="$(awk -v v="${VERSION}" '$1 == v { print $2 }' "${HERE}/gen/images.txt")"
[ -n "${IMAGE}" ] || { echo "ERROR: no image for VERSION=${VERSION} in images.txt" >&2; exit 1; }
CONTAINER="${CONTAINER:-mssql-bak-rows}"
PORT="${PORT:-14339}"
BAK=/var/opt/mssql/backup/rows

if docker ps -a --format '{{.Names}}' | grep -qx "${CONTAINER}"; then
	echo "ERROR: a container named ${CONTAINER} exists; remove it or set CONTAINER." >&2
	exit 1
fi
SA_PASSWORD="Bk$(openssl rand -hex 16)!a1"
export MSSQL_SA_PASSWORD="${SA_PASSWORD}" SQLCMDPASSWORD="${SA_PASSWORD}"
export MSSQL_ROWS_PASSWORD="${SA_PASSWORD}" MSSQL_ROWS_PORT="${PORT}" MSSQL_ROWS_DATABASE=cmp

echo ">> starting ${IMAGE} as ${CONTAINER} on port ${PORT}"
id="$(docker create --name "${CONTAINER}" -e ACCEPT_EULA=Y -e MSSQL_SA_PASSWORD -p "127.0.0.1:${PORT}:1433" "${IMAGE}")"
cleanup() { docker rm -f "${id}" >/dev/null 2>&1 || true; }
trap cleanup EXIT
docker start "${id}" >/dev/null
if docker exec "${CONTAINER}" test -x /opt/mssql-tools18/bin/sqlcmd; then
	SQLCMD=(/opt/mssql-tools18/bin/sqlcmd -C)
else
	SQLCMD=(/opt/mssql-tools/bin/sqlcmd)
fi
sql() { docker exec -e SQLCMDPASSWORD "${CONTAINER}" "${SQLCMD[@]}" -S localhost -U sa -b "$@"; }
ready=0
for _ in $(seq 1 90); do
	if sql -Q "SELECT 1" >/dev/null 2>&1; then
		ready=1
		break
	fi
	sleep 2
done
[ "${ready}" = 1 ] || { echo "ERROR: the server in ${IMAGE} did not start" >&2; docker logs "${CONTAINER}" 2>&1 | tail -20 >&2; exit 1; }
docker exec -u 0 "${CONTAINER}" mkdir -p "${BAK}"
docker exec -u 0 "${CONTAINER}" sh -c "if id mssql >/dev/null 2>&1; then chown mssql '${BAK}'; fi"

failed=0
compared=0
while IFS='|' read -r name files; do
	case " ${QUIET} " in *" ${name} "*) ;; *) continue ;; esac
	echo ">> ${name}"
	from=""
	local_files=""
	IFS=, read -r -a parts <<<"${files}"
	for f in "${parts[@]}"; do
		docker cp -L "${DIR}/${f}" "${CONTAINER}:${BAK}/${f}"
		docker exec -u 0 "${CONTAINER}" sh -c "if id mssql >/dev/null 2>&1; then chown mssql '${BAK}/${f}'; fi"
		from="${from:+${from}, }DISK = '${BAK}/${f}'"
		local_files="${local_files:+${local_files},}${DIR}/${f}"
	done
	# Every file of the set moved into the container's data directory: FILELISTONLY's logical names,
	# rows after its header and dashes, separated by |.
	moves="$(awk -F'|' 'NR > 2 && NF > 3 { printf ", MOVE N'\''%s'\'' TO N'\''/var/opt/mssql/data/cmp_%d.f'\''", $1, NR }' \
		"${DIR}/${name}.filelistonly.txt")"
	if ! restored="$(sql -Q "RESTORE DATABASE cmp FROM ${from} WITH REPLACE, RECOVERY${moves}" 2>&1)"; then
		echo "ERROR: RESTORE of ${name} failed:" >&2
		echo "${restored}" | tail -5 >&2
		failed=$((failed + 1))
		docker exec -u 0 "${CONTAINER}" sh -c "rm -f '${BAK}'/*"
		continue
	fi
	if python3 -I "${HERE}/compare_rows.py" --duckdb "${DUCKDB}" "${local_files}"; then
		compared=$((compared + 1))
	else
		failed=$((failed + 1))
	fi
	sql -Q "ALTER DATABASE cmp SET SINGLE_USER WITH ROLLBACK IMMEDIATE; DROP DATABASE cmp" >/dev/null
	docker exec -u 0 "${CONTAINER}" sh -c "rm -f '${BAK}'/*"
done <"${DIR}/sets.txt"

echo ">> compare_rows: ${compared} sets equal, ${failed} with differences"
[ "${failed}" = 0 ] && [ "${compared}" -gt 0 ]
