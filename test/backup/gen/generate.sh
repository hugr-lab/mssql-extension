#!/usr/bin/env bash
# Spec 085: generate backups of the synthetic database (samples.sql) on a CLEAN, throwaway SQL Server
# instance, with the golden RESTORE HEADERONLY / LABELONLY / FILELISTONLY of each beside it (spec 085
# T4: only synthetic data from an instance of our own).
#
#   OUT=/tmp/bak VERSION=2019 test/backup/gen/generate.sh     # an image of images.txt, by digest
#   OUT=/tmp/bak IMAGE=mcr.microsoft.com/mssql/server:2022-latest test/backup/gen/generate.sh
#   OUT=/tmp/bak FIXTURES=samples test/backup/gen/generate.sh  # samples.sql only (the committed set)
#
# FIXTURES (default "samples catalog partition files load large rows dbcs"): samples.sql, the database of the
# committed samples; catalog.sql, the catalog fixture (spec 085 PR 2b); partition.sql, E7's partitioning
# database and more boundary types (PR 2c); files.sql, two data files and a second
# filegroup, then (files_remove.sql) a file emptied and removed (multi-page IAM chains, E19); load.sql, backups taken
# while load_workload.sql commits in the background (a live database: pass 2, a fuzzy tail, the file
# outgrowing its recorded Size); large.sql, data past one GAM interval (LARGE_ROWS rows of a page each,
# 530000 by default); rows.sql, the record forms of user tables (E20: version tags, forwarding, the
# uniquifier, ghosts, row-overflow, added columns of a partitioned table); dbcs.sql, varchar under the
# double-byte code pages (PR 2d). A fixture's backups come with the server's own catalog of them where it holds
# still (<set>.tables.txt / .columns.txt / .views.txt: catalog_golden.sql, run by golden() below).
#
# Writes into OUT: the backups, <set>.headeronly.txt / .labelonly.txt / .filelistonly.txt, sets.txt
# (one line per backup set: name|file[,file] -- what test/backup/compare_info.py reads) and
# server.txt (the server's version). What a version lacks is not generated: MS_XPRESS is asked for
# by name from 2022 (before, plain WITH COMPRESSION, the same algorithm), ZSTD from 2025.
#
# It starts a container of its own (CONTAINER, default mssql-bak-gen; refused if one of that name
# exists), with an SA password generated here and never printed or written to a file, and removes
# it however the script ends. Nothing else is touched.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
: "${OUT:?set OUT to the directory the backups go to}"
VERSION="${VERSION:-2025}"
FIXTURES="${FIXTURES:-samples catalog partition files load large rows dbcs}"
LARGE_ROWS="${LARGE_ROWS:-530000}"
if [ -z "${IMAGE:-}" ]; then
	IMAGE="$(awk -v v="${VERSION}" '$1 == v { print $2 }' "${HERE}/images.txt")"
	[ -n "${IMAGE}" ] || { echo "ERROR: no image for VERSION=${VERSION} in images.txt" >&2; exit 1; }
fi
CONTAINER="${CONTAINER:-mssql-bak-gen}"
DIR=/var/opt/mssql/backup/gen
mkdir -p "${OUT}"
OUT="$(cd "${OUT}" && pwd)"

if docker ps -a --format '{{.Names}}' | grep -qx "${CONTAINER}"; then
	echo "ERROR: a container named ${CONTAINER} exists; remove it or set CONTAINER." >&2
	exit 1
fi

# A password of our own, held in this shell's environment only: docker is given the variable's NAME
# (`-e NAME`, which docker reads from its own environment), so the value is on no command line.
SA_PASSWORD="Bk$(openssl rand -hex 16)!a1"
export MSSQL_SA_PASSWORD="${SA_PASSWORD}" SQLCMDPASSWORD="${SA_PASSWORD}"

echo ">> starting ${IMAGE} as ${CONTAINER}"
# Created, then started: the trap is set on the container we created (by its id, so a container of
# that name that is not ours is never removed), before a start that may fail.
id="$(docker create --name "${CONTAINER}" -e ACCEPT_EULA=Y -e MSSQL_SA_PASSWORD "${IMAGE}")"
cleanup() { docker rm -f "${id}" >/dev/null 2>&1 || true; }
trap cleanup EXIT
docker start "${id}" >/dev/null

# The 2017 image ships the old tools (no -C: they do not check the certificate); later ones
# mssql-tools18, which do unless told -C (the container's certificate is self-signed). Which one the
# image has is asked inside the readiness loop, until one answers, as detect_sqlcmd in
# test/compat/mssql_version_matrix.sh does: `docker start` returns before `docker exec` necessarily
# sees the container's files, and a probe asked once, too early, finds neither and picks a path the
# image does not have -- every later call then fails and the wait below reads as a server that never
# started.
SQLCMD=()
find_sqlcmd() {
	local p
	for p in /opt/mssql-tools18/bin/sqlcmd /opt/mssql-tools/bin/sqlcmd; do
		if docker exec "${CONTAINER}" test -x "${p}" 2>/dev/null; then
			SQLCMD=("${p}")
			if [[ "${p}" == *tools18* ]]; then SQLCMD+=(-C); fi
			return 0
		fi
	done
	return 1
}
sql() {	# $@ = sqlcmd arguments
	docker exec -e SQLCMDPASSWORD "${CONTAINER}" "${SQLCMD[@]}" -S localhost -U sa -b "$@"
}

