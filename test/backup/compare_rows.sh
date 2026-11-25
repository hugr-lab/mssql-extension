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
QUIET="${QUIET:-full catalog_full partition_full files_full files_removed large_xpress load_base rows_full dbcs_full lob_full lob_xpress cd_full cd_xpress page_full page_xpress names_full diffs_full clr_types_full}"
# Spec 085 PR 4c: the full + differential pairs of overlays.txt (name|full set|differential set) compared
# too: the full restored WITH NORECOVERY, the differential WITH RECOVERY, against
# mssql_backup_scan(full, diff := differential).
OVERLAYS="${OVERLAYS-diffs_over diffs_over_xpress page_over}"
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

# A set's files, as sets.txt lists them (comma-separated).
set_files() {
	awk -F'|' -v n="$1" '$1 == n { print $2 }' "${DIR}/sets.txt"
}
# Copies a set's files into the container: its RESTORE's FROM clause into `from`, its local paths into
# `local_files`.
stage_set() {
	from=""
	local_files=""
	IFS=, read -r -a parts <<<"$(set_files "$1")"
	for f in "${parts[@]}"; do
		docker cp -L "${DIR}/${f}" "${CONTAINER}:${BAK}/${f}"
		docker exec -u 0 "${CONTAINER}" sh -c "if id mssql >/dev/null 2>&1; then chown mssql '${BAK}/${f}'; fi"
		from="${from:+${from}, }DISK = '${BAK}/${f}'"
		local_files="${local_files:+${local_files},}${DIR}/${f}"
	done
}
# Every pair asked for must be there: a diffs fixture that failed to generate is a failure, not a skip.
for want in ${OVERLAYS}; do
	if [ ! -f "${DIR}/overlays.txt" ] || ! awk -F'|' -v n="${want}" '$1 == n { found = 1 } END { exit !found }' "${DIR}/overlays.txt"; then
		echo "ERROR: ${want} is not in ${DIR}/overlays.txt (OVERLAYS='' compares no pair)" >&2
		failed=$((failed + 1))
	fi
done
if [ -f "${DIR}/overlays.txt" ]; then
	while IFS='|' read -r name full diff; do
		case " ${OVERLAYS} " in *" ${name} "*) ;; *) continue ;; esac
		echo ">> ${name} (${full} + ${diff})"
		if [ -z "$(set_files "${full}")" ] || [ -z "$(set_files "${diff}")" ]; then
			echo "ERROR: ${name}: ${full} or ${diff} is not in sets.txt" >&2
			failed=$((failed + 1))
			continue
		fi
		stage_set "${full}"
		full_from="${from}"
		full_files="${local_files}"
		stage_set "${diff}"
		diff_from="${from}"
		diff_files="${local_files}"
		moves="$(awk -F'|' 'NR > 2 && NF > 3 { printf ", MOVE N'\''%s'\'' TO N'\''/var/opt/mssql/data/cmp_%d.f'\''", $1, NR }' \
			"${DIR}/${full}.filelistonly.txt")"
		if ! restored="$(sql -Q "RESTORE DATABASE cmp FROM ${full_from} WITH REPLACE, NORECOVERY${moves}; RESTORE DATABASE cmp FROM ${diff_from} WITH RECOVERY" 2>&1)"; then
			echo "ERROR: RESTORE of ${name} failed:" >&2
			echo "${restored}" | tail -5 >&2
			failed=$((failed + 1))
			docker exec -u 0 "${CONTAINER}" sh -c "rm -f '${BAK}'/*"
			continue
		fi
		if python3 -I "${HERE}/compare_rows.py" --duckdb "${DUCKDB}" --diff "${diff_files}" "${full_files}"; then
			compared=$((compared + 1))
		else
			failed=$((failed + 1))
		fi
		sql -Q "ALTER DATABASE cmp SET SINGLE_USER WITH ROLLBACK IMMEDIATE; DROP DATABASE cmp" >/dev/null
		docker exec -u 0 "${CONTAINER}" sh -c "rm -f '${BAK}'/*"
	done <"${DIR}/overlays.txt"
fi

