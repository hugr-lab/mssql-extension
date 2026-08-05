#!/usr/bin/env bash
#
# Every `require-env` variable in test/sql must be set by the Makefile, or be on
# the explicit opt-in list below. Spec 063 D7.
#
# WHY THIS EXISTS. A sqllogictest file whose `require-env` variable is unset does
# not fail — it SKIPS, and the run reports success. Spec 057 found this class
# three times in one spec:
#
#   * MSSQL_TEST_SERVER          — 4 files, dormant since they were written;
#   * MSSQL_TEST_CONNECTION_STRING — 4 more, including BOTH auto-TABLOCK tests,
#     which is to say the tests for the behaviour that spec rewrote had never
#     executed;
#   * and the widest, which was not require-env at all but the same shape:
#     `make integration-test` filtered on tags matching 8 of 172 files while the
#     tag the other 164 carry matched NOTHING and exited 0 — 304 assertions
#     reported as a passing suite.
#
# Three instances of one class in one spec is a rate, and the fourth will look
# exactly like the first three. This is the check that turns "nobody noticed"
# into "the build fails".
#
# It deliberately does NOT cover the standalone C++ tests (`make test-cpp`), which
# went stale the same way inside the same spec: they link against the built
# extension archive, and no CI job builds the extension on a pull request at all
# (issue #212). That gate is blocked behind it.
#
# Run: scripts/ci/check_require_env.sh
set -uo pipefail
cd "$(dirname "$0")/../.."

# Variables a developer or a workflow supplies deliberately, and whose absence is
# meant to skip the file. Anything NOT here must come from the Makefile.
OPT_IN='^(AZURE_|MSSQL_COUNTERS$|MSSQL_KERBEROS_TEST$|MSSQL_WINSSPI_TEST$|MSSQL_NAMED_INSTANCE_HOST$)'

fail=0
vars=$(grep -rho 'require-env [A-Za-z_][A-Za-z0-9_]*' test/sql | awk '{print $2}' | sort -u)
if [ -z "$vars" ]; then
	echo "check_require_env: found no require-env lines at all — the search is broken, not the suite" >&2
	exit 1
fi

for v in $vars; do
	if printf '%s' "$v" | grep -qE "$OPT_IN"; then
		continue
	fi
	if grep -qE "^(export )?${v}[[:space:]]*[:?]?=" Makefile; then
		continue
	fi
	files=$(grep -rl "require-env ${v}\b" test/sql | tr '\n' ' ')
	echo "check_require_env: ${v} is set by nothing — these files SKIP silently:" >&2
	echo "    ${files}" >&2
	echo "    Fix: set and export it in the Makefile, or add it to OPT_IN in $0" >&2
	fail=1
done

if [ "$fail" -ne 0 ]; then
	exit 1
fi
echo "check_require_env: OK — $(printf '%s\n' $vars | wc -l | tr -d ' ') variables, all set or opt-in"
