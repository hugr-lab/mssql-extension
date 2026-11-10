#!/usr/bin/env bash
# Spec 085: generate backups of the synthetic database (samples.sql) on a CLEAN, throwaway SQL Server
# instance, with the golden RESTORE HEADERONLY / LABELONLY / FILELISTONLY of each beside it (spec 085
# T4: only synthetic data from an instance of our own).
#
#   OUT=/tmp/bak VERSION=2019 test/backup/gen/generate.sh     # an image of images.txt, by digest
#   OUT=/tmp/bak IMAGE=mcr.microsoft.com/mssql/server:2022-latest test/backup/gen/generate.sh
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
# mssql-tools18, which do unless told -C (the container's certificate is self-signed).
if docker exec "${CONTAINER}" test -x /opt/mssql-tools18/bin/sqlcmd; then
	SQLCMD=(/opt/mssql-tools18/bin/sqlcmd -C)
else
	SQLCMD=(/opt/mssql-tools/bin/sqlcmd)
fi
sql() {	# $@ = sqlcmd arguments
	docker exec -e SQLCMDPASSWORD "${CONTAINER}" "${SQLCMD[@]}" -S localhost -U sa -b "$@"
}

echo ">> waiting for the server"
ready=0
for _ in $(seq 1 90); do
	if sql -Q "SELECT 1" >/dev/null 2>&1; then
		ready=1
		break
	fi
	if [ "$(docker inspect -f '{{.State.Running}}' "${CONTAINER}")" != true ]; then
		break
	fi
	sleep 2
done
if [ "${ready}" != 1 ]; then
	echo "ERROR: the server in ${IMAGE} did not start; its log:" >&2
	docker logs "${CONTAINER}" 2>&1 | tail -20 >&2
	exit 1
fi
sql -h -1 -W -Q "SET NOCOUNT ON; SELECT CAST(SERVERPROPERTY('ProductVersion') AS varchar(32))" | head -1 >"${OUT}/server.txt"
major="$(cut -d. -f1 "${OUT}/server.txt")"
echo ">> server $(cat "${OUT}/server.txt")"

docker exec -u 0 "${CONTAINER}" mkdir -p "${DIR}"
docker exec -u 0 "${CONTAINER}" chown mssql "${DIR}"
docker cp "${HERE}/samples.sql" "${CONTAINER}:/tmp/samples.sql"
echo ">> running samples.sql"
sql -v DIR="${DIR}" -i /tmp/samples.sql

# The sets samples.sql made on this version, in its order.
sets=("full|full.bak" "full_xpress|full_xpress.bak")
if [ "${major}" -ge 17 ]; then sets+=("full_zstd|full_zstd.bak"); fi
sets+=("stripe|stripe_1.bak,stripe_2.bak")
if [ "${major}" -ge 17 ]; then sets+=("stripe_zstd|stripe_zstd_1.bak,stripe_zstd_2.bak"); fi
sets+=("diff|diff.bak" "log|log.trn")

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

echo ">> done: $(wc -l <"${OUT}/sets.txt" | tr -d ' ') sets in ${OUT}"
