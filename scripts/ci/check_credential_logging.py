#!/usr/bin/env python3
"""Gate: no credential value reaches a log, exception or format call.

WHY A GATE AND NOT A QUERY
--------------------------
Joern's q6 ("credential inside a log, exception or format expression") already
encodes this check. The problem is WHEN it runs: it needs Joern installed and a
CPG built, minutes of it, so it lives in `make joern-scan` as an exploratory
tool that runs when someone remembers. The one deterministic check in the tree
-- test/sql/tds_connection/pool_stats_no_credentials.test, spec 047 SC-005 --
covers a single function.

Credential disclosure is a REGRESSION class, not a discovery class: issue #492
(mssql_azure_auth_test showed a token's last 3 characters, and a short token in
full) is exactly what comes back when a new error message is written next to a
credential. So this is a grep-shaped gate: no dependencies, ~1 second, and it
cannot rot.

It is the STRUCTURAL form of q6 and makes the same trade. The dataflow form was
tried in Joern and is unusable here -- it taints a call's result with its
arguments, so every debug fprintf downstream of DoLogin7 reported as reached by
`password` (28 flows, all false).

WHAT IT FLAGS
-------------
A statement that calls a message sink and mentions something that looks like it
HOLDS a credential.

WHAT IT CANNOT SEE, STATED UP FRONT
-----------------------------------
This is a structural check over single statements. It does NOT follow values, so
a credential copied into an innocently named local first --

    auto s = info.password;
    fprintf(stderr, "%s", s);          // NOT flagged

-- is out of reach, and so is anything assembled across statements that does not
mention a credential-looking name at the sink. That is an accepted limit, not an
oversight: following values is what q6's dataflow form tried and what made it
unusable here. The gate's job is to catch the direct, obvious regression.

It also cannot see through a value stashed on an object (`last_error_ = pw;`
then `throw IOException(last_error_)` elsewhere) beyond flagging the assignment
itself, which it does treat as a sink.

EXIT CODES
----------
  0  clean
  1  at least one unsuppressed hit, or nothing was scanned at all
  2  bad invocation

Run: scripts/ci/check_credential_logging.py [--root src] [--self-test]
"""

import argparse
import os
import re
import sys

# ---------------------------------------------------------------------------
# Sinks: anything whose argument becomes human-readable text.
#
# The first cut listed `MSSQL_LOG[A-Z_]*` and `Log(Debug|Info|...)`, which match
# EXACTLY NOTHING in this tree -- measured. The project's loggers are macros
# named by subsystem, and there are 34 of them with hundreds of call sites:
#
#   MSSQL_DEBUG (89)  MSSQL_FILTER_DEBUG_LOG (75)  MSSQL_CONN_DEBUG_LOG (62)
#   MSSQL_SCAN_DEBUG_LOG (55)  MSSQL_SOCKET_DEBUG_LOG (53)
#   MSSQL_TLS_DEBUG_LOG (40)   ... INSERT_DEBUG, BULK_LOAD_LOG, PLP_DEBUG, ...
#
# MSSQL_CONN_DEBUG_LOG sits next to DoLogin7, which is the single most
# credential-adjacent code in the repo, and the gate could not see it.
#
# So the rule is GENERIC rather than a list: any UPPER_SNAKE call whose name
# ends in _LOG or _DEBUG. That covers all 34 and any subsystem logger added
# later without this file needing an edit.
# ---------------------------------------------------------------------------
SINK = re.compile(
    r"(?:"
    r"\b[A-Z][A-Z0-9_]*(?:_LOG|_DEBUG)\s*\("            # the project's loggers
    r"|\b[A-Za-z_][A-Za-z0-9_]*Exception\s*\("          # InvalidInputException, ...
    r"|\bstd::[a-z_]*error\s*\("                        # std::runtime_error (44 uses)
    r"|\b[A-Za-z_][A-Za-z0-9_]*Error\s*\("              # q6's `.*Error`
    r"|\bv?[fs]?n?printf\s*\("                          # printf/fprintf/snprintf/vfprintf
    r"|\bf?puts\s*\("
    r"|\bStringUtil::Format\s*\("
    r"|\bDUCKDB_LOG[A-Z_]*\s*\("
    r"|\bstd::(?:cerr|cout|clog)\s*<<"                  # azure_device_code.cpp uses cerr
    r"|\b(?:os|oss|ss|msg|out|stream|buf)\s*<<"         # ostringstream accumulation
    r"|\b[A-Za-z_][A-Za-z0-9_]*(?:error|message|msg|detail|reason)_?\s*(?:=|\+=)"
    r")",
    re.IGNORECASE if False else 0,
)

