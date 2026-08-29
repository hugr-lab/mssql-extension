#!/usr/bin/env bash
#
# Three checks on how test/sql files gate themselves. Spec 063 D7.
#
# WHY THIS EXISTS. A sqllogictest file whose gate is unsatisfiable does not fail —
# it SKIPS, and the run reports success. Spec 057 found this class three times in
# one spec:
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
# The fourth arrived on PR #264 and this script printed OK on it, twice over, so
# it now runs three checks instead of one:
#
#   1. EXPORTED, not merely assigned. The old test was
#      `^(export )?VAR[[:space:]]*[:?]?=`, which an unexported assignment
#      satisfies. MSSQL_TEST_DSN_TLS was assigned at Makefile:96 with its export
#      commented out at :132, so it matched while never reaching the child
#      process — four TLS files skipping while this script said OK.
#
#   2. OPT-IN NAMES MUST EXIST SOMEWHERE. Opt-in variables skip check 1 by
#      design (a developer supplies them), but AZURE_SQL_HOST / AZURE_SQL_DATABASE
#      appeared in NOTHING but the test files that gated on them — the repo
#      documents AZURE_SQL_DB_HOST / AZURE_SQL_DB — so no environment anyone
#      could build from the docs ever satisfied them. An opt-in variable must be
#      named in the Makefile, .env.example, docs/ or a workflow, or it is a typo
#      with a skip attached.
#
#   3. GATES BEFORE ASSERTIONS. A `require` that fails mid-file aborts and marks
#      the WHOLE file skipped, discarding the assertions ABOVE it too.
#      azure_secret_validation.test had four credential-free assertions on this
#      extension's own validation sitting above a `require azure`, so they
#      reported as skipped rather than passed everywhere without Azure. Split the
#      file instead; the gates belong at the top.
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
# meant to skip the file. Anything NOT here must be exported by the Makefile.
OPT_IN='^(AZURE_|MSSQL_COUNTERS$|MSSQL_KERBEROS_TEST$|MSSQL_WINSSPI_TEST$|MSSQL_NAMED_INSTANCE_HOST$)'

# The OTHER lane that runs this suite. `make integration-test` is not what CI
# uses; this is (.github/workflows/ci.yml, "Run integration tests").
CI_LANE='scripts/ci/integration_test.sh'

fail=0

#===----------------------------------------------------------------------===#
# Check 1 + 2: every require-env variable is exported, or is a documented opt-in
#===----------------------------------------------------------------------===#
vars=$(grep -rho 'require-env [A-Za-z_][A-Za-z0-9_]*' test/sql | awk '{print $2}' | sort -u)
if [ -z "$vars" ]; then
	echo "check_require_env: found no require-env lines at all — the search is broken, not the suite" >&2
	exit 1
fi

for v in $vars; do
	files=$(grep -rl "require-env ${v}\b" test/sql | tr '\n' ' ')
	if printf '%s' "$v" | grep -qE "$OPT_IN"; then
		# Opt-in: the Makefile is not required to set it, but SOMETHING outside
		# test/sql has to name it, or nobody can know it exists.
		if grep -rqE "\b${v}\b" Makefile .env.example docs .github/workflows 2>/dev/null; then
			continue
		fi
		echo "check_require_env: ${v} is opt-in but is named NOWHERE outside test/sql —" >&2
		echo "    no environment built from the docs can satisfy it. These files SKIP silently:" >&2
		echo "    ${files}" >&2
		echo "    Fix: use the documented name, or document this one in .env.example + docs/TESTING.md" >&2
		fail=1
		continue
	fi
	# Assigned is not enough — an unexported make variable never reaches the
	# test binary. Require an actual `export`.
	#
	# And require it in BOTH lanes (issue #278). Two different things run this
	# suite: `make integration-test` and, in CI, scripts/ci/integration_test.sh,
	# which builds its own selection and exports its own environment. This check
	# used to look only at the Makefile — so when MSSQL_TEST_DSN_TLS was fixed
	# there, the four TLS files kept skipping in CI and this script kept printing
	# OK. A variable exported in one lane and not the other is exactly the shape
	# that produced that.
	missing=""
	makefile_missing=0
	if ! grep -qE "^export[[:space:]]+${v}([[:space:]]|=|:=|\?=|$)" Makefile; then
		missing="Makefile"
		makefile_missing=1
	fi
	grep -qE "^export[[:space:]]+${v}=" "$CI_LANE" || missing="${missing:+${missing} and }${CI_LANE}"
	if [ -z "$missing" ]; then
		continue
	fi
	echo "check_require_env: ${v} is not exported by ${missing} — these files SKIP silently there:" >&2
	echo "    ${files}" >&2
	# Gate on the MAKEFILE leg alone (job 1182). Testing `missing = "Makefile"`
	# suppressed this hint whenever BOTH lanes were missing — which is exactly the
	# historical case that motivated the script: MSSQL_TEST_DSN_TLS assigned at
	# Makefile:96 with its export commented out AND absent from the CI lane.
	if [ "$makefile_missing" -eq 1 ] && grep -qE "^[[:space:]]*${v}[[:space:]]*[:?]?=" Makefile; then
		echo "    (it IS assigned in the Makefile, but never exported — assignment alone" >&2
		echo "     does not reach the child process.)" >&2
	fi
	echo "    Fix: export it in BOTH the Makefile and ${CI_LANE}, or add it to OPT_IN in $0" >&2
	fail=1
done

#===----------------------------------------------------------------------===#
# Check 3: no gate below the first assertion
#===----------------------------------------------------------------------===#
while IFS= read -r f; do
	offender=$(awk '
		/^(statement|query)[ \t]/ { if (!seen) { seen = 1; first = NR } ; next }
		/^require([ \t]|-env[ \t])/ { if (seen) { print NR ": " $0; exit } }
	' "$f")
	if [ -n "$offender" ]; then
		echo "check_require_env: ${f} has a gate below its first assertion:" >&2
		echo "    ${offender}" >&2
		echo "    A failing 'require' aborts and marks the WHOLE file skipped, so the" >&2
		echo "    assertions ABOVE it report as skipped rather than passed." >&2
		echo "    Fix: move the gate to the top, or split the file in two." >&2
		fail=1
	fi
done < <(find test/sql -name '*.test' | sort)

if [ "$fail" -ne 0 ]; then
	exit 1
fi
echo "check_require_env: OK — $(printf '%s\n' $vars | wc -l | tr -d ' ') variables exported or documented-opt-in, no gate below an assertion"
