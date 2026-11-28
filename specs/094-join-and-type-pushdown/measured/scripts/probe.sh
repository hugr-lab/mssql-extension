#!/usr/bin/env bash
# Spec 094 experiments (measured/X1-X3.md) on a throwaway SQL Server 2025: setup.sql, then each script
# named, e.g. `probe.sh x1_vector.sql` (`SQLCMD_FORMAT="-y 0"` for x2_forms.sql's plans). The password stays
# in this shell's environment.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
IMAGE="$(awk '$1 == 2025 { print $2 }' "${HERE}/../../../../test/backup/gen/images.txt")"
CONTAINER=mssql-x094-probe
SA_PASSWORD="Bk$(openssl rand -hex 16)!a1"
export MSSQL_SA_PASSWORD="${SA_PASSWORD}" SQLCMDPASSWORD="${SA_PASSWORD}"
id="$(docker create --name "${CONTAINER}" -e ACCEPT_EULA=Y -e MSSQL_SA_PASSWORD "${IMAGE}")"
cleanup() { docker rm -f "${id}" >/dev/null 2>&1 || true; }
trap cleanup EXIT
docker start "${id}" >/dev/null
SQLCMD=(/opt/mssql-tools18/bin/sqlcmd -C)
sql() { docker exec -e SQLCMDPASSWORD "${CONTAINER}" "${SQLCMD[@]}" -S localhost -U sa "$@"; }
for _ in $(seq 1 90); do sql -Q "SELECT 1" >/dev/null 2>&1 && break; sleep 2; done
docker exec -u 0 "${CONTAINER}" mkdir -p /tmp/x
for f in "${HERE}"/*.sql; do docker cp "${f}" "${CONTAINER}:/tmp/x/$(basename "${f}")"; done
docker exec -u 0 "${CONTAINER}" chmod -R a+r /tmp/x
sql -b -i /tmp/x/setup.sql
for f in "$@"; do
  echo "##### ${f}"
  sql -d x094 -s '|' ${SQLCMD_FORMAT:--W -w 65535} -i "/tmp/x/${f}" 2>&1 || true
done