# ---------------------------------------------------------------------------
# What looks like a credential VALUE.
#
# Unconditional words. `dsn`, `conn_str`, `sas` and `auth_header` were missing
# from the first cut although q6 carries `dsn`.
# ---------------------------------------------------------------------------
SECRETISH = re.compile(
    r"\b[A-Za-z0-9_]*("
    r"password|passwd|pwd"
    r"|secret"
    r"|credential"
    r"|keytab"
    r"|connection_string|conn_str|connstr"
    r"|auth_header|authorization"
    r"|bearer"
    r"|\bsas\b|sas_token|sas_key"
    r"|\bdsn\b"
    r")[A-Za-z0-9_]*\b",
    re.IGNORECASE,
)

# ---------------------------------------------------------------------------
# `token` needs its own rule, and this is the one judgement call in the file.
#
# Issue #492 WAS a token disclosure, so leaving tokens out would miss the gate's
# own motivating bug. But `token` in this codebase overwhelmingly means a TDS
# PROTOCOL token: 465 bare uses, plus token_length (34), tds_type_token (25),
# token_type (22), token_end (20), token_result (19), done_token (12),
# token_packets (12), tokens (78)... Matching bare `token` would fire on
# hundreds of correct lines, and a gate that cries wolf gets switched off --
# which is a worse outcome than the gate not existing.
#
# So: an INCLUSION list of the qualifiers that mean "credential". Measured in
# src/: access_token (86), fedauth_token (15), azure_token (10),
# output_token (10, the SSPI/GSSAPI blob).
#
# COST, STATED: a NEW credential-bearing token name has to be added here or it
# is missed. That is the deliberate trade against ~465 false positives. The
# self-test pins both directions -- fedauth_token flags, token_length does not.
# ---------------------------------------------------------------------------
CREDENTIAL_TOKEN = re.compile(
    r"\b[A-Za-z0-9_]*("
    r"access_token|refresh_token|id_token|bearer_token|auth_token|oauth_token"
    r"|fedauth_token|azure_token|sspi_token|output_token|sas_token|jwt"
    r"|accesstoken|authtoken"
    r")[A-Za-z0-9_]*\b",
    re.IGNORECASE,
)


def is_credential_name(ident):
    return bool(SECRETISH.search(ident)) or bool(CREDENTIAL_TOKEN.search(ident))


# The NAME of a credential, or WHERE IT LIVES -- not the credential. q6's
# `identifierOfSecret`, extended two ways after running this over src/:
#
#   * a trailing underscore, because that is this codebase's member convention
#     (`secret_name_` is as much a name as `secret_name`);
#   * path/file/dir suffixes. `config_.keytabfile` is a FILE PATH, and
#     krb5_authenticator.cpp:166 names it in "keytab file 'X' is not readable.
#     Check the path and file permissions." -- which is precisely what the user
#     needs. A credential's location is not the credential, by the same logic
#     that its name is not.
#
# THAT SECOND ONE IS A JUDGEMENT CALL worth challenging in review. Taking it
# means `krb5_keytabfile`, `krb5_credcachefile` and `krb5_configfile` are out of
# scope for this gate even though the FILES they name hold key material. The
# argument for it: the gate is about credential VALUES, a path is not one, and
# the alternative is a per-line suppression on a diagnostic that cannot do its
# job without naming the path. The argument against: a path can embed a service
# account name. If that trade is rejected, drop the path suffixes here and
# suppress krb5_authenticator.cpp:166 instead -- the self-test pins both
# directions, so the fixtures will say immediately which way the file is set.
# Keytab MATERIAL (`keytab_data`, `keytab_bytes`) still flags either way.
NAME_OF_SECRET = re.compile(
    r"^[A-Za-z0-9_]*("
    r"_name|_type|_id|_key_name|_kind|name|type|kind"
    r"|_file|file|_path|path|_dir|dir|_offset|_length|_len|_size|_count"
    r")_?$",
    re.IGNORECASE,
)

# An UPPER_SNAKE identifier is a compile-time constant (CLAUDE.md's naming rule)
# and cannot hold a runtime credential. The ones that trip the vocabulary are the
# secret KEY NAMES (MSSQL_SECRET_PASSWORD) and protocol constants
# (FEDAUTH_ADAL_WORKFLOW_PASSWORD, GSS_C_NO_CREDENTIAL).
CONSTANT = re.compile(r"^[A-Z][A-Z0-9_]*$")