# Spec 085 PR 5 (D11, task 27): the backups taken under load (FUZZY), and the full + differential pairs of
# overlays.txt among them (FUZZY_OVERLAYS), restored -- RESTORE recovers them: the tail redone, its open
# transactions undone -- and compared with --fuzzy: each table's fuzzy_status against whether it differs.
# Every one asked for must be there.
# tail_window (changes at known points of the copy, review of #591) among them. Per set (set=schema.table,...):
# FUZZY_FLAGGED_EQUAL the tables that may read stale while equal (an index rebuilt in the tail),
# FUZZY_MUST_DIFFER those that must differ (changed after their copy: a run that missed its window fails).
FUZZY="${FUZZY-fuzzy_full fuzzy_xpress load_full load_xpress tail_window_full}"
FUZZY_OVERLAYS="${FUZZY_OVERLAYS-fuzzy_over}"
FUZZY_FLAGGED_EQUAL="${FUZZY_FLAGGED_EQUAL-tail_window_full=dbo.rebuilt}"
FUZZY_MUST_DIFFER="${FUZZY_MUST_DIFFER-tail_window_full=dbo.stale_update,dbo.rollback_insert,dbo.rollback_delete,dbo.rollback_loaded,dbo.rollback_truncated,dbo.savepoint,dbo.parallel_open,dbo.open_truncated,dbo.open_grown,dbo.loaded,dbo.truncated,dbo.widened}"
of_set() {	# $1 = set, $2 = a list of set=tables entries: the set's tables
	printf '%s\n' $2 | awk -F= -v n="$1" '$1 == n { print $2 }'
}
fuzzy_compare() {	# $1 = what, then compare_rows.py's arguments
	local what="$1"
	shift
	if python3 -I "${HERE}/compare_rows.py" --duckdb "${DUCKDB}" --fuzzy \
		--flagged-equal "$(of_set "${what}" "${FUZZY_FLAGGED_EQUAL}")" \
		--must-differ "$(of_set "${what}" "${FUZZY_MUST_DIFFER}")" "$@"; then
		compared=$((compared + 1))
	else
		echo "ERROR: ${what}: a fuzzy_status disagrees with the RESTORE" >&2
		failed=$((failed + 1))
	fi
	sql -Q "ALTER DATABASE cmp SET SINGLE_USER WITH ROLLBACK IMMEDIATE; DROP DATABASE cmp" >/dev/null
	docker exec -u 0 "${CONTAINER}" sh -c "rm -f '${BAK}'/*"
}
for name in ${FUZZY}; do
	if [ -z "$(set_files "${name}")" ]; then
		echo "ERROR: ${name} is not in ${DIR}/sets.txt (FUZZY='' compares none)" >&2
		failed=$((failed + 1))
		continue
	fi
	echo ">> ${name} (fuzzy)"
	stage_set "${name}"
	moves="$(awk -F'|' 'NR > 2 && NF > 3 { printf ", MOVE N'\''%s'\'' TO N'\''/var/opt/mssql/data/cmp_%d.f'\''", $1, NR }' \
		"${DIR}/${name}.filelistonly.txt")"
	if ! restored="$(sql -Q "RESTORE DATABASE cmp FROM ${from} WITH REPLACE, RECOVERY${moves}" 2>&1)"; then
		echo "ERROR: RESTORE of ${name} failed:" >&2
		echo "${restored}" | tail -5 >&2
		failed=$((failed + 1))
		docker exec -u 0 "${CONTAINER}" sh -c "rm -f '${BAK}'/*"
		continue
	fi
	fuzzy_compare "${name}" "${local_files}"
done
for want in ${FUZZY_OVERLAYS}; do
	pair="$(awk -F'|' -v n="${want}" '$1 == n { print $2 "|" $3 }' "${DIR}/overlays.txt" 2>/dev/null || true)"
	full="${pair%%|*}"
	diff="${pair#*|}"
	if [ -z "${pair}" ] || [ -z "$(set_files "${full}")" ] || [ -z "$(set_files "${diff}")" ]; then
		echo "ERROR: ${want} is not a pair of ${DIR}/overlays.txt with both sets in sets.txt" >&2
		failed=$((failed + 1))
		continue
	fi
	echo ">> ${want} (${full} + ${diff}, fuzzy)"
	stage_set "${full}"
	full_from="${from}"
	full_files="${local_files}"
	stage_set "${diff}"
	moves="$(awk -F'|' 'NR > 2 && NF > 3 { printf ", MOVE N'\''%s'\'' TO N'\''/var/opt/mssql/data/cmp_%d.f'\''", $1, NR }' \
		"${DIR}/${full}.filelistonly.txt")"
	if ! restored="$(sql -Q "RESTORE DATABASE cmp FROM ${full_from} WITH REPLACE, NORECOVERY${moves}; RESTORE DATABASE cmp FROM ${from} WITH RECOVERY" 2>&1)"; then
		echo "ERROR: RESTORE of ${want} failed:" >&2
		echo "${restored}" | tail -5 >&2
		failed=$((failed + 1))
		docker exec -u 0 "${CONTAINER}" sh -c "rm -f '${BAK}'/*"
		continue
	fi
	fuzzy_compare "${want}" --diff "${local_files}" "${full_files}"
done

# Spec 085 task 27: every set's log -- a data backup's tail, a log backup's whole log -- as
# mssql_backup_transaction_records reads it, against fn_dump_dblog over the same files on this server
# (compare_rows.py --log). LOG_SETS: the sets compared, "all" (default) for every set of sets.txt.
LOG_SETS="${LOG_SETS-all}"
while IFS='|' read -r name files; do
	if [ "${LOG_SETS}" != all ]; then
		case " ${LOG_SETS} " in *" ${name} "*) ;; *) continue ;; esac
	fi
	echo ">> ${name} (log)"
	stage_set "${name}"
	server_files=""
	IFS=, read -r -a parts <<<"${files}"
	for f in "${parts[@]}"; do
		server_files="${server_files:+${server_files},}${BAK}/${f}"
	done
	# No database is restored for it: the session attaches master.
	if MSSQL_ROWS_DATABASE=master python3 -I "${HERE}/compare_rows.py" --duckdb "${DUCKDB}" --log "${server_files}" \
		"${local_files}"; then
		compared=$((compared + 1))
	else
		echo "ERROR: ${name}: its log differs from fn_dump_dblog's" >&2
		failed=$((failed + 1))
	fi
	docker exec -u 0 "${CONTAINER}" sh -c "rm -f '${BAK}'/*"
done <"${DIR}/sets.txt"

echo ">> compare_rows: ${compared} sets equal, ${failed} with differences"
[ "${failed}" = 0 ] && [ "${compared}" -gt 0 ]
