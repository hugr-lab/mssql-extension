#!/usr/bin/env python3
"""Gate: no credential value reaches a log, exception or format call.

WHY A GATE AND NOT A QUERY
--------------------------
Joern's q6 ("credential inside a log, exception or format expression") already
encodes this check, and it found nothing the last time it ran. The problem is
that it runs when someone remembers: it needs Joern installed and a CPG built,
which is minutes, so it lives in `make joern-scan` as an exploratory tool. The
one deterministic check in the tree -- test/sql/tds_connection/
pool_stats_no_credentials.test, spec 047 SC-005 -- covers a single function.

Credential disclosure is a regression class, not a discovery class: the fix for
issue #492 (mssql_azure_auth_test showed the last 3 characters of an access
token, and a short token in full) is exactly the kind of thing that comes back
when a new error message is written next to a credential. So this is a grep-
shaped gate: no dependencies, runs in a second, and cannot rot.

It is deliberately the STRUCTURAL form of q6 and makes the same trade. The
dataflow form was tried in Joern and is unusable here -- it taints a call's
result with its arguments, so every debug fprintf downstream of DoLogin7
reported as reached by `password` (28 flows, all false).

WHAT IT FLAGS
-------------
A statement that calls a message sink (an exception constructor, printf-family,
StringUtil::Format, a DuckDB log macro) and mentions an identifier that looks
like it HOLDS a credential.

WHAT IT DOES NOT FLAG
---------------------
  * The name OF a credential: `secret_name`, `secret_type`, `token_id`. Logging
    those is what the error messages are supposed to do ("Secret 'x' not
    found"), and flagging them would bury the one finding that matters.
  * Words inside string literals. A message reading "Login failed for user" or
    "password is required" discloses nothing; only an interpolated VALUE does.
    So literals are stripped before matching -- which is also why a format
    string like "pwd=%s" is caught by its ARGUMENT, not by its text.
  * Anything in ALLOWLIST below, each entry carrying the reason it is safe.

EXIT CODES
----------
  0  clean
  1  at least one unallowlisted hit (prints file:line and the statement)
  2  bad invocation

Run: scripts/ci/check_credential_logging.py [--root src] [--list-allowlisted]
"""

import argparse
import os
import re
import sys

# --- what counts as a message sink -------------------------------------------
# Anything whose argument becomes human-readable text: a thrown exception's
# message, a debug fprintf, a formatted string, a log line.
SINK = re.compile(
    r"\b("
    r"[A-Za-z_][A-Za-z0-9_]*Exception"       # InvalidInputException, IOException, ...
    r"|f?printf|sn?printf|puts|fputs"
    r"|StringUtil::Format"
    r"|DUCKDB_LOG[A-Z_]*"
    r"|MSSQL_LOG[A-Z_]*"
    r"|Log(?:Debug|Info|Warning|Error|Trace)"
    r")\b"
)

# --- what looks like a credential VALUE --------------------------------------
# Same vocabulary as joern/scan.sc's q6 `secretish`.
SECRETISH = re.compile(
    r"\b[A-Za-z0-9_]*("
    r"password|passwd|pwd|secret|access_token|refresh_token|bearer"
    r"|keytab|credential|connection_string"
    r")[A-Za-z0-9_]*\b",
    re.IGNORECASE,
)

# --- what is the NAME of a credential, not the credential --------------------
# q6's `identifierOfSecret`, as a full-identifier test.
NAME_OF_SECRET = re.compile(
    r"^[A-Za-z0-9_]*(_name|_type|_id|_key_name|_kind|name|type|kind)$", re.IGNORECASE
)

# A credential VALUE in this codebase is a snake_case local or member -- that is
# CLAUDE.md's naming rule, not a guess: locals are `snake_case`, members
# `snake_case_`, constants `UPPER_SNAKE_CASE`, types and methods `PascalCase`.
#
# So an ALL-CAPS identifier is a compile-time constant and cannot hold a runtime
# credential. The ones that trip SECRETISH are the secret KEY NAMES
# (MSSQL_SECRET_PASSWORD) and protocol constants (FEDAUTH_ADAL_WORKFLOW_PASSWORD,
# GSS_C_NO_CREDENTIAL) -- names of a credential, like q6's `identifierOfSecret`.
CONSTANT = re.compile(r"^[A-Z][A-Z0-9_]*$")

