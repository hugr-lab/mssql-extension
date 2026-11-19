#!/usr/bin/env bash
# Spec 085 task 25: the backup functions read through s3:// and https:// answer as over local files.
#
#   DUCKDB=build/release/duckdb test/backup/compare_remote.sh      # a build with httpfs linked in
#
# Starts an S3 server of its own -- RustFS (Apache-2.0; MinIO publishes no images any more) -- in a
# container (CONTAINER, default mssql-backup-s3; refused if one of that name exists) on 127.0.0.1:PORT
# (default 9443), with TLS on a certificate made here for localhost
# and credentials generated here (in this shell's environment only: docker is given the variables'
# NAMES), puts DIR (default test/data/backup: the samples and the databases beside them) into a bucket,
# lets it be read anonymously too (for https://, no S3 API), runs compare_remote.py against both, and
# removes the container and the certificate however the script ends. The oracle lane runs it on its
# httpfs build. Needs docker, openssl, curl and the aws CLI.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DUCKDB="${DUCKDB:-build/release/duckdb}"
DIR="${DIR:-test/data/backup}"
PORT="${PORT:-9443}"
CONTAINER="${CONTAINER:-mssql-backup-s3}"
# RustFS 1.0.4, by the digest of its multi-platform index (as images.txt pins the SQL Server images).
IMAGE="rustfs/rustfs@sha256:ed9210d6411d77ffceb70d94b964a3cb7d225983368bc25bebe01e995293dcc2"
BUCKET=backups
# Refused when a container of the name exists; once started, the container is removed by its own id
# (never by name: a second run, or one an earlier docker error let through, must not remove another's).
if docker container inspect "${CONTAINER}" >/dev/null 2>&1; then
	echo "ERROR: a container named ${CONTAINER} exists; remove it or set CONTAINER." >&2
	exit 1
fi
# The caller's AWS profile and session are not this server's.
unset AWS_PROFILE AWS_DEFAULT_PROFILE AWS_SESSION_TOKEN AWS_SECURITY_TOKEN
work="$(mktemp -d)"
id=""
cleanup() {
	# -v: the anonymous volume of /data with it (the bind mount of the certificates is never removed).
	[ -n "${id}" ] && docker rm -fv "${id}" >/dev/null 2>&1
	rm -rf "${work}"
}
trap cleanup EXIT
mkdir -p "${work}/certs"
openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=localhost \
	-addext "subjectAltName=DNS:localhost,IP:127.0.0.1" \
	-keyout "${work}/certs/rustfs_key.pem" -out "${work}/certs/rustfs_cert.pem" 2>"${work}/openssl.err" ||
	{ echo "ERROR: openssl could not make the certificate:" >&2; cat "${work}/openssl.err" >&2; exit 1; }
chmod 644 "${work}/certs/rustfs_key.pem"
CERT="${work}/certs/rustfs_cert.pem"
# Plain shell variables, given only to the processes that need them (as environment: no command line
# shows them).
access_key="$(openssl rand -hex 12)"
secret_key="$(openssl rand -hex 24)"
# Created, then started: the id is known before a start can fail (a port taken, a mount refused), so
# the trap removes a container that never ran too.
id="$(RUSTFS_ACCESS_KEY="${access_key}" RUSTFS_SECRET_KEY="${secret_key}" docker create --name "${CONTAINER}" \
	-p "127.0.0.1:${PORT}:9000" -e RUSTFS_ACCESS_KEY -e RUSTFS_SECRET_KEY -e RUSTFS_TLS_PATH=/tls \
	-e RUSTFS_ADDRESS=0.0.0.0:9000 -v "${work}/certs:/tls:ro" --entrypoint rustfs "${IMAGE}" server /data)"
docker start "${id}" >/dev/null
# Up once it answers over TLS at all (an anonymous request is refused: any HTTP status will do).
up() { [ "$(curl -s -o /dev/null -w '%{http_code}' --cacert "${CERT}" "https://localhost:${PORT}/")" != 000 ]; }
for _ in $(seq 1 60); do
	up && break
	sleep 1
done
if ! up; then
	log="$(docker logs "${id}" 2>&1 | tail -20)"
	log="${log//"${secret_key}"/***}"
	log="${log//"${access_key}"/***}"
	echo "ERROR: the S3 server did not start; its log:" >&2
	printf '%s\n' "${log}" >&2
	exit 1
fi
# The aws CLI on a configuration of its own (the user's is not read, nor changed): the classic transfer
# client, which takes AWS_CA_BUNDLE (the CRT one negotiates TLS on its own trust store).
printf '[default]\ns3 =\n  preferred_transfer_client = classic\n' >"${work}/aws_config"
s3() {
	AWS_CONFIG_FILE="${work}/aws_config" AWS_SHARED_CREDENTIALS_FILE=/dev/null \
		AWS_ACCESS_KEY_ID="${access_key}" AWS_SECRET_ACCESS_KEY="${secret_key}" \
		AWS_DEFAULT_REGION=us-east-1 AWS_CA_BUNDLE="${CERT}" \
		AWS_REQUEST_CHECKSUM_CALCULATION=when_required AWS_RESPONSE_CHECKSUM_VALIDATION=when_required \
		aws --endpoint-url "https://localhost:${PORT}" "$@"
}
s3 s3 mb "s3://${BUCKET}" >/dev/null
s3 s3 cp --recursive --only-show-errors "${DIR}" "s3://${BUCKET}/samples" --exclude '*.py*'
s3 s3api put-bucket-policy --bucket "${BUCKET}" --policy "{\"Version\":\"2012-10-17\",\"Statement\":[{\"Effect\":\"Allow\",\"Principal\":{\"AWS\":[\"*\"]},\"Action\":[\"s3:GetObject\"],\"Resource\":[\"arn:aws:s3:::${BUCKET}/*\"]}]}"
MSSQL_S3_KEY_ID="${access_key}" MSSQL_S3_SECRET="${secret_key}" MSSQL_S3_ENDPOINT="localhost:${PORT}" \
	MSSQL_CA_CERT_FILE="${CERT}" \
	python3 -I "${HERE}/compare_remote.py" --duckdb "${DUCKDB}" --local "${DIR}" \
	--remote "s3://${BUCKET}/samples" --remote "https://localhost:${PORT}/${BUCKET}/samples"
