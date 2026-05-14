# Run the Windows SSPI sqllogictest suite (spec 042 Phase 4).
#
# Bundles the three env vars + the unittest invocation into one command.
# Run from a domain-joined Windows host with valid Kerberos credentials
# (verify with `klist`) against any AD-joined SQL Server you have access to.
#
# Usage:
#   .\scripts\ci\winsspi_test.ps1 -SqlHost sqlhost.corp.example.com -Database master
#
# (-SqlHost, not -Host -- $Host is a PowerShell automatic variable for the host UI.)
#
# Optional:
#   -UnittestExe   path to unittest.exe (default: build\release\test\unittest.exe)
#   -TestFile      sqllogictest file (default: the bundled winsspi suite)
#
# The script does NOT run kinit -- on Windows, your interactive logon session
# already holds a TGT if you're signed in as a domain user. If `klist` shows
# nothing, log out and back in (or use `runas /netonly` against domain creds).

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SqlHost,

    [Parameter(Mandatory = $true)]
    [string]$Database,

    [string]$UnittestExe = "build\release\test\unittest.exe",

    [string]$TestFile = "test\sql\integrated_auth\winsspi_basic.test"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $UnittestExe)) {
    Write-Error @"
unittest.exe not found at '$UnittestExe'.
Either build the extension locally with 'make' (produces build\release\test\unittest.exe)
or download the 'unittest-windows_amd64' artifact from a green CI run and unzip it
to that path.
"@
}

if (-not (Test-Path $TestFile)) {
    Write-Error "Test file not found: $TestFile"
}

Write-Host "[winsspi-test] verifying Kerberos ticket..." -ForegroundColor Cyan
$klist = & klist 2>&1
if ($LASTEXITCODE -ne 0 -or $klist -notmatch "krbtgt") {
    Write-Warning @"
klist did not report a krbtgt entry. The Windows SSPI tests need a valid Kerberos
ticket in the current logon session. If this is unexpected:
  - Confirm you're signed in as a domain user (not a local account)
  - Try `runas /netonly /user:DOMAIN\you cmd.exe` and re-run from that shell
Continuing anyway; tests will fail with SEC_E_NO_CREDENTIALS if there's no ticket.
"@
}

$env:MSSQL_WINSSPI_TEST = "1"
$env:MSSQL_TEST_HOST    = $SqlHost
$env:MSSQL_TEST_DB      = $Database

Write-Host "[winsspi-test] running $TestFile against $SqlHost / $Database..." -ForegroundColor Cyan
& $UnittestExe $TestFile
$exit = $LASTEXITCODE

if ($exit -eq 0) {
    Write-Host "[winsspi-test] all cases passed" -ForegroundColor Green
} else {
    Write-Host "[winsspi-test] FAILED (exit $exit)" -ForegroundColor Red
}
exit $exit