# PascalCase is a type or a method. A type name (KeyValueSecret, AzureSecretInfo)
# is not a value. A METHOD could return one, so those are not skipped wholesale:
# only names that read as a constructor//accessor-of-plumbing rather than as a
# credential getter. `GetPassword()` deliberately still reports.
PLUMBING_CALL = re.compile(
    r"^(Create|Read|Validate|Acquire|Free|Delete|Register|Lookup|Encode|Decode|Obfuscate)"
    r"[A-Za-z0-9_]*$"
)
TYPE_NAME = re.compile(
    r"^(?:[A-Z][a-z0-9]+)+$"  # KeyValueSecret, SecretManager, AzureSecretInfo, Keytab
)

# --- identifiers that merely CONTAIN a credential word -----------------------
# Each is an object, a function, a count or a protocol field -- never a
# credential value. Matched against the whole identifier.
ALLOWLIST = {
    # Containers and plumbing: the manager/reader/entry, not the value.
    "secret_manager": "the SecretManager object, not a secret",
    "secret_entry": "a SecretEntry handle; its ToString() redacts by DuckDB contract",
    "secret_vec": "a vector of secret NAMES for a lookup",
    "secret_name_vec": "a vector of secret names",
    "azure_secret_reader": "the reader object",
    "kv_secret": "a KeyValueSecret handle, not a value",
    "mssql_secret": "the secret TYPE name, a literal",
    "secrets": "a collection of secret names",
    "secret_count": "a count",
    # Function and API names that appear in the same statement as a sink.
    "obfuscate_password": "the LOGIN7 obfuscation function's NAME",
    "get_init_creds_password": "a krb5 API symbol named in an error message",
    "mssql_kerberos_auth_test_secret": "a registered function name",
    "has_password": "a boolean",
    "has_access_token": "a boolean",
    "has_azure_secret": "a boolean",
    # Protocol field offsets/lengths in the LOGIN7 layout, not the value.
    "sspi_attrdb_chgpwd_offset": "a LOGIN7 field offset",
    "password_offset": "a LOGIN7 field offset",
    "password_length": "a length, used to prove the field was written",
}

SKIP_DIRS = {"build", "duckdb", "vcpkg", ".git", "third_party"}

IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def strip_comments_and_literals(text):
    """Blank out comments and string/char literals, preserving line structure.

    Literals go because a message's TEXT discloses nothing -- only an
    interpolated value does. Newlines are preserved so reported line numbers
    stay true to the file.
    """
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            i = min(i + 2, n)
        elif c in ('"', "'"):
            quote = c
            i += 1
            while i < n and text[i] != quote:
                if text[i] == "\\":
                    i += 1
                if i < n:
                    out.append("\n" if text[i] == "\n" else " ")
                i += 1
            i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def strip_preprocessor(text):
    """Blank out preprocessor directives, preserving line structure.

    They carry no `;`, so leaving them in makes the whole file one statement --
    which is exactly how the first cut of this script reported every finding at
    line 1. Continuation lines ending in `\\` belong to the directive too.
    """
    out = []
    in_directive = False
    for line in text.split("\n"):
        stripped = line.lstrip()
        if not in_directive and stripped.startswith("#"):
            in_directive = True
        if in_directive:
            out.append("")
            in_directive = line.rstrip().endswith("\\")
        else:
            out.append(line)
    return "\n".join(out)


