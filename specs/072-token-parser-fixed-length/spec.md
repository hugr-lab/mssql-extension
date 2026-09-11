# Spec 072 — The token-skip group that assumed every token carries a length

Issue [#323](https://github.com/hugr-lab/mssql-extension/issues/323): calling a stored procedure desyncs the TDS parser. Filed as one bug in one token. The reconnaissance below found it is one *group* — six token types handled by one arm on the assumption that each is `type(1) + length(2) + data` — in which two tokens do not fit the assumption, one is registered under the wrong byte, and the two consumers of the parser fail in opposite ways when it happens: the scan path loudly, the `mssql_exec` path **silently, reporting success**.

Written as reconnaissance first; the implementation landed in the same PR, with every finding below verified against the parser as it was.

## 0. How everything below was measured

- SQL Server 2025 (container `mssql-v2test`, `TestDB`), extension at `main` `5fadf8a`, DuckDB `v2.0-cyanoptera` pin `070ce1f6`.
- `MSSQL_DEBUG=1` for the parser's unknown-token dump, which prints the position, the bytes remaining, and the next 32 bytes of the buffer. Every byte quoted below is from that dump, not from the specification.
- Four procedure shapes, each followed by a second statement on the **same** connection (`SET mssql_connection_limit = 1`, `mssql_pool_stats` before and after):
  - `I323P` — `SET NOCOUNT ON; INSERT … 3 rows` (the issue's shape)
  - `I323R` — `SET NOCOUNT ON; RETURN 7` (non-zero status)
  - `I323S` — `SET NOCOUNT ON; SELECT …` (through `mssql_scan`)
  - `I323O @x INT OUTPUT` — an OUTPUT parameter over `SQL_BATCH`
- Plus `SELECT … FOR BROWSE` for the token neighbours in the same group, and `EXEC I323R; RAISERROR(…, 16, 1)` for what the desync does to an error that follows it.

## 1. The findings

### F1 — RETURNSTATUS is fixed-length, and the skip reads its value as a length

`src/tds/tds_token_parser.cpp:155-172` handles six tokens with one arm:

```cpp
case TokenType::ORDER:
case TokenType::RETURNSTATUS:
case TokenType::RETURNVALUE:
case TokenType::LOGINACK:
case TokenType::TABNAME:
case TokenType::COLINFO:
    // Skip these tokens - they have a 2-byte length
```

[MS-TDS] 2.2.7.16 RETURNSTATUS is `TokenType(1) + Value(LONG, 4)`. Five bytes, no length field. The arm reads the low two bytes of the *value* as a length and consumes `3 + that`.

Measured, `EXEC dbo.I323R` (`RETURN 7`), whole response 18 bytes:

```text
79 07 00 00 00                              RETURNSTATUS, value 7
fe 00 00 e0 00  00 00 00 00 00 00 00 00     DONEPROC, status 0, curcmd 0xE0, rowcount 0
```

The arm reads `07 00` = 7, consumes 10 bytes — the five of RETURNSTATUS **and the first five of DONEPROC** — and lands on the rowcount:

```text
[TDS PARSER] Unknown token 0x00 at pos=10, buffer_size=18, available=8,
             hex: 00 00 00 00 00 00 00 00
```

With a zero status (`I323P`) the arithmetic is different and the outcome the same: `00 00` = 0, consume 3, two bytes of value left over, then DONEPROC at +2:

```text
[TDS PARSER] Unknown token 0x00 at pos=9, buffer_size=24, available=15,
             hex: 00 00 fe 00 00 e0 00 00 00 00 00 00 00 00 00
```

Which is the dump in the issue, byte for byte. So the issue's diagnosis is right, and the non-zero case is worse than the one it describes: a return value of *n* eats *n* bytes of whatever token follows.

### F2 — The two consumers fail in opposite ways, and the exec path fails silently

Both `MSSQLResultStream` (scans) and `MSSQLSimpleQuery` (`mssql_exec`, and every catalog metadata query) drive the same `TokenParser`. On an unknown token the parser sets `state_ = ParserState::Error` and stops. What happens next differs:

**`mssql_scan`** — `mssql_result_stream.cpp:229` checks `parser_.GetState() == ParserState::Error` and throws `IO Error: TDS parse error: Unknown token type: 0x0`. The pool then logs `Closing connection in non-Idle state` and discards the connection. Loud, recoverable at the cost of a connection. This is the failure the issue reports.

**`mssql_exec`** — `mssql_simple_query.cpp` has **no such check**. Its loop is `while (TryParseNext() != NeedMoreData)`; a parser in the Error state answers `NeedMoreData` forever, so the loop exits, keeps reading packets to EOM (feeding a dead parser, which is what keeps the socket clean), and then:

```cpp
// If EOM was set and we're not done, there's no more data coming
if (is_eom && !done) {
    // Force done - the server has sent all data it's going to send
```

returns **success**. Measured:

| statement via `mssql_exec` | result |
| --- | --- |
| `EXEC dbo.I323P` (NOCOUNT ON) | `0`, no error, connection reused (pool: 1 idle before and after) |
| `EXEC dbo.I323N` (NOCOUNT OFF, 3-row INSERT) | `3` — correct, because DONEINPROC(3) arrives **before** RETURNSTATUS |
| `EXEC dbo.I323R; RAISERROR('after', 16, 1)` | **`0`, no error** |
| `RAISERROR('control', 16, 1)` alone | `SQL Server error 50000: control` |

The third row is the finding. Every token after RETURNSTATUS is dropped on this path: later result sets, later DONE row counts, and **a SQL Server ERROR token**, which is exactly the one a caller most needs. A batch of `EXEC p; <anything>` reports the outcome of `p` and nothing after it, with no indication.

This is why the bug survived: the common shape — one `EXEC`, NOCOUNT off — reports the right row count, because the count precedes the desync, and the desync itself is swallowed. The issue's "the connection is not recoverable" is the scan path; the exec path is recoverable and wrong, which is the worse property.

### F3 — TABNAME is registered under the wrong byte

`tds_types.hpp:104`: `TABNAME = 0x04`. [MS-TDS] 2.2.7.22 says 0xA4. Measured with `SELECT id FROM dbo.I323T FOR BROWSE`:

```text
[TDS PARSER] Unknown token 0xa4 at pos=22, …
             hex: a4 15 00  02  03 00 64 00 62 00 6f 00  05 00 49 00 33 00 32 00 33 00 54 00
                  a5 03 00 01 01 00  d1 04 …
```

Read against the spec: `a4`, length `0x0015`, part count 2, `dbo` (3 UCS-2 chars), `I323T` (5) — then `a5` COLINFO length 3, then `d1` ROW. So TABNAME *does* carry a 2-byte length and *does* belong in the group; it is only the enum byte that is wrong, and the arm is never reached. Any `FOR BROWSE` result fails with `Unknown token type: 0x164` (see F5 for the number), and the pool discards the connection.

### F4 — RETURNVALUE has no length field either, and cannot be skipped by one

[MS-TDS] 2.2.7.17 RETURNVALUE is `TokenType(1) + ParamOrdinal(USHORT) + ParamName(B_VARCHAR) + Status(BYTE) + UserType(ULONG) + Flags(USHORT) + TypeInfo + Value`. No length. The arm would read `ParamOrdinal` as one. Its size is only knowable by parsing `TypeInfo` — which the parser already does for COLMETADATA.

**Unreachable today**, and measured to be: `DECLARE @v INT; EXEC dbo.I323O @v OUTPUT; SELECT @v` produced no `0xAC` anywhere in the stream — the OUTPUT value stays server-side and comes back as an ordinary row (`d1 04 2a 00 00 00` = 42). RETURNVALUE is sent only in response to an **RPC** request (packet type 3), and this extension sends only `SQL_BATCH` (type 1). So the arm is wrong for it, and nothing exercises the arm.

### F5 — The unknown-token message prints decimal after a hex prefix

`tds_token_parser.cpp:236`:

```cpp
parse_error_ = "Unknown token type: 0x" + std::to_string(token_type);
```

`std::to_string(uint8_t)` is decimal. 0xA4 reports as `0x164`. The issue's `0x0` happens to read the same in both bases, which is why nobody tripped on it there.

### F6 — The group's assumption is not checkable by a rule

[MS-TDS] 2.2.4.1 encodes a length class in bits 5-4 of the token byte
(`11` fixed-length with the size in bits 3-2, `10` variable-length with a USHORT length, `00` variable-count). RETURNSTATUS = `0111 1001` → fixed, 4 bytes: the rule would have caught F1. But the rule has exceptions this parser already relies on: DONE/DONEPROC/DONEINPROC (`1111 11xx`) class-decode to 8 bytes and are 13 since TDS 7.2; RETURNVALUE (`1010 1100`) class-decodes to "USHORT length" and has none. Recorded so the next reader does not propose it as a self-check — see § 3.

## 2. The work

### W1 — RETURNSTATUS gets its own arm

Consume five bytes, unconditionally. Nothing reads the value today, so no field is added for it (a field nothing reads is the pattern spec 071 removed).

### W2 — TABNAME = 0xA4

One byte in the enum. The arm it then reaches is correct for it (F3).

### W3 — RETURNVALUE leaves the skip group

Route it to a `default`-style arm that fails **by name**: `RETURNVALUE (0xAC) is sent only for RPC requests, which this extension does not issue`. If it ever appears, a named error beats a silent mis-skip; when RPC is added, that arm is where TypeInfo parsing goes.

### W4 — The exec path stops swallowing a parser error

`MSSQLSimpleQuery` gets the check the stream has: `ParserState::Error` → throw `IOException("TDS parse error: …")`, not "force done". This is a **behaviour change** — a desync on the exec path becomes an error instead of a success — and it is the point: with W1 in place nothing should trip it, and if something does, F2 is what silence costs.

### W5 — Hex in the message

`StringUtil::Format("Unknown token type: 0x%02X", token_type)`.

### Tests, each verified failing on `main` first

- **T1, server-free, gates a PR.** `test/cpp/test_token_parser_tokens.cpp` in `STANDALONE_TEST_SOURCES`: feed the two captured streams (`79 07 00 00 00 fe …` and `79 00 00 00 00 fe …`), assert the parser yields `Done` with status 0, curcmd 0xE0, rowcount 0 and never enters Error; feed the FOR BROWSE capture, assert TABNAME/COLINFO are skipped and the ROW parses; assert the unknown-token message for byte 0xA4 contains `0xA4`.
- **T2, fuzz seed.** `fuzz/corpus/tds_tokens/returnstatus_doneproc`, with the leading `0x00` mode byte the README requires. Without W1 this seed is not a crash — it is a wrong parse, which the fuzzer cannot see — so T1 is the gate and T2 is coverage for the arithmetic under mutation.
- **T3, live.** `test/sql/regression/issue_323.test` (the #298 convention): the four procedure shapes, each followed by a statement on the same connection; `FOR BROWSE`; and `EXEC p; RAISERROR(…)` asserted as an **error** — the F2 case, which is the one that will not regress quietly.

## 3. What this spec does not propose

- **Parsing RETURNVALUE.** Needs RPC to be reachable, and RPC is not on the roadmap. W3 makes its arrival an error with a name.
- **A token-length-class self-check** from [MS-TDS] 2.2.4.1. Tempting — it would have flagged F1 — but with two exceptions in a twenty-entry table it is a trap dressed as an invariant (F6).
- **Reading the return status into a field.** No consumer. `mssql_exec` returns a row count; a `mssql_exec_status()` is a feature, not this fix.

## 4. Risks

- **W4 turns silent into loud.** Any *other* latent desync on the exec path — a token this survey did not exercise — becomes a visible error where it was a success with dropped tokens. Intended; goes in the CHANGELOG as such.
- **Multi-packet responses.** Today the exec path's drain-to-EOM on a dead parser is what keeps the socket clean for the next statement. W4 must keep draining before it throws, or the next statement on that connection reads the tail of this one. The stream path already discards the connection; the exec path should not have to.
- **Catalog metadata queries** go through `MSSQLSimpleQuery` too. None calls a procedure, so W1 does not change them; W4 makes a parse error in one of them an error rather than an empty result — which is the correct answer and a new one.

## 5. Acceptance

- T1 in CI, T2 in the corpus, T3 green against a live server.
- All four procedure shapes run through `mssql_exec` and `mssql_scan` and the next statement on the same connection succeeds without the pool discarding it.
- `EXEC p; RAISERROR(…, 16, 1)` through `mssql_exec` raises.
- `SELECT … FOR BROWSE` through `mssql_scan` returns rows.
- An unknown token reports its byte in hex.