# A PascalCase ACCESSOR that returns a credential must report -- the first cut's
# TYPE_NAME rule excused GetPassword / GetSecret / GetCredential, contradicting
# this file's own comment. Only Get/Fetch/Take/Reveal/Expose count as accessor
# verbs; Read/Create/Validate/Acquire are plumbing in this codebase
# (ReadAzureSecret returns a struct, ValidateAzureSecretExists returns a bool).
CREDENTIAL_ACCESSOR = re.compile(
    r"^(Get|Fetch|Take|Reveal|Expose)[A-Za-z0-9_]*"
    r"(Password|Passwd|Pwd|Secret|Token|Credential|Keytab)[A-Za-z0-9_]*$"
)

PLUMBING_CALL = re.compile(
    r"^(Create|Read|Validate|Acquire|Free|Delete|Register|Lookup|Encode|Decode|Obfuscate)"
    r"[A-Za-z0-9_]*$"
)
# KeyValueSecret, SecretManager, AzureSecretInfo, Keytab -- a type, not a value.
TYPE_NAME = re.compile(r"^(?:[A-Z][a-z0-9]+)+$")

# A credential read BY KEY. `kv_secret.TryGetValue("password")` is how
# mssql_storage.cpp:207 actually reads the password, and the first cut could not
# see it: the literal was stripped, the UPPER_SNAKE form was excused as a
# constant, and `kv_secret` itself was allowlisted. The literal only counts in a
# LOOKUP position -- otherwise every `throw ...("Secret '%s' not found", ...)`
# would match its own message text.
KEY_READ = re.compile(
    r"(?:TryGetValue|TryGetSecretValue|GetValue|\.at|\[)\s*\(?\s*"
    r"\"[^\"]*(?:password|passwd|pwd|secret|keytab|credential"
    r"|access_token|refresh_token|fedauth_token|azure_token)[^\"]*\"",
    re.IGNORECASE,
)

# Per-line escape hatch, so a false positive does not need a GLOBAL allowlist
# entry that then excuses the name everywhere in the tree.
SUPPRESS = re.compile(r"//\s*credential-log-ok\b\s*:?\s*(?P<reason>.*)")

# Identifiers that merely CONTAIN a credential word but are an object, a
# function, a count or a protocol field. Pruned from the first cut's 18: the
# entries that only ever appeared in comments, or nowhere at all, are gone, and
# `kv_secret` is NO LONGER excused -- it is the receiver of the key reads above.
# EMPTY ON PURPOSE. The first cut shipped 18 entries; the review of PR #498
# pointed out that several excused names appearing only in comments or nowhere
# at all. So each candidate was tested by REMOVING it and re-scanning src/: all
# of them turned out to change nothing, because the structural rules above now
# cover every case they were written for --
#
#   secret_manager, secret_entry, azure_secret_reader  -> TYPE_NAME / plumbing
#   obfuscate_password, get_init_creds_password        -> PLUMBING_CALL
#   has_password, has_access_token, has_azure_secret   -> never reach a sink
#   password_offset, sspi_attrdb_chgpwd_offset         -> NAME_OF_SECRET's _offset
#
# An allowlist with no necessary entries is pure liability: it cannot help, and
# it silently excuses a name EVERYWHERE the day someone writes a real leak with
# it. The mechanism stays for a name that is genuinely safe tree-wide, but the
# per-line marker is the preferred escape hatch because it carries its reason to
# the place that needs it:
#
#   fprintf(...);  // credential-log-ok: <why this cannot disclose>
ALLOWLIST = {}

SKIP_DIRS = {"build", "duckdb", "vcpkg", ".git", "third_party"}
# .inl / .ipp / .cxx / .hh were missing, so a root holding only those passed
# vacuously.
SOURCE_SUFFIXES = (".cpp", ".cc", ".cxx", ".c", ".hpp", ".hh", ".hxx", ".h", ".inl", ".ipp")

IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")