def statements(text):
    """Yield (start_line, statement_text) for one statement at a time.

    Depth counts PARENTHESES and brackets only -- never braces. Counting braces
    was the first cut's bug: a function body kept depth >= 1, so no `;` inside
    one ever closed a statement and each file came back as a single blob.

    A brace is instead a boundary in its own right, which separates a function
    body from its signature and one statement from the next.
    """
    buf = []
    start_line = 1
    line = 1
    depth = 0
    for ch in text:
        if ch == "\n":
            line += 1
        if not buf or "".join(buf).isspace():
            start_line = line
        buf.append(ch)
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth = max(0, depth - 1)
        elif depth == 0 and ch in ";{}":
            yield start_line, "".join(buf)
            buf = []
            start_line = line
    if buf and not "".join(buf).isspace():
        yield start_line, "".join(buf)


# An accessor that returns the NAME or TYPE of a credential rather than the
# credential. `secret->GetType()` yields "mssql" / "azure"; interpolating it is
# what the "Secret 'x' is not of type 'y'" errors are for.
#
# These expressions are deleted from the statement before identifiers are
# collected, so a credential-looking receiver used ONLY this way disappears --
# but the same receiver passed anywhere else in the same statement still
# reports. That is why this is not an ALLOWLIST entry for `secret`: blanket-
# excusing a name that broad would wave through a future `fprintf("%s", secret)`.
#
# Motivated by the only two hits in the tree: mssql_storage.cpp:188 and
# azure_secret_reader.cpp:44, both `throw ...(fmt, secret_name, secret->GetType())`.
SAFE_ACCESSOR = re.compile(
    r"[A-Za-z_][A-Za-z0-9_]*\s*(?:->|\.)\s*Get(?:Type|Name)\s*\(\s*\)"
)


def offending_identifiers(stmt):
    """Credential-looking identifiers in `stmt` that are not excused."""
    stmt = SAFE_ACCESSOR.sub(" ", stmt)
    hits = []
    for ident in set(IDENT.findall(stmt)):
        if not SECRETISH.fullmatch(ident) and not SECRETISH.match(ident):
            continue
        if not SECRETISH.search(ident):
            continue
        if ident in ALLOWLIST:
            continue
        if NAME_OF_SECRET.match(ident):
            continue
        if CONSTANT.match(ident):
            continue
        if TYPE_NAME.match(ident) or PLUMBING_CALL.match(ident):
            continue
        hits.append(ident)
    return sorted(hits)


