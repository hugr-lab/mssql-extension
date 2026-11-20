#!/usr/bin/env bash
#
# Spec 093 T4: build the user CLR types of test/clr/TestTypes.cs into an assembly and write it as hex
# to test/data/clr/mssql_test_udt.dll.hex, which test/sql/types/clr_udt_raw_scan.test deploys with
# CREATE ASSEMBLY ... FROM 0x<hex> (the server trusts it by its SHA-512, sys.sp_add_trusted_assembly).
#
# The build runs in a .NET SDK container (it pulls the image and, inside it, the NuGet package
# Microsoft.NETFramework.ReferenceAssemblies). Nothing here touches a SQL Server.
#
#   test/clr/build.sh                         # the default image
#   DOTNET_SDK_IMAGE=mcr.microsoft.com/dotnet/sdk:8.0@sha256:... test/clr/build.sh
#
# The build is deterministic (<Deterministic>, no PDB): the same source and SDK give the same bytes.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "${HERE}/../.." && pwd)"
IMAGE="${DOTNET_SDK_IMAGE:-mcr.microsoft.com/dotnet/sdk:8.0}"
OUT_HEX="${REPO}/test/data/clr/mssql_test_udt.dll.hex"

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT
cp "${HERE}/MssqlTestUdt.csproj" "${HERE}/TestTypes.cs" "${WORK}/"

# As root: the SDK's first run takes a named mutex that a user not in the image's passwd cannot create
# (measured, Docker Desktop on arm64: "cannot open the device or file specified : 'NuGet-Migrations'"
# under --user, the same build fine as root); the files are then handed back to the caller.
docker run --rm --tmpfs /tmp:rw,exec,mode=1777 \
	-e HOME=/src/.home -e DOTNET_CLI_HOME=/src/.home -e NUGET_PACKAGES=/src/.nuget \
	-e DOTNET_CLI_TELEMETRY_OPTOUT=1 -e DOTNET_NOLOGO=1 \
	-v "${WORK}:/src" -w /src "${IMAGE}" \
	sh -c "dotnet build MssqlTestUdt.csproj -c Release -o /src/out -nologo; rc=\$?; chown -R $(id -u):$(id -g) /src; exit \$rc"

DLL="${WORK}/out/MssqlTestUdt.dll"
test -s "${DLL}"
mkdir -p "$(dirname "${OUT_HEX}")"
od -An -v -tx1 "${DLL}" | tr -d ' \n' >"${OUT_HEX}"
echo "wrote ${OUT_HEX}: $(wc -c <"${DLL}" | tr -d ' ') bytes"
if command -v shasum >/dev/null; then
	echo "sha512: $(shasum -a 512 "${DLL}" | cut -d' ' -f1)"
fi