# An accessor that yields something ABOUT a credential rather than the
# credential. Deleted before identifiers are collected, so a credential-looking
# receiver used ONLY this way disappears -- while the same receiver passed
# anywhere else in the same statement still reports.
#
#   GetType()/GetName()  "mssql" / "azure"; what the "Secret 'x' is not of type
#                        'y'" errors are for.
#   size()/length()      a LENGTH. tds_connection.cpp:484 and :671 log
#                        `token_size=%zu, fedauth_token.size()`, which discloses
#                        nothing. Also clears `password.size()` and
#                        `password.empty()`, both of which the review of #498
#                        listed as false positives needing a global allowlist
#                        entry -- they need neither now.
SAFE_ACCESSOR = re.compile(
    r"[A-Za-z_][A-Za-z0-9_]*\s*(?:->|\.)\s*"
    r"(?:Get(?:Type|Name|Size|Length)|size|length|empty|capacity)\s*\(\s*\)"
)


def strip_comments_and_literals(text):
    """Blank comments and string/char literals, preserving line structure.

    Literals go because a message's TEXT discloses nothing -- only an
    interpolated value does. Handles two lexer traps the first cut got wrong:

      * RAW strings. `R"(... " ...)"` holds an odd number of quotes, which flips
        the in-literal state for the rest of the file.
      * DIGIT SEPARATORS. `1'000` is not the start of a char literal.
    """
    out = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        # Raw string: R"delim( ... )delim", optionally prefixed L/u8/u/U.
        if c in "Rr" and i + 1 < n and text[i + 1] == '"':
            m = re.match(r'R"([^(\s\\]{0,16})\(', text[i:])
            if m:
                delim = m.group(1)
                close = ')' + delim + '"'
                end = text.find(close, i + m.end())
                body = text[i:end + len(close)] if end != -1 else text[i:]
                out.append("".join("\n" if ch == "\n" else " " for ch in body))
                i = (end + len(close)) if end != -1 else n
                continue
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                i += 1
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            i = min(i + 2, n)
        elif c == "'" and i > 0 and text[i - 1].isalnum() and i + 1 < n and text[i + 1].isalnum():
            # Digit separator (1'000, 0xFF'FF), not a char literal.
            out.append(" ")
            i += 1
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
    """Blank preprocessor directives, but KEEP `#define` bodies.

    Directives carry no `;`, so leaving them whole made every file one
    statement. But blanking a `#define` outright hid the macro BODIES -- which
    is where the project's loggers call fprintf -- so a leak written inside a
    macro was invisible. Only the `#define NAME(args)` head is removed; the body
    is scanned like any other code.
    """
    out = []
    in_directive = False
    for line in text.split("\n"):
        stripped = line.lstrip()
        if not in_directive and stripped.startswith("#"):
            m = re.match(r"(\s*#\s*define\s+[A-Za-z_][A-Za-z0-9_]*(?:\([^)]*\))?)(.*)$", line)
            if m:
                out.append(" " * len(m.group(1)) + m.group(2))
            else:
                out.append("")
            in_directive = True
        elif in_directive:
            out.append(line)
        else:
            out.append(line)
        if in_directive:
            in_directive = line.rstrip().endswith("\\")
    return "\n".join(out)


def statements(text):
    """Yield (start_line, statement_text).

    Counting braces as nesting depth was the first cut's bug: a function body
    kept depth >= 1, so no `;` inside one ever closed a statement and each file
    came back as a single blob with every finding reported at line 1.

    A plain paren-DEPTH counter is not enough either, as running this over src/
    showed: `const bool ok = RunWithRoutingHops([&]() { ...140 lines... });`
    keeps paren depth >= 1 for the whole lambda, so no `;` inside the body split
    and the entire lambda came back as one statement -- which false-positived
    tds_connection.cpp:823, pairing a sink in one part of the body with an
    `sspi_token` in another. The brace bug's cousin.

    So the state is a STACK of contexts. A `;` ends a statement when the
    innermost context is a brace block (a lambda body, a function body) or
    nothing at all -- but not inside parens, where `for (a; b; c)` lives.
    """
    buf = []
    start_line = 1
    line = 1
    stack = []
    for ch in text:
        if ch == "\n":
            line += 1
        if not buf or "".join(buf).isspace():
            start_line = line
        buf.append(ch)
        if ch in "([":
            stack.append("paren")
        elif ch == "{":
            stack.append("brace")
        elif ch in ")]":
            if stack and stack[-1] == "paren":
                stack.pop()
        elif ch == "}":
            if stack and stack[-1] == "brace":
                stack.pop()
        in_paren = bool(stack) and stack[-1] == "paren"
        if not in_paren and ch in ";{}":
            yield start_line, "".join(buf)
            buf = []
            start_line = line
    if buf and not "".join(buf).isspace():
        yield start_line, "".join(buf)