def scan_file(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            raw = fh.read()
    except OSError as exc:
        print(f"WARNING: cannot read {path}: {exc}", file=sys.stderr)
        return []
    cleaned = strip_preprocessor(strip_comments_and_literals(raw))
    findings = []
    for start_line, stmt in statements(cleaned):
        if not SINK.search(stmt):
            continue
        hits = offending_identifiers(stmt)
        if hits:
            compact = " ".join(stmt.split())
            if len(compact) > 160:
                compact = compact[:157] + "..."
            findings.append((start_line, hits, compact))
    return findings


# --- the gate's own tests ----------------------------------------------------
# A gate that silently stops detecting is worse than no gate. The first cut of
# this script counted braces as nesting depth, so no statement inside a function
# body ever terminated, every file came back as one blob and the line numbers
# were all 1 -- it still "passed"/"failed" plausibly. These fixtures would have
# caught that in a second: #4 and #5 only report at the right line if statements
# are split properly.
MUST_FLAG = [
    ("a token in a debug fprintf", 'fprintf(stderr, "token=%s\\n", access_token);'),
    ("a password in an exception", 'throw InvalidInputException("bad pwd %s", password);'),
    ("a client secret in a Format", 'auto s = StringUtil::Format("%s", client_secret);'),
    (
        "a credential on a later line of a wrapped call",
        'throw IOException(\n    "login failed for %s with %s",\n    user,\n    raw_password);',
    ),
    ("a keytab path in a log", 'DUCKDB_LOG_WARNING(context, "keytab %s", krb5_keytabfile);'),
    ("a connection string in an exception", 'throw BinderException("dsn %s", connection_string);'),
]

MUST_NOT_FLAG = [
    ("the NAME of a secret", 'throw BinderException("Secret \'%s\' not found", secret_name);'),
    (
        "a secret's TYPE via a safe accessor",
        'throw BinderException("%s is not %s", secret_name, secret->GetType());',
    ),
    (
        "the word password only inside the message text",
        'fprintf(stderr, "password is required for this authenticator\\n");',
    ),
    ("a secret KEY-NAME constant", 'throw InvalidInputException("missing %s", MSSQL_SECRET_PASSWORD);'),
    ("a LOGIN7 field length", 'fprintf(stderr, "pwd field %u bytes\\n", password_length);'),
    ("a type name", 'auto &kv = static_cast<const KeyValueSecret &>(*secret);'),
    (
        "a credential NOT passed to a sink",
        'const std::string password = input.options.at("password");',
    ),
]


def self_test():
    """Run the detector over fixtures. Returns the number of failures."""
    failures = 0

    def findings_for(src):
        cleaned = strip_preprocessor(strip_comments_and_literals(src))
        out = []
        for line, stmt in statements(cleaned):
            if SINK.search(stmt):
                hits = offending_identifiers(stmt)
                if hits:
                    out.append((line, hits))
        return out

    print("must FLAG:")
    for label, src in MUST_FLAG:
        got = findings_for(src)
        ok = bool(got)
        failures += 0 if ok else 1
        detail = ", ".join(sorted({h for _, hs in got for h in hs})) if got else "NOTHING"
        print(f"  [{'ok' if ok else 'FAIL'}] {label:52s} -> {detail}")

    print("\nmust NOT flag:")
    for label, src in MUST_NOT_FLAG:
        got = findings_for(src)
        ok = not got
        failures += 0 if ok else 1
        detail = "clean" if ok else ", ".join(sorted({h for _, hs in got for h in hs}))
        print(f"  [{'ok' if ok else 'FAIL'}] {label:52s} -> {detail}")

    # Statement splitting: the multi-line fixture's credential sits on line 4,
    # and the finding must be anchored at the statement's first line (1), not
    # at every line and not at a merged blob.
    multi = findings_for(MUST_FLAG[3][1])
    if len(multi) != 1 or multi[0][0] != 1:
        failures += 1
        print(f"\n  [FAIL] wrapped-call statement split: expected one finding at line 1, got {multi}")
    else:
        print("\n  [ok] a wrapped call reports once, anchored at its first line")

    print(f"\n{'FAIL' if failures else 'PASS'}: self-test, {failures} failure(s)")
    return failures


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", default="src", help="directory to scan (default: src)")
    ap.add_argument(
        "--list-allowlisted",
        action="store_true",
        help="print the allowlist with justifications and exit",
    )
    ap.add_argument(
        "--self-test",
        action="store_true",
        help="check the detector against its own fixtures and exit",
    )
    args = ap.parse_args()

    if args.self_test:
        return 1 if self_test() else 0

    if args.list_allowlisted:
        print(f"{len(ALLOWLIST)} allowlisted identifiers:\n")
        for ident, why in sorted(ALLOWLIST.items()):
            print(f"  {ident:34s} {why}")
        return 0

    if not os.path.isdir(args.root):
        print(f"ERROR: {args.root} is not a directory", file=sys.stderr)
        return 2

    total = 0
    files = 0
    for dirpath, dirnames, filenames in os.walk(args.root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for fn in sorted(filenames):
            if not fn.endswith((".cpp", ".hpp", ".h", ".cc")):
                continue
            path = os.path.join(dirpath, fn)
            files += 1
            for line, hits, stmt in scan_file(path):
                total += 1
                print(f"{path}:{line}: credential-looking {', '.join(hits)} in a message sink")
                print(f"    {stmt}")

    print(f"\nscanned {files} file(s) under {args.root}/")
    if total:
        print(
            f"FAIL: {total} statement(s) pass a credential-looking value to a log,\n"
            f"      exception or format call.\n\n"
            f"If one is safe, add the identifier to ALLOWLIST in this script WITH\n"
            f"the reason -- that is the review record. Do not widen SECRETISH."
        )
        return 1
    print("PASS: no credential-looking value reaches a message sink.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
