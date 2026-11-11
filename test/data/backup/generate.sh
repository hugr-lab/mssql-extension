#!/usr/bin/env bash
# Spec 085 task 10: regenerate the committed backup samples on a CLEAN, throwaway SQL Server
# instance (spec 085 T4: only synthetic data from an instance of our own is committed).
#
#   test/data/backup/generate.sh            # SQL Server 2025 (the reader's verified version)
#   IMAGE=mcr.microsoft.com/mssql/server:2022-latest test/data/backup/generate.sh
#
# It starts a container of its own (CONTAINER, default mssql-bak-samples; refused if one of that name
# exists), with an SA password generated here and never printed or written to a file, runs
# samples.sql, saves RESTORE HEADERONLY / LABELONLY / FILELISTONLY of every backup as golden text,
# copies the backups out next to this script and removes the container. Nothing else is touched.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE="${IMAGE:-mcr.microsoft.com/mssql/server:2025-latest}"
CONTAINER="${CONTAINER:-mssql-bak-samples}"
SQLCMD=/opt/mssql-tools18/bin/sqlcmd
DIR=/var/opt/mssql/backup/samples

if docker ps -a --format '{{.Names}}' | grep -qx "${CONTAINER}"; then
	echo "ERROR: a container named ${CONTAINER} exists; remove it or set CONTAINER." >&2
	exit 1
fi

# A password of our own, held in this shell's environment only: docker is given the variable's NAME
# (`-e NAME`, which docker reads from its own environment), so the value is on no command line.
SA_PASSWORD="Bk$(openssl rand -hex 16)!a1"
export MSSQL_SA_PASSWORD="${SA_PASSWORD}" SQLCMDPASSWORD="${SA_PASSWORD}"

echo ">> starting ${IMAGE} as ${CONTAINER}"
docker run -d --name "${CONTAINER}" -e ACCEPT_EULA=Y -e MSSQL_SA_PASSWORD "${IMAGE}" >/dev/null
# Only once the container is ours: removed however the script ends.
cleanup() { docker rm -f "${CONTAINER}" >/dev/null 2>&1 || true; }
trap cleanup EXIT

sql() {	# $@ = sqlcmd arguments
	docker exec -e SQLCMDPASSWORD "${CONTAINER}" "${SQLCMD}" -C -S localhost -U sa -b "$@"
}

echo ">> waiting for the server"
for _ in $(seq 1 60); do
	if sql -Q "SELECT 1" >/dev/null 2>&1; then
		break
	fi
	sleep 2
done
sql -Q "SELECT @@VERSION" -h -1 -W

docker exec -u 0 "${CONTAINER}" mkdir -p "${DIR}"
docker exec -u 0 "${CONTAINER}" chown mssql "${DIR}"
docker cp "${HERE}/samples.sql" "${CONTAINER}:/tmp/samples.sql"
echo ">> running samples.sql"
sql -i /tmp/samples.sql

# Golden output: one RESTORE ... ONLY per backup set (a striped set is named by all its families).
golden() {	# $1 = name, $2 = the FROM clause
	for kind in HEADERONLY LABELONLY FILELISTONLY; do
		sql -Q "SET NOCOUNT ON; RESTORE ${kind} FROM $2" -s '|' -W >"${HERE}/$1.$(echo "${kind}" | tr '[:upper:]' '[:lower:]').txt"
	done
}
disk() { printf "DISK = '%s/%s'" "${DIR}" "$1"; }
for name in full full_xpress full_zstd diff; do
	golden "${name}" "$(disk "${name}.bak")"
done
golden log "$(disk log.trn)"
golden stripe "$(disk stripe_1.bak), $(disk stripe_2.bak)"
golden stripe_zstd "$(disk stripe_zstd_1.bak), $(disk stripe_zstd_2.bak)"

echo ">> copying the backups out"
for f in full full_xpress full_zstd diff stripe_1 stripe_2 stripe_zstd_1 stripe_zstd_2; do
	docker cp "${CONTAINER}:${DIR}/${f}.bak" "${HERE}/${f}.bak"
done
docker cp "${CONTAINER}:${DIR}/log.trn" "${HERE}/log.trn"

echo ">> done; sizes:"
ls -l "${HERE}"/*.bak "${HERE}"/*.trn