def offending(stmt_code, stmt_raw):
    """What in this statement looks like a credential reaching a sink.

    `stmt_code` has comments and literals blanked; `stmt_raw` is the original,
    needed for the key-read check (whose signal IS a literal).
    """
    if not SINK.search(stmt_code):
        return []
    hits = []
    if KEY_READ.search(stmt_raw):
        hits.append('a credential read by key (e.g. TryGetValue("password"))')
    cleaned = SAFE_ACCESSOR.sub(" ", stmt_code)
    for ident in sorted(set(IDENT.findall(cleaned))):
        if not is_credential_name(ident):
            continue
        if ident in ALLOWLIST or ident.lower() in ALLOWLIST:
            continue
        if NAME_OF_SECRET.match(ident):
            continue
        if CONSTANT.match(ident):
            continue
        if CREDENTIAL_ACCESSOR.match(ident):
            hits.append(ident)
            continue
        if TYPE_NAME.match(ident) or PLUMBING_CALL.match(ident):
            continue
        hits.append(ident)
    return hits


def scan_text(raw):
    """[(line, hits, statement)] for one translation unit's text."""
    code = strip_preprocessor(strip_comments_and_literals(raw))
    raw_lines = raw.split("\n")
    findings = []
    for start_line, stmt in statements(code):
        n_lines = stmt.count("\n")
        span = raw_lines[start_line - 1: start_line + n_lines]
        if any(SUPPRESS.search(l) for l in span):
            continue
        stmt_raw = "\n".join(span)
        hits = offending(stmt, stmt_raw)
        if hits:
            compact = " ".join(stmt.split())
            if len(compact) > 160:
                compact = compact[:157] + "..."
            findings.append((start_line, hits, compact))
    return findings