echo ">> waiting for the server"
ready=0
for _ in $(seq 1 90); do
	if [ "${#SQLCMD[@]}" -gt 0 ] || find_sqlcmd; then
		if sql -Q "SELECT 1" >/dev/null 2>&1; then
			ready=1
			break
		fi
	fi
	if [ "$(docker inspect -f '{{.State.Running}}' "${CONTAINER}")" != true ]; then
		break
	fi
	sleep 2
done
if [ "${#SQLCMD[@]}" -eq 0 ]; then
	echo "ERROR: no sqlcmd in ${IMAGE}: neither /opt/mssql-tools18/bin/sqlcmd nor /opt/mssql-tools/bin/sqlcmd (or the container stopped first); its log:" >&2
	docker logs "${CONTAINER}" 2>&1 | tail -20 >&2
	exit 1
fi
if [ "${ready}" != 1 ]; then
	echo "ERROR: the server in ${IMAGE} did not start (asked through ${SQLCMD[0]}); its log:" >&2
	docker logs "${CONTAINER}" 2>&1 | tail -20 >&2
	exit 1
fi
# A plain redirect, its first line read afterwards: `| head -1` under pipefail can kill sqlcmd with
# SIGPIPE, and an empty or odd version would make the `-ge 17` tests below silently false (a failed
# test inside an `if` does not trip set -e), dropping the ZSTD sets with nothing said.
sql -h -1 -W -Q "SET NOCOUNT ON; SELECT CAST(SERVERPROPERTY('ProductVersion') AS varchar(32))" >"${OUT}/server.txt"
version=""
IFS= read -r version <"${OUT}/server.txt" || true
version="${version//[[:space:]]/}"
major="${version%%.*}"
if ! [[ ${major} =~ ^[0-9]+$ ]]; then
	echo "ERROR: the server's ProductVersion reads '${version}', not <major>.<minor>.<build>: no major version to choose the sets by" >&2
	exit 1
fi
echo "${version}" >"${OUT}/server.txt"
echo ">> server ${version}"

