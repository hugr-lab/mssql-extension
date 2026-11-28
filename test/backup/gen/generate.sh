#!/usr/bin/env bash
# Spec 085: generate backups of the synthetic database (samples.sql) on a CLEAN, throwaway SQL Server
# instance, with the golden RESTORE HEADERONLY / LABELONLY / FILELISTONLY of each beside it (spec 085
# T4: only synthetic data from an instance of our own).
#
#   OUT=/tmp/bak VERSION=2019 test/backup/gen/generate.sh     # an image of images.txt, by digest
#   OUT=/tmp/bak IMAGE=mcr.microsoft.com/mssql/server:2022-latest test/backup/gen/generate.sh
#   OUT=/tmp/bak FIXTURES=samples test/backup/gen/generate.sh  # samples.sql only (the committed set)
#
# FIXTURES (default "samples catalog partition files load fuzzy lob large rows dbcs cd page sparse names diffs clr_types tail_window variant datetime2 json"; json on 2025 only): samples.sql, the database of the
# committed samples; catalog.sql, the catalog fixture (spec 085 PR 2b); partition.sql, E7's partitioning
# database and more boundary types (PR 2c); files.sql, two data files and a second
# filegroup, then (files_remove.sql) a file emptied and removed (multi-page IAM chains, E19); load.sql, backups taken
# while load_workload.sql commits in the background (a live database: pass 2, a fuzzy tail, the file
# outgrowing its recorded Size); large.sql, data past one GAM interval (LARGE_ROWS rows of a page each,
# 530000 by default); rows.sql, the record forms of user tables (E20: version tags, forwarding, the
# uniquifier, ghosts, row-overflow, added columns of a partitioned table); dbcs.sql, varchar under the
# double-byte code pages (PR 2d); lob.sql, values stored off row in every form (PR 3: lob_full plain,
# lob_xpress compressed); cd.sql, ROW-compressed tables (spec 086 PR 1: cd_full plain, cd_xpress compressed); page.sql, PAGE-compressed ones (spec 086 PR 2: page_full, page_xpress); sparse.sql, sparse columns (spec 086 PR 3, E24: sparse_full, then sparse_changes.sql's sparse_diff, both
# compressed, paired as sparse_over; also committed under test/data/backup/sparse, made by its generate.sh); names.sql, a
# case-sensitive database whose tables differ only in case, and names with spaces, quotes, ] and dots (spec 085 PR 4b);
# fuzzy.sql, E3's controls under load (spec 085 PR 5: fuzzy_base, then fuzzy_full, fuzzy_xpress and fuzzy_diff
# taken while three sessions commit and one transaction stays open; fuzzy_over pairs the last with the first);
# open_tail.sql, one transaction held open across a full and a differential (PR 5: open_tail_base, _full,
# _diff; not run by default; committed under test/data/backup/open_tail, made by its generate.sh);
# diffs.sql, a full and a differential over it after E2's changes (spec 085 PR 4c: diffs_full, diffs_diff and
# diffs_diff_xpress, paired in overlays.txt); schema_changes.sql, truncates, a shrink and a sliding window between a
# full and its differential (PR 4c-3: schema_full, schema_diff; not run by default); lob_changes.sql, values stored
# off row changed in every form, then a shrink (PR 4c-3: lob_changes_full, lob_changes_diff, lob_changes_shrink; not run by default; committed under test/data/backup/lob_changes, made by its generate.sh); switch_in.sql, a
# partition switched in with no shrink (PR 4c-3: switch_full, switch_diff; not run by default; committed under test/data/backup/switch_in, made by its generate.sh);
# history_break.sql, a full restored over its database between two fulls (review of #612: break_before, break_after;
# not run by default; committed under test/data/backup/history_break, made by its generate.sh);
# clr.sql, CLR columns of every storage form: a Format.Native user type (test/clr's assembly) stored at a fixed
# offset as binary, a user type past 8000 bytes, hierarchyid and geometry, uncompressed, ROW, PAGE and a heap
# (clr_full; not run by default; committed under test/data/backup/clr, made by its generate.sh); clr_types.sql,
# the CLR system types -- every geometry / geography form T5 reads, hierarchyid's -- in every storage form and
# off row (spec 093 T8: clr_types_full, then clr_types_changes.sql's clr_types_diff, paired as clr_types_over;
# 2025's committed under test/data/backup/clr_types); variant.sql,
# sql_variant of every base type in every storage form and off row, then variant_changes.sql's changes and a
# differential (spec 093 T8b: variant_full, variant_diff, paired as variant_over; 2025's committed under
# test/data/backup/variant); datetime2.sql, datetime2(7) at TIMESTAMP_NS's edges and datetime2's in every
# storage form, a temporal table, then datetime2_changes.sql's changes and a differential (spec 093 T8c:
# datetime2_full, datetime2_diff, paired as datetime2_over; 2025's committed under test/data/backup/datetime2); vector.sql, SQL Server 2025's vector (float32 and float16, at their edges and
# dimension limits) in every storage form and off row, then vector_changes.sql's changes and a differential
# (spec 093 T8d: vector_full, vector_diff, paired as vector_over; skipped before 2025, which has no vector;
# committed under test/data/backup/vector);
# tail_window.sql, changes made at known points of a backup's copy -- the backup written into a FIFO this
# script reads, held in its first data pass and inside its second (review of #591: tail_window_full, compared
# with its RESTORE as compare_rows.sh's FUZZY; committed under test/data/backup/tail_window, made by its
# generate.sh, which sets TAIL_WINDOW_REQUIRED=1: a backup that missed its window fails, where the lane's
# only says so);
# bench_changes.sql, a benchmark of 10 million rows and differentials of 10k / 110k / 1.1M changes and an index rebuild
# (PR 4c-3: bench_full, bench_diff_*, each also compressed as *_x; not run by default). A fixture's backups come with the server's own catalog of them where it holds
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
FIXTURES="${FIXTURES:-samples catalog partition files load fuzzy lob large rows dbcs cd page sparse names diffs clr_types tail_window variant datetime2 vector json}"
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
	for kind in tables columns views modules partitioning boundaries partitions schemas; do extras+=("$2.${kind}.txt"); done
}
scalar() {	# $1 = a query answering one value
	sql -h -1 -W -Q "SET NOCOUNT ON; $1" | head -1 | tr -d ' \r'
}
sets=()
extras=()
overlays=()	# name|full set|differential set: what compare_rows restores as a pair
for fixture in ${FIXTURES}; do
	if [ "${fixture}" = large ]; then
		# Its 4.4 GB data file beside the others' databases and backups is close to a runner's disk:
		# the databases whose backups are made go first (the backups stay).
		sql -Q "IF DB_ID('bak_load') IS NOT NULL DROP DATABASE bak_load; IF DB_ID('bak_files') IS NOT NULL DROP DATABASE bak_files" >/dev/null
		df -h / || true
	fi
	if [ "${fixture}" = clr ]; then
		# The user CLR types' assembly (test/clr/build.sh), trusted by its SHA-512 and created in the database
		# clr.sql makes: a script of its own, which clr.sql reads with :r (10 KB of hex in no command line),
		# written straight into the container (no file beside the fixture).
		hex="$(tr -d ' \n' <"${HERE}/../../data/clr/mssql_test_udt.dll.hex")"
		printf "DECLARE @b varbinary(max) = 0x%s;\nDECLARE @h varbinary(64) = HASHBYTES('SHA2_512', @b);\nIF NOT EXISTS (SELECT 1 FROM sys.trusted_assemblies WHERE hash = @h) EXEC sys.sp_add_trusted_assembly @h, N'mssql_test_udt';\nCREATE ASSEMBLY MssqlTestUdt FROM @b WITH PERMISSION_SET = SAFE;\n" \
			"${hex}" | docker exec -i -u 0 "${CONTAINER}" sh -c 'cat >/tmp/gen/clr_assembly.sql && chmod 644 /tmp/gen/clr_assembly.sql'
	fi
	if [ "${major}" -lt 17 ] && { [ "${fixture}" = vector ] || [ "${fixture}" = json ]; }; then
		# SQL Server 2025's vector (spec 093 T8d) and json (spec 086 PR 6) types: an older server has none.
		echo ">> skipping ${fixture}.sql: SQL Server ${major} has no ${fixture} type (2025 on)"
		continue
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
	fuzzy)
		# Spec 085 PR 5: E3's controls under load. The workload in the background (three sessions), then the open transaction
		# once it commits, then a committed change of pre_commit, then the backups (which end by setting
		# backups_done, so the open transaction rolls back), then the workload stopped. Each program's output
		# goes to its own log in /tmp, shown when it did not run.
		for session in 0 1 2; do
			docker exec -d -e SQLCMDPASSWORD "${CONTAINER}" sh -c '"$@" >>/tmp/fuzzy_workload.log 2>&1' sh \
				"${SQLCMD[@]}" -S localhost -U sa -b -v SESSION="${session}" -i /tmp/gen/fuzzy_workload.sql
		done
		fuzzy_ticks() { scalar "SELECT ticks FROM bak_fuzzy.dbo.control WITH (READUNCOMMITTED)" || true; }
		fuzzy_failed() {	# $1 = what went wrong
			echo "ERROR: the fuzzy fixture $1; its logs:" >&2
			docker exec "${CONTAINER}" sh -c 'cat /tmp/fuzzy_workload.log /tmp/fuzzy_open.log 2>/dev/null' >&2 || true
			exit 1
		}
		for _ in $(seq 1 60); do
			[ "$(fuzzy_ticks)" -ge 20 ] 2>/dev/null && break
			sleep 1
		done
		[ "$(fuzzy_ticks)" -ge 20 ] 2>/dev/null || fuzzy_failed "workload did not commit 20 transactions in 60 s"
		docker exec -d -e SQLCMDPASSWORD "${CONTAINER}" sh -c '"$@" >/tmp/fuzzy_open.log 2>&1' sh \
			"${SQLCMD[@]}" -S localhost -U sa -b -i /tmp/gen/fuzzy_open.sql
		opened=""
		for _ in $(seq 1 60); do
			opened="$(scalar "SELECT COUNT(*) FROM bak_fuzzy.dbo.open_only WITH (READUNCOMMITTED) WHERE id > 100000" || true)"
			[ "${opened}" -gt 0 ] 2>/dev/null && break
			sleep 1
		done
		[ "${opened}" -gt 0 ] 2>/dev/null || fuzzy_failed "open transaction did not make its changes in 60 s"
		sql -Q "UPDATE bak_fuzzy.dbo.pre_commit SET v = v + 1 WHERE id % 2 = 0" >/dev/null
		before="$(fuzzy_ticks)"
		echo ">> fuzzy: workload at ${before} transactions, a transaction open; backing up under them"
		run_sql fuzzy_backup.sql
		after="$(fuzzy_ticks)"
		echo ">> fuzzy: backups done at ${after} transactions"
		[ "${after}" -gt "${before}" ] 2>/dev/null || fuzzy_failed "workload stopped before the backups ended (${before} -> ${after})"
		sql -Q "UPDATE bak_fuzzy.dbo.control SET stop_requested = 1" >/dev/null
		left=""
		for _ in $(seq 1 60); do
			left="$(scalar "SELECT COUNT(*) FROM sys.dm_exec_sessions WHERE database_id = DB_ID('bak_fuzzy') AND session_id <> @@SPID" || true)"
			[ "${left}" = 0 ] && break
			sleep 1
		done
		[ "${left}" = 0 ] || fuzzy_failed "programs did not end 60 s after they were asked to (${left} session(s) left)"
		sets+=("fuzzy_base|fuzzy_base.bak" "fuzzy_full|fuzzy_full.bak" "fuzzy_xpress|fuzzy_xpress.bak" "fuzzy_diff|fuzzy_diff.bak")
		overlays+=("fuzzy_over|fuzzy_base|fuzzy_diff")
		;;
	open_tail)
		# Spec 085 PR 5: one transaction held open across a full and a differential (committed under
		# test/data/backup/open_tail by its generate.sh; not run by default). Its program's output goes to a
		# log in /tmp, shown when it did not run.
		docker exec -d -e SQLCMDPASSWORD "${CONTAINER}" sh -c '"$@" >/tmp/open_tail.log 2>&1' sh \
			"${SQLCMD[@]}" -S localhost -U sa -b -i /tmp/gen/open_tail_open.sql
		open_tail_failed() {	# $1 = what went wrong
			echo "ERROR: the open_tail fixture $1; its log:" >&2
			docker exec "${CONTAINER}" cat /tmp/open_tail.log >&2 || true
			exit 1
		}
		opened=""
		for _ in $(seq 1 60); do
			opened="$(scalar "SELECT COUNT(*) FROM bak_open_tail.dbo.changed WITH (READUNCOMMITTED) WHERE id > 200" || true)"
			[ "${opened}" = 3 ] && break
			sleep 1
		done
		[ "${opened}" = 3 ] || open_tail_failed "transaction did not make its changes in 60 s"
		run_sql open_tail_backup.sql
		left=""
		for _ in $(seq 1 60); do
			left="$(scalar "SELECT COUNT(*) FROM sys.dm_exec_sessions WHERE database_id = DB_ID('bak_open_tail') AND session_id <> @@SPID" || true)"
			[ "${left}" = 0 ] && break
			sleep 1
		done
		[ "${left}" = 0 ] || open_tail_failed "transaction did not end 60 s after it was asked to"
		sets+=("open_tail_base|open_tail_base.bak" "open_tail_full|open_tail_full.bak" "open_tail_diff|open_tail_diff.bak")
		;;
	rows)
		golden bak_rows rows_full
		sets+=("rows_full|rows_full.bak")
		;;
	dbcs)
		golden bak_dbcs dbcs_full
		sets+=("dbcs_full|dbcs_full.bak")
		;;
	lob)
		# The compressed set is a copy-only backup taken after the plain one, nothing changed between.
		golden bak_lob lob_full
		sets+=("lob_full|lob_full.bak" "lob_xpress|lob_xpress.bak")
		;;
	large)
		golden bak_large large_xpress
		sets+=("large_xpress|large_xpress.bak")
		;;
	cd)
		# ROW compression (spec 086 PR 1); the compressed set a copy-only backup taken after the plain one.
		golden bak_cd cd_full
		sets+=("cd_full|cd_full.bak" "cd_xpress|cd_xpress.bak")
		;;
	page)
		# PAGE compression (spec 086 PR 2: anchor records and page dictionaries); the compressed set a copy-only
		# backup taken after the plain one. Two states of one database, as files': the golden of page_full taken
		# right after its backups, then page_changes.sql's changes and differential.
		golden bak_page page_full
		run_sql page_changes.sql
		sets+=("page_full|page_full.bak" "page_xpress|page_xpress.bak" "page_diff|page_diff.bak")
		overlays+=("page_over|page_full|page_diff")
		;;
	sparse)
		# Sparse columns (spec 086 PR 3: the record's sparse vector, E24), compressed; then sparse_changes.sql's
		# changes and differential, as page's.
		golden bak_sparse sparse_full
		run_sql sparse_changes.sql
		sets+=("sparse_full|sparse_full.bak" "sparse_diff|sparse_diff.bak")
		overlays+=("sparse_over|sparse_full|sparse_diff")
		;;
	names)
		# Names DuckDB holds otherwise (spec 085 PR 4b): a case-sensitive database, spaces, quotes.
		golden bak_names names_full
		sets+=("names_full|names_full.bak")
		;;
	diffs)
		# A differential over its full (spec 085 PR 4c), plain and compressed; compare_rows restores each pair.
		sets+=("diffs_full|diffs_full.bak" "diffs_diff|diffs_diff.bak" "diffs_diff_xpress|diffs_diff_xpress.bak")
		overlays+=("diffs_over|diffs_full|diffs_diff" "diffs_over_xpress|diffs_full|diffs_diff_xpress")
		;;
	schema_changes)
		# What the catalog alone shows between a full and its differential (spec 085 PR 4c-3): truncates, a
		# delete of every row, pages moved by a shrink, a sliding window. Not an overlay compare_rows takes.
		sets+=("schema_full|schema_full.bak" "schema_diff|schema_diff.bak")
		;;
	lob_changes)
		# Values stored off row changed in every form (spec 085 PR 4c-3, the LOB tail): a differential with no
		# shrink, then one after a shrink that moves the LOB pages, both over the full.
		sets+=("lob_changes_full|lob_changes_full.bak" "lob_changes_diff|lob_changes_diff.bak"
			"lob_changes_shrink|lob_changes_shrink.bak")
		;;
	diff_layouts)
		# Differentials over a striped full of a database of three data files: to one file, in two stripes, and
		# in two stripes compressed, and of one filegroup (DQ-41 revisited: the DCM-computed map). Not run by default.
		sets+=("dlayout_full|dlayout_full_1.bak,dlayout_full_2.bak" "dlayout_diff|dlayout_diff.bak"
			"dlayout_diff_striped|dlayout_diff_s1.bak,dlayout_diff_s2.bak"
			"dlayout_diff_xpress|dlayout_diff_x1.bak,dlayout_diff_x2.bak" "dlayout_fg2_diff|dlayout_fg2_diff.bak"
			"dlayout_fg2_diff_striped|dlayout_fg2_s1.bak,dlayout_fg2_s2.bak")
		;;
	switch_in)
		# A partition switched in with no shrink after it (spec 085 PR 4c-3): only the catalog tells.
		sets+=("switch_full|switch_full.bak" "switch_diff|switch_diff.bak")
		;;
	clr_types)
		# Then clr_types_changes.sql's changes and differential (review of #621: the change listing on two states).
		run_sql clr_types_changes.sql
		sets+=("clr_types_full|clr_types_full.bak" "clr_types_diff|clr_types_diff.bak")
		overlays+=("clr_types_over|clr_types_full|clr_types_diff")
		;;
	variant)
		# sql_variant (spec 093 T8b), compressed; then variant_changes.sql's changes and differential.
		golden bak_variant variant_full
		run_sql variant_changes.sql
		sets+=("variant_full|variant_full.bak" "variant_diff|variant_diff.bak")
		overlays+=("variant_over|variant_full|variant_diff")
		;;
	vector)
		# vector (spec 093 T8d), compressed; then vector_changes.sql's changes and differential. 2025 on.
		golden bak_vector vector_full
		run_sql vector_changes.sql
		sets+=("vector_full|vector_full.bak" "vector_diff|vector_diff.bak")
		overlays+=("vector_over|vector_full|vector_diff")
		;;
	datetime2)
		# datetime2(7) at TIMESTAMP_NS's edges and datetime2's (spec 093 T8c), compressed; then
		# datetime2_changes.sql's changes and differential.
		golden bak_datetime2 datetime2_full
		run_sql datetime2_changes.sql
		sets+=("datetime2_full|datetime2_full.bak" "datetime2_diff|datetime2_diff.bak")
		overlays+=("datetime2_over|datetime2_full|datetime2_diff")
		;;
	json)
		# SQL Server 2025's json type (spec 086 PR 6, E9 / E9b), compressed; then json_changes.sql's changes and
		# differential (skipped above below major 17).
		golden bak_json json_full
		run_sql json_changes.sql
		sets+=("json_full|json_full.bak" "json_diff|json_diff.bak")
		overlays+=("json_over|json_full|json_diff")
		;;
	tail_window)
		# Changes at known points of a backup's data copy (spec 085 D11, review of #591; run by default, so the
		# lane checks it on every version; 2025's committed under test/data/backup/tail_window by its
		# generate.sh). The two open transactions first, in the background; their output to logs in /tmp,
		# which the server's user can write.
		for s in open parallel; do
			docker exec -d -e SQLCMDPASSWORD "${CONTAINER}" sh -c '"$@" >/tmp/tail_window_'"${s}"'.log 2>&1' sh \
				"${SQLCMD[@]}" -S localhost -U sa -b -i "/tmp/gen/tail_window_${s}.sql"
		done
		tw_failed() {	# $1 = what went wrong
			echo "ERROR: the tail_window fixture $1; its logs:" >&2
			docker exec "${CONTAINER}" sh -c 'cat /tmp/tail_window_*.log 2>/dev/null' >&2 || true
			exit 1
		}
		tw_wait() {	# $1 = a query answering 1 when what is waited for holds
			local _
			for _ in $(seq 1 300); do
				[ "$(scalar "$1" || true)" = 1 ] && return 0
				sleep 0.2
			done
			return 1
		}
		tw_lsn() {	# the log's end, in RESTORE HEADERONLY's numeric form (VLF * 10^15 + offset * 10^5 + slot):
			# sys.dm_db_log_stats takes no lock (fn_dblog in the database waited out its lock timeout, 1222,
			# beside the open transactions)
			scalar "SELECT CAST(CONVERT(bigint, CONVERT(varbinary(4), LEFT(l, 8), 2)) AS decimal(25, 0))
				* 1000000000000000 + CONVERT(bigint, CONVERT(varbinary(4), SUBSTRING(l, 10, 8), 2)) * 100000
				+ CONVERT(int, CONVERT(varbinary(2), RIGHT(l, 4), 2))
				FROM (SELECT log_end_lsn AS l FROM sys.dm_db_log_stats(DB_ID(N'bak_tail_window'))) d"
		}
		lsn_le() {	# $1 <= $2, two LSNs of that form (digits: past bash's integers)
			[ "${#1}" -lt "${#2}" ] || { [ "${#1}" -eq "${#2}" ] && [[ ! "$1" > "$2" ]]; }
		}
		tw_wait "SELECT CASE WHEN (SELECT COUNT(*) FROM bak_tail_window.dbo.rollback_insert WITH (READUNCOMMITTED)) = 150
			AND (SELECT COUNT(*) FROM bak_tail_window.dbo.rollback_delete WITH (READUNCOMMITTED)) = 50
			AND (SELECT n FROM tw_control.dbo.opened WITH (READUNCOMMITTED)) = 1
			AND (SELECT COUNT(*) FROM bak_tail_window.dbo.parallel_open WITH (READUNCOMMITTED)) = 2000 THEN 1 ELSE 0 END" ||
			tw_failed "open transactions did not make their changes in 60 s"
		# A trial backup of the same layout: where its second data pass starts is where the real one's does (a
		# stream header, so at a multiple of 512 -- an MSDA of the filler's random bytes is not), and where its
		# log (MQTL) does.
		tw_with="CHECKSUM, BUFFERCOUNT = 2, MAXTRANSFERSIZE = 65536, NAME = 'tail_window_full'"
		sql -Q "BACKUP DATABASE bak_tail_window TO DISK = '${DIR}/tail_window_trial.bak' WITH FORMAT, INIT, COPY_ONLY, ${tw_with}" >/dev/null
		pass2="$(docker exec "${CONTAINER}" sh -c "grep -obUa MSDA '${DIR}/tail_window_trial.bak' | awk -F: '\$1 % 512 == 0 { n++; if (n == 2) print \$1 }'")"
		log_at="$(docker exec "${CONTAINER}" sh -c "grep -obUa MQTL '${DIR}/tail_window_trial.bak' | awk -F: '\$1 % 4 == 0 { print \$1; exit }'")"
		docker exec "${CONTAINER}" rm -f "${DIR}/tail_window_trial.bak" "${DIR}/tail_window_chain.bak" "${DIR}/tail_window_chain.trn"
		# The reader holds twice. With a second data pass (2019 on): 512 KiB in (within the first pass), then
		# 256 KiB or more past the second pass's start (tail_window_pass1.sql at the first, pass2.sql at the
		# second). SQL Server 2017 copies the data once (its pages changed meanwhile are in the log only: the
		# lane's 2017 backups): one hold, 512 KiB before its log -- inside the filler, the file's last extents,
		# past the other tables' pages -- for pass2.sql, the first hold's script skipped. The holds place the
		# scripts only roughly: the server reads ahead of its writes by more than its buffers (measured on 2019,
		# #631's lane: a run held at 512 KiB had already copied the filler, megabytes on; another started its
		# second pass after every change of pass2.sql). So what the copy caught is checked below, from the
		# server's own answer, not assumed; the lane compares the set as fuzzy whatever it caught.
		if [ -n "${pass2}" ]; then
			hold1=8
			hold2=$(((pass2 + 65535) / 65536 + 4 - hold1))
			first_script=tail_window_pass1.sql
			[ "${hold2}" -gt 0 ] || tw_failed "second data pass at ${pass2}, before the first hold"
			echo ">> tail_window: the second data pass at ${pass2}; the reader holds at ${hold1} and $((hold1 + hold2)) blocks of 64 KiB"
		else
			[ -n "${log_at}" ] || tw_failed "trial backup has neither a second data pass nor a log stream"
			hold1=$(((log_at - 524288) / 65536))
			hold2=0
			first_script=""
			[ "${hold1}" -gt 8 ] || tw_failed "log stream at ${log_at}, too close to the data's start"
			echo ">> tail_window: one data pass, its log at ${log_at}; the reader holds at ${hold1} blocks of 64 KiB"
		fi
		docker exec "${CONTAINER}" sh -c "rm -f ${DIR}/tw.fifo ${DIR}/tw.h1 ${DIR}/tw.g1 ${DIR}/tw.h2 ${DIR}/tw.g2 && mkfifo ${DIR}/tw.fifo"
		docker exec -d "${CONTAINER}" sh -c "{ dd bs=65536 count=${hold1} iflag=fullblock 2>/dev/null; touch ${DIR}/tw.h1;
			while [ ! -e ${DIR}/tw.g1 ]; do sleep 0.2; done; dd bs=65536 count=${hold2} iflag=fullblock 2>/dev/null;
			touch ${DIR}/tw.h2; while [ ! -e ${DIR}/tw.g2 ]; do sleep 0.2; done; cat; } <${DIR}/tw.fifo >${DIR}/tail_window_full.bak"
		# The backup through the FIFO. It writes every block, then fails to set the file's size (error 31 in
		# DiskChangeFileSize, 3013; measured on 2022 and 2025): a FIFO has none. The file is whole -- RESTORE
		# VERIFYONLY below. A version that sets no size ends it with its success line, also taken.
		docker exec -d -e SQLCMDPASSWORD "${CONTAINER}" sh -c '"$@" >/tmp/tail_window_backup.log 2>&1' sh \
			"${SQLCMD[@]}" -S localhost -U sa -Q "BACKUP DATABASE bak_tail_window TO DISK = '${DIR}/tw.fifo' WITH FORMAT, INIT, ${tw_with}"
		backing_up="SELECT CASE WHEN EXISTS (SELECT 1 FROM sys.dm_exec_requests WHERE command LIKE 'BACKUP%') THEN 1 ELSE 0 END"
		for hold in 1 2; do
			for _ in $(seq 1 300); do
				docker exec "${CONTAINER}" test -e "${DIR}/tw.h${hold}" && break
				sleep 0.2
			done
			docker exec "${CONTAINER}" test -e "${DIR}/tw.h${hold}" || tw_failed "reader did not reach hold ${hold} in 60 s"
			[ "$(scalar "${backing_up}" || true)" = 1 ] || tw_failed "backup was not running at hold ${hold}"
			echo ">> tail_window: held $(scalar "SELECT CAST(percent_complete AS int) FROM sys.dm_exec_requests WHERE command LIKE 'BACKUP%'")% into the backup"
			if [ "${hold}" = 2 ]; then
				tw_before_pass2="$(tw_lsn)" || tw_failed "could not read the log's end before pass2.sql"
				run_sql tail_window_pass2.sql
			elif [ -n "${first_script}" ]; then
				run_sql "${first_script}"
				tw_after_pass1="$(tw_lsn)" || tw_failed "could not read the log's end after ${first_script}"
			fi
			docker exec "${CONTAINER}" touch "${DIR}/tw.g${hold}"
		done
		tw_wait "SELECT 1 - (${backing_up#SELECT })" || tw_failed "backup did not end 60 s after the reader went on"
		if docker exec "${CONTAINER}" grep -v -e DiskChangeFileSize -e '^Processed ' -e '^Msg 3634' -e '^Msg 3013' -e 'successfully processed' -e 'terminating abnormally' -e '^ *$' /tmp/tail_window_backup.log; then
			tw_failed "backup said more than its FIFO's size error"
		fi
		sql -Q "RESTORE VERIFYONLY FROM DISK = '${DIR}/tail_window_full.bak'" >/dev/null || tw_failed "backup does not verify"
		# The window, from the server's answer: a full backup checkpoints where its last data pass starts
		# (CheckpointLSN; the second pass's, measured: #631's lane). pass2.sql's changes must come after it,
		# else they are in the copy and its tables read consistent; with a second pass, pass1.sql's before it,
		# else the filler is stale. A necessary condition only (where the last pass was when pass2.sql ran is
		# not in the header): the committed set's scenarios are checked by mssql_backup_tail_window.test.
		# Whether log_end_lsn is the last record's LSN or the next one's, a checkpoint equal to it was written
		# after every record before it, so it holds on both sides: a miss is CheckpointLSN > the log's end
		# before pass2.sql, or < the log's end after pass1.sql.
		tw_checkpoint="$(sql -Q "SET NOCOUNT ON; RESTORE HEADERONLY FROM DISK = '${DIR}/tail_window_full.bak'" -s '|' -W |
			awk -F'|' 'NR == 1 { for (i = 1; i <= NF; i++) if ($i == "CheckpointLSN") c = i } NR == 3 && c { print $c }')"
		for v in "${tw_checkpoint}" "${tw_before_pass2}" ${first_script:+"${tw_after_pass1}"}; do
			[[ "${v}" =~ ^[0-9]+$ ]] || tw_failed "an LSN of its window check is not a number: '${v}'"
		done
		tw_missed=""
		if ! lsn_le "${tw_checkpoint}" "${tw_before_pass2}"; then
			tw_missed="its last data pass checkpointed at ${tw_checkpoint}, after pass2.sql began (${tw_before_pass2})"
		elif [ -n "${first_script}" ] && ! lsn_le "${tw_after_pass1}" "${tw_checkpoint}"; then
			tw_missed="its second data pass checkpointed at ${tw_checkpoint}, before pass1.sql ended (${tw_after_pass1})"
		fi
		if [ -z "${tw_missed}" ]; then
			echo ">> tail_window: the window held (CheckpointLSN ${tw_checkpoint})"
		elif [ "${TAIL_WINDOW_REQUIRED:-0}" = 1 ]; then
			tw_failed "missed its window: ${tw_missed}; generate it again"
		else
			echo ">> tail_window: WARNING: missed its window: ${tw_missed} (compared as fuzzy all the same)"
			# On the run's summary too: a lane that misses it every time no longer exercises the dirty cases.
			[ -z "${GITHUB_ACTIONS:-}" ] || echo "::warning::tail_window missed its window: ${tw_missed}"
		fi
		sql -Q "UPDATE tw_control.dbo.phase SET n = 3" >/dev/null
		tw_wait "SELECT CASE WHEN (SELECT COUNT(*) FROM bak_tail_window.dbo.parallel_open WITH (READUNCOMMITTED)) = 0 THEN 1 ELSE 0 END" ||
			tw_failed "open transaction did not end 60 s after it was asked to"
		docker exec "${CONTAINER}" rm -f "${DIR}/tw.fifo" "${DIR}/tw.h1" "${DIR}/tw.g1" "${DIR}/tw.h2" "${DIR}/tw.g2"
		run_sql tail_window_report.sql
		sets+=("tail_window_full|tail_window_full.bak")
		;;
	history_break)
		# A full restored over its database between two fulls (review of #612): a new recovery fork.
		sets+=("break_before|break_before.bak" "break_after|break_after.bak")
		;;
	clr)
		# CLR columns of every storage form (a Format.Native user type at a fixed offset, described as binary).
		sets+=("clr_full|clr_full.bak")
		;;
	bench_changes)
		# A benchmark (spec 085 PR 4c-3): 10 million rows, then differentials of 10k / 110k / 1.1M changes, and
		# one after an index rebuild; each also compressed (_x: the full copy-only, the differentials over bench_full).
		sets+=("bench_full|bench_full.bak" "bench_diff_10k|bench_diff_10k.bak" "bench_diff_110k|bench_diff_110k.bak"
			"bench_diff_1m|bench_diff_1m.bak" "bench_diff_rebuild|bench_diff_rebuild.bak" "bench_full_x|bench_full_x.bak"
			"bench_diff_10k_x|bench_diff_10k_x.bak" "bench_diff_110k_x|bench_diff_110k_x.bak"
			"bench_diff_1m_x|bench_diff_1m_x.bak" "bench_diff_rebuild_x|bench_diff_rebuild_x.bak")
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
: >"${OUT}/overlays.txt"
for entry in "${overlays[@]+"${overlays[@]}"}"; do
	echo "${entry}" >>"${OUT}/overlays.txt"
done

echo ">> done: $(wc -l <"${OUT}/sets.txt" | tr -d ' ') sets in ${OUT}"