def scan_file(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            return scan_text(fh.read())
    except OSError as exc:
        print(f"WARNING: cannot read {path}: {exc}", file=sys.stderr)
        return []


# ===========================================================================
# The gate's own tests.
#
# A gate that silently stops detecting is worse than no gate. The first cut
# counted braces as nesting depth, so no statement inside a function body ever
# terminated -- and its 13 fixtures ALL still passed with the bug restored,
# because every fixture was a single statement with no braces. The self-test
# tested nothing it was built for.
#
# Hence SPLITTER_TESTS below: fixtures that can only pass when statements are
# genuinely split, including the exact failure the bug produced.
# ===========================================================================
MUST_FLAG = [
    # VGSML's five planted leaks (review of PR #498). The first cut caught
    # only the first.
    ("plain fprintf", 'fprintf(stderr, "pw=%s", password.c_str());'),
    ("the project's own logger macro",
     'MSSQL_CONN_DEBUG_LOG(1, "pw=%s", info.password.c_str());'),
    ("a fedauth token (no `token` in the old word list)",
     'fprintf(stderr, "%s", fedauth_token.c_str());'),
    ("a PascalCase credential getter",
     'throw IOException("x" + info.GetPassword());'),
    ("std::runtime_error as a sink",
     'throw std::runtime_error("bad: " + password);'),
    # The rest of the vocabulary and sink surface the review listed.
    ("a client secret in a Format", 'auto s = StringUtil::Format("%s", client_secret);'),
    ("a credential on a later line of a wrapped call",
     'throw IOException(\n    "login failed for %s with %s",\n    user,\n    raw_password);'),
    # Keytab MATERIAL, not the keytab's path -- see the note on NAME_OF_SECRET
    # for why a path is out of scope and must stay consistent with the
    # krb5_authenticator.cpp:166 fixture below.
    ("keytab material in a log", 'DUCKDB_LOG_WARNING(context, "keytab %s", keytab_data);'),
    ("a dsn in an exception", 'throw IOException("d %s", dsn);'),
    ("an access token in a subsystem logger", 'MSSQL_TLS_DEBUG_LOG(2, "t=%s", access_token);'),
    ("a sas token", 'fprintf(stderr, "%s", sas_token);'),
    ("std::cerr streaming", 'std::cerr << "pw: " << pwd << std::endl;'),
    ("ostringstream accumulation", 'oss << "pw " << password;'),
    ("an error member assignment", 'last_error_ = "failed with " + password;'),
    ("vfprintf", 'vfprintf(stderr, fmt, password_args);'),
    ("a key read via TryGetValue", 'throw IOException(kv_secret.TryGetValue("password").ToString());'),
    ("a key read via options.at", 'fprintf(stderr, "%s", input.options.at("password").c_str());'),
    ("a bare _DEBUG macro", 'INSERT_DEBUG("pw=%s", password);'),
]

MUST_NOT_FLAG = [
    ("the NAME of a secret", 'throw BinderException("Secret \'%s\' not found", secret_name);'),
    ("a secret's TYPE via a safe accessor",
     'throw BinderException("%s is not %s", secret_name, secret->GetType());'),
    ("the word password only inside the message text",
     'fprintf(stderr, "password is required for this authenticator\\n");'),
    ("a secret KEY-NAME constant",
     'throw InvalidInputException("missing %s", MSSQL_SECRET_PASSWORD);'),
    ("a LOGIN7 field offset", 'fprintf(stderr, "off %u\\n", password_offset);'),
    ("a type name", 'auto &kv = static_cast<const KeyValueSecret &>(*secret);'),
    ("a credential NOT passed to a sink",
     'const std::string password = input.options.at("password");'),
    # The token rule's negative half: these are TDS PROTOCOL tokens, 465 bare
    # uses in src/, and flagging them would make the gate noise.
    ("a protocol token length", 'MSSQL_PROTO_DEBUG_LOG(3, "len=%u", token_length);'),
    ("a protocol token type", 'fprintf(stderr, "tok %u\\n", token_type);'),
    ("the TDS type token", 'throw IOException("bad type %u", col.tds_type_token);'),
    ("a DONE token", 'MSSQL_DEBUG_LOG("done %d", done_token);'),
    ("plumbing that returns a struct", 'auto info = ReadAzureSecret(context, name);'),
    ("a per-line suppression",
     'fprintf(stderr, "%s", password);  // credential-log-ok: redacted upstream'),
    ("a lookup of a NON-credential key", 'fprintf(stderr, "%s", kv.TryGetValue("host").ToString());'),
    # The four false positives the first run against src/ produced. Each is
    # cleared by a structural rule, not a suppression -- which is the bar: a
    # suppression should be rare enough to be worth reading.
    ("a token's LENGTH (tds_connection.cpp:484)",
     'MSSQL_CONN_DEBUG_LOG(1, "token_size=%zu", fedauth_token.size());'),
    ("password.size() (review of #498 listed this as a false positive)",
     'fprintf(stderr, "len %zu\\n", password.size());'),
    ("password.empty() guarding a message that names no value",
     'if (password.empty()) throw InvalidInputException("password is required");'),
    ("a keytab FILE PATH (krb5_authenticator.cpp:166)",
     'throw std::runtime_error("keytab file \'" + config_.keytabfile + "\' is not readable.");'),
    ("a secret_name_ MEMBER (trailing underscore)",
     'throw BinderException("Secret \'%s\' not found", secret_name_);'),
]

# Fixtures that only behave correctly when statements are genuinely SPLIT.
# Restoring the brace-depth bug (or removing splitting altogether) merges the
# statements inside each function body and breaks these -- which is the point.
SPLITTER_TESTS = [
    (
        "a credential and a sink in DIFFERENT statements of one function body",
        'void f() {\n\tauto p = password;\n\tfprintf(stderr, "x");\n}',
        None,  # must not flag at all
    ),
    (
        "a leak on line 3 of a function body reports line 3",
        'void f() {\n\tint a = 1;\n\tfprintf(stderr, "%s", password);\n\ta++;\n}',
        3,
    ),
    (
        "two statements, only the second leaks, reported at its own line",
        'void g() {\n\tdo_setup();\n\tdo_more();\n\tthrow IOException(password);\n}',
        4,
    ),
    # A LAMBDA BODY inside a call's parens. tds_connection.cpp:823 is
    # `RunWithRoutingHops([&]() { ...140 lines... });` and a paren-depth counter
    # never splits it, so a sink in one part of the body paired with a
    # credential in another -- which is what false-positived that line.
    (
        "a sink and an unrelated credential in different statements of a lambda body",
        'auto ok = Run([&]() {\n\tauto t = sspi_token;\n\tMSSQL_CONN_DEBUG_LOG(1, "hop");\n\treturn t;\n});',
        None,
    ),
    (
        "a REAL leak inside a lambda body is still caught, at its own line",
        'auto ok = Run([&]() {\n\tint a = 1;\n\tfprintf(stderr, "%s", password);\n\treturn a;\n});',
        3,
    ),
]


def findings_for(src):
    return scan_text(src)


def self_test():
    failures = 0

    print("must FLAG:")
    for label, src in MUST_FLAG:
        got = findings_for(src)
        ok = bool(got)
        failures += 0 if ok else 1
        detail = ", ".join(sorted({h for _, hs, _ in got for h in hs})) if got else "NOTHING"
        print(f"  [{'ok' if ok else 'FAIL'}] {label:52s} -> {detail}")

    print("\nmust NOT flag:")
    for label, src in MUST_NOT_FLAG:
        got = findings_for(src)
        ok = not got
        failures += 0 if ok else 1
        detail = "clean" if ok else ", ".join(sorted({h for _, hs, _ in got for h in hs}))
        print(f"  [{'ok' if ok else 'FAIL'}] {label:52s} -> {detail}")

    print("\nstatement splitting (these FAIL if the splitter breaks):")
    for label, src, want_line in SPLITTER_TESTS:
        got = findings_for(src)
        if want_line is None:
            ok = not got
            detail = "clean" if ok else f"flagged {got[0][1]} at line {got[0][0]}"
        else:
            ok = len(got) == 1 and got[0][0] == want_line
            detail = f"line {got[0][0]}" if got else "NOTHING"
            if len(got) > 1:
                detail = f"{len(got)} findings"
        failures += 0 if ok else 1
        print(f"  [{'ok' if ok else 'FAIL'}] {label:62s} -> {detail}")

    # The lexer traps that silently disabled the rest of a file.
    print("\nlexer:")
    for label, src, should_flag in [
        ("a raw string holding an odd quote does not flip the lexer",
         'auto q = R"(he said " hi)";\nfprintf(stderr, "%s", password);', True),
        ("a digit separator is not a char literal",
         'int n = 1\'000;\nfprintf(stderr, "%s", password);', True),
    ]:
        got = findings_for(src)
        ok = bool(got) == should_flag
        failures += 0 if ok else 1
        print(f"  [{'ok' if ok else 'FAIL'}] {label:62s} -> {'flagged' if got else 'clean'}")

    print(f"\n{'FAIL' if failures else 'PASS'}: self-test, {failures} failure(s)")
    return failures


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", default="src", help="directory to scan (default: src)")
    ap.add_argument("--list-allowlisted", action="store_true",
                    help="print the allowlist with justifications and exit")
    ap.add_argument("--self-test", action="store_true",
                    help="check the detector against its own fixtures and exit")
    args = ap.parse_args()

    if args.self_test:
        return 1 if self_test() else 0

    if args.list_allowlisted:
        print(f"{len(ALLOWLIST)} allowlisted identifiers:\n")
        for ident, why in sorted(ALLOWLIST.items()):
            print(f"  {ident:34s} {why}")
        print("\nPer-line alternative, preferred for a one-off:")
        print("  fprintf(...);  // credential-log-ok: <reason>")
        return 0

    if not os.path.isdir(args.root):
        print(f"ERROR: {args.root} is not a directory", file=sys.stderr)
        return 2

    total = 0
    files = 0
    for dirpath, dirnames, filenames in os.walk(args.root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        for fn in sorted(filenames):
            if not fn.endswith(SOURCE_SUFFIXES):
                continue
            path = os.path.join(dirpath, fn)
            files += 1
            for line, hits, stmt in scan_file(path):
                total += 1
                print(f"{path}:{line}: credential-looking {', '.join(hits)} in a message sink")
                print(f"    {stmt}")

    # A root with no source files used to exit 0 with PASS, which is the worst
    # kind of green: it looks like coverage.
    if files == 0:
        print(f"\nFAIL: no source files found under {args.root}/ — nothing was checked.")
        return 1

    print(f"\nscanned {files} file(s) under {args.root}/")
    if total:
        print(
            f"FAIL: {total} statement(s) pass a credential-looking value to a log,\n"
            f"      exception or format call.\n\n"
            f"If one is safe, prefer a per-line marker carrying the reason:\n"
            f"      // credential-log-ok: <why this cannot disclose>\n"
            f"Use ALLOWLIST in this script only for a name that is safe EVERYWHERE.\n"
            f"Do not widen the credential vocabulary to silence a finding."
        )
        return 1
    print("PASS: no credential-looking value reaches a message sink.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