docker exec -u 0 "${CONTAINER}" mkdir -p "${DIR}"
# The server runs as mssql from the 2019 image on; the 2017 image runs it as root and has no such
# user, so there the directory is the server's already.
docker exec -u 0 "${CONTAINER}" sh -c "if id mssql >/dev/null 2>&1; then chown mssql '${DIR}'; fi"
docker exec -u 0 "${CONTAINER}" mkdir -p /tmp/gen
for f in "${HERE}"/*.sql; do
	docker cp "${f}" "${CONTAINER}:/tmp/gen/$(basename "${f}")"
done
run_sql() {	# $1 = a script of /tmp/gen
	sql -v DIR="${DIR}" GEN=/tmp/gen LARGE_ROWS="${LARGE_ROWS}" -s '|' -W -i "/tmp/gen/$1"
}
# The server's own catalog of database $1 as set $2's backup holds it (run right after that backup,
# nothing changed since): $2.tables.txt / .columns.txt / .views.txt / .modules.txt, through catalog_golden.sql's
# :OUT. The path goes in whole, as ONE -v variable: in an :OUT path sqlcmd substitutes no variable
# :setvar defines, and of -v variables only the first ("$(DIR)/$(SET).x" kept "$(SET)" literally;
# measured, mssql-tools18 on 2022).
golden() {	# $1 = database, $2 = set
	sql -d "$1" -v OUTPREFIX="${DIR}/$2" -s '|' -W -i /tmp/gen/catalog_golden.sql
	local kind
	for kind in tables columns views modules partitioning boundaries partitions; do extras+=("$2.${kind}.txt"); done
}
scalar() {	# $1 = a query answering one value
	sql -h -1 -W -Q "SET NOCOUNT ON; $1" | head -1 | tr -d ' \r'
}
sets=()
extras=()
for fixture in ${FIXTURES}; do
	if [ "${fixture}" = large ]; then
		# Its 4.4 GB data file beside the others' databases and backups is close to a runner's disk:
		# the databases whose backups are made go first (the backups stay).
		sql -Q "IF DB_ID('bak_load') IS NOT NULL DROP DATABASE bak_load; IF DB_ID('bak_files') IS NOT NULL DROP DATABASE bak_files" >/dev/null
		df -h / || true
	fi
	echo ">> running ${fixture}.sql"
	run_sql "${fixture}.sql"
	case "${fixture}" in
	samples)
		# The sets samples.sql made on this version, in its order. test/backup/compare_info.py's
		# samples_sets() states the same list as what to expect of a version (--expect-major): change both.
		sets+=("full|full.bak" "full_xpress|full_xpress.bak")
		if [ "${major}" -ge 17 ]; then sets+=("full_zstd|full_zstd.bak"); fi
		sets+=("stripe|stripe_1.bak,stripe_2.bak")
		if [ "${major}" -ge 17 ]; then sets+=("stripe_zstd|stripe_zstd_1.bak,stripe_zstd_2.bak"); fi
		sets+=("diff|diff.bak" "log|log.trn")
		;;
	catalog)
		golden bak_catalog catalog_full
		sets+=("catalog_full|catalog_full.bak")
		;;
	partition)
		golden bak_partition partition_full
		sets+=("partition_full|partition_full.bak")
		;;
	files)
		# Two states of one database: the golden of each is taken right after its backup.
		golden bak_files files_full
		run_sql files_remove.sql
		golden bak_files files_removed
		sets+=("files_full|files_full.bak" "files_removed|files_removed.bak")
		;;
	load)
		# The workload in the background, the backups once it is committing, then it is stopped and
		# waited for (its session gone). Its output goes to a log, shown when it did not run: in /tmp,
		# which the server's user can write (from the 2019 image on it is mssql; /tmp/gen is root's).
		docker exec -d -e SQLCMDPASSWORD "${CONTAINER}" sh -c '"$@" >/tmp/workload.log 2>&1' sh \
			"${SQLCMD[@]}" -S localhost -U sa -b -i /tmp/gen/load_workload.sql
		# A failed query answers empty, so the checks below name it (under set -e a bare assignment of a
		# failed substitution would end the script with no word).
		ticks() { scalar "SELECT ticks FROM bak_load.dbo.control" || true; }
		workload_failed() {	# $1 = what went wrong
			echo "ERROR: the load fixture's workload $1; its log:" >&2
			docker exec "${CONTAINER}" cat /tmp/workload.log >&2 || true
			exit 1
		}
		for _ in $(seq 1 60); do
			[ "$(ticks)" -ge 20 ] 2>/dev/null && break
			sleep 1
		done
		before="$(ticks)"
		[ "${before}" -ge 20 ] 2>/dev/null || workload_failed "did not commit 20 transactions in 60 s (at '${before}')"
		echo ">> workload at ${before} transactions; backing up under it"
		run_sql load_backup.sql
		after="$(ticks)"
		echo ">> backups done at ${after} transactions"
		# The backups overlapped the workload only if it went on committing through them.
		[ "${after}" -gt "${before}" ] 2>/dev/null || workload_failed "stopped before the backups ended (${before} -> ${after})"
		sql -Q "UPDATE bak_load.dbo.control SET stop_requested = 1" >/dev/null
		left=""
		for _ in $(seq 1 60); do
			left="$(scalar "SELECT COUNT(*) FROM sys.dm_exec_sessions WHERE database_id = DB_ID('bak_load') AND session_id <> @@SPID" || true)"
			[ "${left}" = 0 ] && break
			sleep 1
		done
		[ "${left}" = 0 ] || workload_failed "did not end 60 s after it was asked to stop (${left} session(s) left)"
		sets+=("load_base|load_base.bak" "load_full|load_full.bak" "load_xpress|load_xpress.bak" "load_diff|load_diff.bak" "load_log|load_log.trn")
		;;
	rows)
		golden bak_rows rows_full
		sets+=("rows_full|rows_full.bak")
		;;
	dbcs)
		golden bak_dbcs dbcs_full
		sets+=("dbcs_full|dbcs_full.bak")
		;;
	large)
		golden bak_large large_xpress
		sets+=("large_xpress|large_xpress.bak")
		;;
	*)
		echo "ERROR: unknown fixture ${fixture}" >&2
		exit 1
		;;
	esac
done

: >"${OUT}/sets.txt"
for entry in "${sets[@]}"; do
	name="${entry%%|*}"
	files="${entry#*|}"
	from=""
	IFS=, read -r -a parts <<<"${files}"
	for f in "${parts[@]}"; do
		from="${from:+${from}, }DISK = '${DIR}/${f}'"
		docker cp "${CONTAINER}:${DIR}/${f}" "${OUT}/${f}"
	done
	for kind in HEADERONLY LABELONLY FILELISTONLY; do
		sql -Q "SET NOCOUNT ON; RESTORE ${kind} FROM ${from}" -s '|' -W \
			>"${OUT}/${name}.$(echo "${kind}" | tr '[:upper:]' '[:lower:]').txt"
	done
	echo "${entry}" >>"${OUT}/sets.txt"
done
for f in "${extras[@]+"${extras[@]}"}"; do
	docker cp "${CONTAINER}:${DIR}/${f}" "${OUT}/${f}"
done

echo ">> done: $(wc -l <"${OUT}/sets.txt" | tr -d ' ') sets in ${OUT}"
