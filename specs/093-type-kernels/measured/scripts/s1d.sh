#!/usr/bin/env bash
# Spec 093 T8b, experiment S1d: run s1d.sql on a throwaway SQL Server of each version given, print the
# server's answer, copy the backup out and print the stored bytes the backup reader reads for each row
# (mssql_backup_scan, a sql_variant still its bytes before T8b). Usage: s1d.sh <duckdb> <out dir> <version>...
# The password is generated here and kept in the environment of the docker calls only.
set -euo pipefail
DUCKDB="$1"
OUT="$2"
shift 2
HERE="$(cd "$(dirname "$0")" && pwd)"
IMAGES="${HERE}/../../../../test/backup/gen/images.txt"
mkdir -p "${OUT}"
for version in "$@"; do
	image="$(awk -v v="${version}" '$1 == v { print $2 }' "${IMAGES}")"
	container="mssql-s1d-${version}"
	export MSSQL_SA_PASSWORD="Bk$(openssl rand -hex 16)!a1"
	export SQLCMDPASSWORD="${MSSQL_SA_PASSWORD}"
	id="$(docker create --name "${container}" -e ACCEPT_EULA=Y -e MSSQL_SA_PASSWORD "${image}")"
	trap 'docker rm -f "${id}" >/dev/null 2>&1 || true' EXIT
	docker start "${id}" >/dev/null
	tools=/opt/mssql-tools18/bin/sqlcmd
	docker exec "${container}" test -x "${tools}" || tools=/opt/mssql-tools/bin/sqlcmd
	sql() { docker exec -e SQLCMDPASSWORD "${container}" "${tools}" -C -S localhost -U sa "$@"; }
	for _ in $(seq 1 90); do sql -Q "SELECT 1" >/dev/null 2>&1 && break; sleep 2; done
	docker cp "${HERE}/s1d.sql" "${container}:/tmp/s1d.sql"
	sql -b -s '|' -W -w 65535 -i /tmp/s1d.sql >"${OUT}/server_${version}.txt"
	docker cp "${container}:/var/opt/mssql/data/s1d.bak" "${OUT}/s1d_${version}.bak"
	docker rm -f "${id}" >/dev/null
	trap - EXIT
	"${DUCKDB}" -no-agent -csv -c "SELECT id, hex(v) AS stored FROM mssql_backup_scan('${OUT}/s1d_${version}.bak', 'dbo', 's1') ORDER BY id" \
		>"${OUT}/backup_${version}.csv"
	echo ">> ${version}: $(grep -c '|' "${OUT}/server_${version}.txt") server rows, $(($(wc -l <"${OUT}/backup_${version}.csv") - 1)) backup rows"
done
