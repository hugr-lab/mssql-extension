# Quickstart / Verification: Lazy GSSAPI Linking

How to validate spec 053 against its success criteria.

## 1. No hard dependency in the binary (SC-002)

```bash
make                       # release build on Linux (with Kerberos dev headers present)
ldd build/release/extension/mssql/mssql.duckdb_extension | grep -iE 'gssapi|krb5'
# Expect: no output. libgssapi_krb5.so.2 / libkrb5.so.3 must NOT be listed.
```

(macOS: `otool -L` should still show the GSS framework — unchanged.)

## 2. Loads on a clean image — the #161 repro (SC-001, SC-005)

```dockerfile
FROM ghcr.io/astral-sh/uv:python3.11-trixie-slim AS uv
RUN uv init && uv add duckdb
# No libgssapi-krb5-2 installed.
RUN uv run python -c "import duckdb; c=duckdb.connect(':memory:'); \
    c.execute('INSTALL mssql FROM community'); c.execute('LOAD mssql'); \
    print('loaded OK')"
```

Expect `loaded OK` (previously: `IOException ... libgssapi_krb5.so.2: cannot open
shared object file`). A SQL-auth `ATTACH` should also work with no Kerberos package.

## 3. Kerberos still works when the runtime is present (SC-003)

```bash
cd test/kerberos
docker compose up -d --build
docker compose exec test-client /run-tests.sh   # all Kerberos auth tests pass
docker compose down -v
```

## 4. Clear error when Kerberos requested but unavailable (SC-004)

On a clean image (no Kerberos runtime), after `LOAD mssql`:

```sql
-- Either of these should report the missing library + install package,
-- NOT crash and NOT a generic error:
SELECT mssql_kerberos_auth_test('sql.example.com');
-- or an ATTACH with authenticator=krb5 / Trusted_Connection=yes
```

Expect a message containing `libgssapi_krb5.so.2` and `libgssapi-krb5-2`
(Debian/Ubuntu) / `krb5-libs` (RHEL/Fedora).

## 5. Unit assertion

Add/confirm a C++ unit test (`test/cpp/`) that drives the unavailable-runtime path
(or asserts the error-message text) so SC-004 is covered without a special image.
