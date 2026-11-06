# Spec 092: read code-page strings as bytes and transcode them on the client

Status: draft (2026-11-06; revised the same day after review, and again after the owner's direction
below), on branch `feat/091-code-pages` beside spec 091's implementation, from main `4c65cfb`.
Spec only: nothing here is built. Depends on spec 091's module (`src/codec/code_page/`, implemented
on this branch).

This is 091's consumers (a), the catalog read path, and (b), raw `mssql_scan` correctness (#224).
The write path stays as it is: `N'...'` literals, BCP as `nvarchar`, and the server replacing what a
code page cannot hold with best fit or `?`. That is 091's consumer (d), a later spec.

**The owner's direction (2026-11-06).** 091's module serves two paths:
- the **backup reader** (085 / 086) needs every code page, double-byte included, because no server
  converts a backup;
- on the **TDS stream** the client-side transcode is used only where it is faster than today's
  path. That is the single-byte pages: the kernel is measured faster than today's simdutf UTF-16
  decode on ASCII, Latin and Cyrillic and close on Cyrillic with typography (091 Benchmark), and the
  wire halves; each workload still passes the A/B gate end to end (D10). The double-byte pages keep
  the server-side `CAST`: their wire bytes are nearly equal (66 against 70 KB for the Japanese
  benchmark chunk) and the client's decode costs about 11.6 times simdutf's (~43 against 3.7 ns a
  value, 091 Benchmark).

## Problem

**Today.**
- **The catalog cast.** A `char` / `varchar` / `text` column under a non-UTF-8 collation is read by
  the catalog as a server-side cast (spec 026):
  - `varchar` / `text` as `CAST(col AS NVARCHAR(n))` (`NeedsNVarcharConversion`,
    `src/catalog/mssql_column_info.cpp:458-481`; `BuildReadExpression`, `:567-570`);
  - `char(n)` as `CAST(col AS NCHAR(n))`, or past 4000 as `RTRIM(CAST(col AS NVARCHAR(MAX)))`
    (#405, `:551-565`).
- **The length.** It comes from `NVarcharLength` (`:488-499`), in an anonymous namespace. Spec 026
  added it as `GetNVarcharLength` in `table_scan.cpp`
  (`specs/026-varchar-nvarchar-conversion/tasks.md:44`); that name survives only in a comment
  (`mssql_column_info.cpp:80`) and in `docs/roadmap-v0.3.0.md:155`. It makes a
  `varchar(4001..8000)` `NVARCHAR(MAX)`, so a bounded column travels as PLP
  (`mssql_column_info.cpp:495-497`).
- **`mssql_convert_varchar_max = false`** exempts a declared `varchar(max)` from the cast
  (`mssql_column_info.cpp:477`). It never exempts `text` (`:474-476`).
- **The client side.** The server transcodes, the wire carries UTF-16LE, and the client decodes it
  with simdutf (`src/codec/string_codec.cpp:493-580`).
- **A raw `mssql_scan`** has no cast. It hands the code-page bytes to a DuckDB VARCHAR through the
  binary kernel (`src/codec/staging/column_ops.cpp:441-457`, `src/codec/binary_codec.cpp:89-136`),
  with a once-per-stream `WARNING` (`src/query/mssql_result_stream.cpp:944-990`; the
  `mssql_warn_non_utf8_collation` switch, `src/connection/mssql_settings.cpp:131-143`).

**What it costs.**
- The wire carries 2 bytes per character where a single-byte page needs 1.
- The server spends CPU on the conversion. That has not been measured.
- A bounded `varchar(4001..8000)` goes as PLP.
- A raw scan returns text that is not valid UTF-8 for anything outside ASCII.
- The analogue measured for UTF-8 collations in #225: 85.8 -> 43.9 MB on the wire and 0.44 -> 0.25 s
  wall for 1M ~41-character values (`docs/proposals/utf8-wire-path.md:16-28`).
  - Spec 057 withdrew the note's absolute wire figures, which came from the extension's byte
    counter under `MSSQL_DEBUG=2` (`utf8-wire-path.md:14`), and kept their ratio
    (`specs/057-write-representation-aware/spec.md:1327-1329`). It withdrew nothing about the wall
    times, which the note gives as three wall-clock passes of the query (`utf8-wire-path.md:22-28`).

**The change.**
- Read such columns as bytes where their page is single-byte, and transcode code page -> UTF-8 on
  the client with 091's kernel (D1, D10). The wire then carries one byte per character. The server
  does no CONVERT.
- A double-byte column keeps today's cast on the catalog path and in pushed statements (D10).
- A raw `mssql_scan` has no cast to keep: it returns correct UTF-8 for every page 091 has,
  double-byte included, which fixes #224 for real.

## Decisions

- **D1: which columns.** A `char` or `varchar` column, bounded or MAX, under a collation whose page
  is single-byte and has a table in 091 (`FindCodePage(page)` non-null and `!IsDoubleByte`), is read
  as itself in every read expression when its catalog transcodes (D4). The page comes from 091's
  `FindCollationByName(name, length)->code_page`. A double-byte page keeps its cast (D10).
  - **On a catalog stream the page is the name's.** The stream is handed, per column, the page its
    read expression assumed, and checks it once at COLMETADATA against the wire's
    (`CodePageForWireCollation(lcid, utf8, sort_id)`). A wire lookup that misses
    (`CODE_PAGE_NOT_FOUND`) or disagrees fails the statement with a named error (the column, its
    collation name, both answers): the tables disagree with themselves, and verbatim bytes would be
    a silent wrong answer. The wire key is derived from `COLLATIONPROPERTY`, not read from a wire
    (091 D9, "still to verify"), so this check is also what makes a wrong derivation loud. Only a
    raw scan, which has no name, takes the wire's answer alone; a miss there keeps today's bytes
    and the warning (D9).
  - **`text` keeps its cast on the catalog path. That is this spec's scope, not a limit.** Two
    per-value paths decode the catalog's columns: the composite-key rowid scan, which fills the
    rowid STRUCT through `ProcessRow` (`src/table_scan/table_scan.cpp:781-842`,
    `src/query/mssql_result_stream.cpp:300-303`, `:355`), and INSERT ... RETURNING's parser
    (`src/dml/insert/mssql_returning_parser.cpp:52`). Both go through `TypeConverter::ConvertValue`,
    which has no `TEXT` / `NTEXT` arm and throws (`src/tds/encoding/type_converter.cpp:322-327`,
    `:349-350`).
    - Adding the two arms is cheap since #197: the row reader frames the legacy LOBs
      (`src/tds/tds_row_reader.cpp:278-309`). A follow-up adds them and drops `text`'s cast.
    - The comment that says no codec handles `TEXT` on the wire (`mssql_column_info.cpp:474-476`)
      has been stale for the staged path since then (`column_ops.cpp:406-411`). It still holds for
      `ConvertValue`, and is reworded so.
  - **A raw scan's `text`** stages as `LobStageBinary` (`column_ops.cpp:406-411`) and is transcoded
    like `varchar`.
  - **A UTF-8 `char(n)` on a connection without UTF8SUPPORT** keeps `NCHAR(n)`
    (`mssql_column_info.cpp:560`). The server transcodes that one, and 091 is not involved.
- **D2: the decoders.** `DATAMODEL.md:649-655` names three paths that publish single-byte text. All
  three learn the page, from one field.
  - **The page is resolved once per column, at stream level.** A new `ColumnMetadata::code_page`
    (0 = none) is set by `MSSQLResultStream` after COLMETADATA and before `RowStager::Configure`
    (`mssql_result_stream.cpp:306-307`), where the catalog's flag is known, and by
    `MSSQLReturningParser` in its metadata callback (`mssql_returning_parser.cpp:18-22`) from its
    target's flag. It stays 0 unless the catalog transcodes (D4) and 091 has the page: on a catalog
    stream it is the single-byte page a bare read expression assumed (D1), on a raw scan the wire's
    page, double-byte included. No decoder looks a page up per value.
  - **The DuckDB wrapper is this spec's**, `src/codec/code_page/code_page_codec.cpp` and its header
    beside the module's (091 D4 kept the core DuckDB-free):
    - `DecodeChunkFromStaging(st, count, col, page, out)` for the staged kernel: D12's pipeline on a
      single-byte page, `DecodeValues` on a double-byte one (a pair must not cross two values);
    - `DecodeFromTds(bytes, col, page, out, row)` for the per-value arm and the constant column,
      through `DecodeToUtf8`;
    - both publish `string_t`, trim a `BIGCHAR`'s output (D3), and size the output at `Utf8Bound`
      while `3 * payload` fits a DuckDB string, else by `ExactUtf8Length`; a column whose exact
      output still exceeds `UINT32_MAX` holds a value no DuckDB string can hold and is refused,
      naming the column (091 F9, D5).
    - The fuzz harnesses that compile `src/codec/*.cpp` non-recursively, `fuzz_row_stager` and
      `fuzz_bcp_encode`, gain `src/codec/code_page/*.cpp` once `string_codec.cpp` calls the module,
      or they stop linking (`fuzz/README.md:80-84`).
  - **The TDS parser keeps no dependency on the codec module.** `tds_column_metadata.cpp` leaves the
    field 0. It is built without the module in `CPP_TEST_SOURCES` (`Makefile:407-418`) and in
    `fuzz_tds_tokens` (`fuzz/build.sh:89-95`, `:139`).
  - **The flag reaches the stream as a constructor argument with no default** (D5's rule), at the
    three construction sites: `MSSQLQueryExecutor`'s two (`src/query/mssql_query_executor.cpp:52`,
    `:118`) and the staged DML's `#out` read (`src/dml/mssql_staged_dml.cpp:436`).
  - **Staged.** A new `FinalizeKernel::CodePage` (`src/include/codec/staging/column_ops.hpp:103-117`).
    `ResolveKernel` chooses it for `P2StageBinary` / `PlpStageBinary` / `LobStageBinary`
    (`column_ops.cpp:266-269`) when the column is single-byte text (`IsSingleByteTextColumn`,
    `src/include/tds/tds_column_metadata.hpp:72-74`) and its `code_page` has a table.
    - `ResolveKernel`'s switch is over the append arm (`column_ops.cpp:239-285`), so the new kernel
      needs no case there. The switches over the kernel have no `default`, and `-Wswitch` makes each
      of them say what it does with `CodePage`: the finalize switch (`row_stager.cpp:600-624`),
      `DecodeFirstValue` (`:785-807`) and `FinalizeKernelName` (`column_ops.cpp:478-498`).
    - `FINALIZE_KERNEL_COUNT` (`column_ops.hpp:120`) is a literal 8. It sizes the counter arrays
      (`src/include/codec/staging/row_stager.hpp:62-64`) and the counters' print loop
      (`mssql_result_stream.cpp:707`). It becomes 9, with a `static_assert` that it equals the last
      enumerator + 1, so a later kernel cannot index past the arrays.
  - **Constant.** `DecodeFirstValue` (`row_stager.cpp:777-808`) calls 091's `DecodeToUtf8` through
    the wrapper, once per column per chunk, as the comment there argues for the families
    (`:762-766`).
  - **Per value.** `string::DecodeFromTds`'s CHAR / VARCHAR branch (`string_codec.cpp:915-925`) gets
    a code-page arm, taken when `code_page` has a table. The paths that reach it (D1) are per value
    for every type already. Because the stream sets `code_page` only when its catalog transcodes,
    a bare `varchar(max)` read per value by the composite-rowid scan (with
    `mssql_convert_varchar_max` false) keeps today's verbatim bytes under `client_transcode false`.
  - **A miss** on a raw scan (a collation newer than the tables) keeps the binary kernel, today's
    verbatim bytes, and the warning (D9). On a catalog stream it is D1's named error.
- **D3: a single-byte page's `char(n)` is read bare as `BIGCHAR`.** The padded bytes are
  transcoded, then trailing U+0020 is trimmed off the output by the wrapper (D2; the module does no
  trim, 091 D5). A double-byte page's `char(n)` keeps `CAST(col AS NCHAR(n))` (D10).
  - This equals today's `NCHAR(n)` read plus the codec's NCHAR trim (`mssql_column_info.cpp:560-565`;
    staged, of the output, `string_codec.cpp:932-933`; per value, of the input's 0x20 0x00 units,
    `:890-893`, the same for valid UTF-16, `:870-878`) whenever 091 equals the server's `CAST`.
    So #405's agreement on joins, `GROUP BY` and `DISTINCT` stays.
  - The code-page arm trims the output on every path, the per-value one included: on a double-byte
    page (a raw scan) an input 0x20 can be a trail byte (932 `81 20` -> U+30FB, 091 F14).
  - Past 4000 bytes no `RTRIM(... NVARCHAR(MAX))` is needed: `BIGCHAR` goes to 8000.
- **D4: the escape hatch, `mssql_client_transcode`** (BOOLEAN, default `true`, `SetScope::GLOBAL`),
  plus an ATTACH option `client_transcode` that wins over it for its catalog.
  - **Read once per catalog at ATTACH**, as `mssql_remote_pushdown` is
    (`src/connection/mssql_settings.cpp:573-583`, `src/include/catalog/mssql_catalog.hpp:69-71`).
    The ATTACH option is parsed beside `remote_pushdown` / `native_types`
    (`src/mssql_storage.cpp:1301-1311`) and takes a string form too (#325).
  - **Fixed for the catalog's life.** One catalog must not mix the two forms: the raw-scan types
    (D7) and the describe cache would disagree. The cache key is the statement, the declarations
    and `native_types` (`src/include/query/mssql_describe_cache.hpp:55`), and stays so.
  - **One session can measure both** (Measurement): two ATTACHes of one DSN, one with
    `client_transcode false`.
  - **`false` restores today's read SQL, wire types and results**: the read expressions of the
    catalog scan and of pushed statements, the raw-scan types, the raw scans' verbatim bytes, the
    warning.
  - **It does not restore the #361 parameter declarations.** Those follow 091 D8's answers whatever
    the hatch says: a constant 091 finds representable goes as `varchar` where it went as
    `nvarchar`, and one #361 wrongly listed goes back to `nvarchar`. 091 D8 is a follow-up of that
    spec; until it lands the declarations are #361's, as today.
- **D5: the read expressions take the decision as an argument.**
  - `BuildReadExpression`, `NeedsNVarcharConversion` and `BuildReturningExpression` get a
    `client_transcode` parameter with **no default**. A caller cannot keep the cast by forgetting
    it, the rule `ReleaseBcpConnectionOnError`'s `reset_on_release` follows (CLAUDE.md,
    `mssql_reset_connection`). `BuildReadExpression`'s trailing parameters have defaults
    (`src/include/catalog/mssql_column_info.hpp:162-165`), so the new one goes before them.
  - The callers: the catalog scan's `BuildColumnExpression` (`table_scan.cpp:94-105`, called at
    `:237`, `:262`, `:314`), the 079 select list (`mssql_node_writer_select.cpp:438-444`), INSERT's
    OUTPUT lists (`src/dml/insert/mssql_insert_statement.cpp:58-59`, `:83-88`) and the staged
    `#out` (`mssql_staged_dml.cpp:79-83`).
  - They stay pure functions of the type and the collation name (`mssql_column_info.hpp:148-151`),
    and of the flag. The page and its family come from 091 by name (`FindCollationByName`,
    `FindCodePage`, `IsDoubleByte`), so `MSSQLInsertColumn`, which has no `code_page`
    (`src/include/dml/insert/mssql_insert_config.hpp:110-161`), needs none.
- **D6: RETURNING follows the read expression.**
  - `BuildReturningExpression` is "the read expression, with two exceptions"
    (`mssql_column_info.hpp:167-175`). A transcoded column falls under neither exception.
  - The `char(n)` exception (`mssql_column_info.cpp:513-521`) remains only where the read is still
    `NCHAR(n)`: a UTF-8 `char` without UTF8SUPPORT, a double-byte page's `char` (D10), or
    `client_transcode false`.
  - `#out` and `#mssql_returning_<uuid>` then hold the source's `varchar` in its own collation,
    since "SELECT INTO copies each column's type, length and collation exactly"
    (`mssql_staged_dml.cpp:47-50`). The rows read back are transcoded by the staged kernel
    (UPDATE / DELETE, `:421-440`) or the per-value arm (INSERT, `mssql_returning_parser.cpp:52`).
- **D7: raw-scan types.** `NativeStringType` (`src/mssql_functions.cpp:302-316`) annotates a
  code-page `char` / `varchar` as `MSSQL_VARCHAR(n, collation)` when the catalog transcodes and the
  page has a table, double-byte included: a raw scan transcodes every page (D10). Today it leaves
  it plain because "a raw scan hands a code-page varchar's bytes over as they are" (the comment at
  `:294-301`; CLAUDE.md's `mssql_catalog_native_types` row says "untranscoded").
  - The collation name comes from the describe (`DescribeFirstResultSet`, `:465-538`).
  - **`prepared := true` asks the describe for such a column too.** `IsDeclaredTypeAmbiguous`
    (`:349-355`) counts a code-page `char` / `varchar` as ambiguous when the catalog transcodes and
    the page has a table, as #387 made it for a UTF-8 one, so the bind asks
    `sp_describe_first_result_set` for the name. Both paths then report the same type, at #387's
    cost of one more round trip.
  - A statement run at bind has no describe, and its code-page columns stay plain VARCHAR as its
    UTF-8 ones do (`:357-368`, `:461-464`). No new round trip is added there.
  - Follow-up: a row lookup by wire key, which 091 leaves for its wire-bytes follow-up (091 D9,
    D10), would give the name wherever one collation holds the key. Some keys are shared: MS-TDS's
    collation flags carry no `_SC` bit (2.2.5.1.2), and 1,488 names exist both with and without
    `_SC` (`tools/code_pages/data/collations.psv`). Typing the COLMETADATA-only shapes from it, and
    dropping #387's describe for those columns, is a later change.
  - `StreamMatchesBoundShape` (`:401-446`) needs no change. An annotated `varchar(n)` against a
    `BIGVARCHAR(n)` stream compares equal on kind and length.
- **D8: the 079 writer pushes more where the catalog transcodes.**
  - **The veto lifts only when the catalog transcodes and the column's page is single-byte with a
    table** (D1). "a code-page varchar in a computed value" exists because such a value "arrives
    untranscoded (#224)" (`src/pushdown/mssql_node_writer_values.cpp:387-393`; the LAG / LEAD default,
    `mssql_node_writer_windows.cpp:157-159`). Under transcoding the value arrives as `BIGVARCHAR`
    in the expression's collation and is transcoded. Under `client_transcode false`, for a
    double-byte page (cast, D10), or for a collation 091 has no table for, the veto stays.
  - **The flag reaches the writer** as a new `SQLWriterOptions::client_transcode`, beside
    `convert_varchar_max` and `utf8_on_wire` (`src/include/pushdown/mssql_sql_writer.hpp:126-131`).
    It is the catalog's, fixed at ATTACH, so the catalog sets it where it sets `utf8_on_wire`
    (`src/catalog/mssql_catalog_pushdown.cpp:253`); `FromContext` reads only the session's settings
    (`src/pushdown/mssql_sql_writer.cpp:60-62`).
  - **`plain_read` turns true for a single-byte code-page column.** The select list sets it when
    the read expression is the bare column (`mssql_node_writer_select.cpp:438-445`). It gates:
    - an ORDER BY under DISTINCT (`src/include/pushdown/mssql_node_writer.hpp:114-116`, used at
      `mssql_node_writer_select.cpp:681`);
    - a derived table's key (`mssql_node_writer_from.cpp:438`, `:460`);
    - subqueries (`mssql_node_writer_subqueries.cpp:181`);
    - set operations: a member's columns keep the server's shape only when plain
      (`mssql_node_writer_select.cpp:1085`), and a string column must be plain on both sides, else
      the veto (`:1100-1106`).

    More shapes push, and with them spec 079's D4, collation semantics for sets, reaches code-page
    columns (What changes for a user).
  - **`string_agg` keeps its `nvarchar(max)` cast.** It also bounds the result
    (`mssql_node_writer_select.cpp:300-305`).
- **D9: the settings afterwards.**
  - **`mssql_warn_non_utf8_collation` is kept, narrowed** to the two cases in which verbatim bytes
    still reach a DuckDB VARCHAR: a catalog with `client_transcode false`, and on a raw scan a
    collation 091 has no table for (one newer than the tables, 091 D9).
    - Why kept, not deprecated: it is the only signal for those two cases. A `.duckdbrc` that SETs
      it keeps working, and its default stays `true`.
    - The message's remedies change (`mssql_result_stream.cpp:978-983`): `client_transcode`, or a
      `CAST` in the query, or "the collation is newer than this build's tables".
  - **`mssql_convert_varchar_max`** (`mssql_settings.cpp:585-593`; read at `table_scan.cpp:193`,
    `src/pushdown/mssql_sql_writer.cpp:62`, `src/catalog/mssql_catalog_plan.cpp:130`, `:473`).
    - It keeps its meaning under `client_transcode false`, and for the double-byte pages, which
      keep the server cast (D10).
    - Otherwise it has no effect: a single-byte page's `varchar(max)` is read as itself and
      transcoded, which is what `false` was for, and it now arrives as valid UTF-8.
    - Its registration and default stay. It is reconsidered with the escape hatch.
- **D10: by page family: single-byte on the client behind a gate, double-byte on the server.**
  - **One predicate beside the read expression,** `TranscodeOnClientByDefault(page)`: false for a
    double-byte page (`IsDoubleByte`), and for a single-byte one the gate's verdict. Every read
    expression asks it (D1, D3, D5, D8); a raw scan does not (below).
  - **Double-byte pages keep the server's `CAST`: the owner's decision (2026-11-06), measured.**
    Japanese is 2 bytes a character in 932 and in UTF-16, so the wire does not shrink (66 against
    70 KB for the benchmark chunk, where 10 % of the characters are ASCII digits), and the client's
    decode costs ~43 ns a value against simdutf's 3.7 (091 Benchmark). 936 / 949 / 950 take the same
    kernel (`DecodeRun<true>`, 091 D5) on texts of the same shape, and are not measured apart. The
    decision is revisited when 091's double-byte SIMD follow-up lands.
  - **The single-byte gate.** Transcoding is the default for the single-byte pages only if every
    single-byte workload (Measurement) meets **B <= A x 1.02 in wall time**, A being
    `client_transcode false` and B on, each the median of 3 interleaved rounds. Server CPU is
    reported beside each workload, not gated; a workload that passes on wall but costs the server
    more is named in the PR for the owner. The micro-benchmark predicts a pass on ASCII, Latin and
    Cyrillic (the client faster and the wire halved) and a close call on Cyrillic with typography
    (the client 1.25x slower, the wire still halved); the gate, not the prediction, decides.
  - **The environment that decides it is local:** the SQL Server 2022 container of
    `make docker-up` (`docker/docker-compose.yml:15`) on the machine that runs DuckDB. It is the hard
    case for B, since the wire bytes B saves cost least there. A networked run, where one is at
    hand, is reported and does not gate.
  - **Per architecture.** The measured kernel is arm64's NEON one. On x86-64 a single-byte page
    takes 091's scalar `DecodeRun` (the SSE2 ASCII step and a table step per other byte, about
    1 GB/s on non-ASCII text) until 091's x86-64 follow-up. So x86-64 is gated on its own run; if it
    fails there, `TranscodeOnClientByDefault` answers per build (NEON builds transcode, the others
    keep the cast) until that follow-up, which is the **owner's call**.
  - **Raw scans transcode every page 091 has, double-byte included, for correctness.** A raw scan
    has no cast to keep: its alternative is today's verbatim bytes (#224). The cost of a double-byte
    raw scan is reported (W3), not gated; a user who wants the server's conversion writes
    `CAST(c AS nvarchar(n))` in the query.
- **D11: unchanged, and why.**
  - **ORDER BY / TOP pushdown.** `OrdersLikeDuckDB` names no string (`mssql_column_info.cpp:345-367`).
    `OrdersLikeDuckDBAsBytes` wants `is_utf8` (`:369-377`). A code page's byte order is not code
    point order: in 1252, 0x80 (U+20AC) sorts below 0xE9 (U+00E9).
  - **Filter pushdown parameters (#361).** The comparisons stay on the server
    (`mssql_expression_vocabulary.cpp:238-289`). Their answers move only through 091 D8.
  - **Rowid keys.**
    - `IsRoundTripExactForKey` answers `Exact` for `varchar` (`mssql_rowid_key_choice.hpp:131-143`).
      The key's value is the server's `CAST` result today, and 091's equal result after.
    - The VALUES path compares the key with an `N'...'` literal: on a `SQL_` collation cast to the
      column's type in the column's collation, `CAST(lit COLLATE <col> AS varchar(n))`
      (`KeyComparand`, `src/catalog/mssql_primary_key.cpp:263-275`, the cast at `:274`, chosen at
      `:111-126`), elsewhere in Unicode. Either way the literal is made from the value read, so it
      finds the row it came from as it does today.
    - A composite key's STRUCT is filled per value, through the D2 arm.
  - **The `#stage` (`mssql_staged_dml.cpp:46-62`, string keys matched as `varbinary`, `:112-115`)
    and the MERGE `#src`** (`LikeTargetOf`, `src/dml/merge/mssql_merge_rewrite.cpp:66-101`). They
    are the write side: DuckDB values go in by BCP as before. A MERGE reads nothing back
    (RETURNING is refused); its source, when a catalog scan of a single-byte page, is transcoded.
  - **The catalog's own types.** `NativeDuckDBType` already annotates a code-page `varchar` with its
    collation (`mssql_column_info.cpp:114-127`). The pushdown vehicle's `column_types`, "a code-page
    varchar as its declared MSSQL_VARCHAR(n)", now meet a `varchar` wire column. `ColumnTypeFits`
    takes a string into a string either way (`src/mssql_functions.cpp:641-658`).
  - **`MSSQLSimpleQuery`'s verbatim copy** (`src/query/mssql_simple_query.cpp:47-50`). Its callers
    read `sysname` / `nvarchar` and ASCII codes such as `sys.objects.type`.
- **D12: the chunk pipeline of a single-byte page.** `DecodeChunkFromStaging` (D2) takes one
  column's chunk in three steps, each a measured choice against `DecodeValues` value by value (the
  benchmark's `fused`, `test/cpp/bench_code_page.cpp:8-21`), kept only where it wins (task 9):
  1. **An all-ASCII chunk is not copied.** The output equals the input, so the staged payload
     becomes the vector's string buffer: the staging gives its buffer up and the vector holds it
     as auxiliary data (`StringVector::AddAuxiliaryData`,
     `duckdb/src/include/duckdb/common/vector/string_vector.hpp:107-108`; `AuxiliaryDataHolder`,
     `duckdb/src/include/duckdb/common/types/vector_buffer.hpp:46-52`). Each value's `string_t`
     points at its staged offset, as the binary kernel's do into its copy today
     (`src/codec/binary_codec.cpp:114-135`). The verdict needs a pass over the bytes, and the
     staging, which keeps its buffer across chunks and never shrinks it
     (`src/include/codec/staging/column_staging.hpp:91-93`), grows a new one: that is the cost
     against step 2's copy.
  2. **Otherwise the ASCII prefix by `CopyAscii`** (091 D5), 64 bytes a step, test and copy in the
     same loads: every value that ends before the first non-ASCII byte keeps its staged offset and
     length (the benchmark's `ascii64` form).
  3. **The rest through the NEON kernel as one string**, from that byte on. On a single-byte page a
     byte reads the same wherever a value starts, and the binary staging holds the values back to
     back with no delimiter (`src/include/codec/staging/column_staging.hpp:220-226`; no +2 in the
     arm's bound, `src/codec/staging/column_ops.cpp:452-456`), so the decoded payload is the values'
     decodings back to back. **Each value's output offset comes from the kernel's masks, not from a
     delimiter walk:** the kernel records each 32-byte step's output position, and a value boundary
     inside a step adds the lengths of the step's bytes before it, from the length codes the step
     already computed (`Compaction` / `Compaction3`). Nothing is staged between values, and the
     output is not walked.
     - The UTF-16 kernel needs the walk: it stages a U+0000 after each value
       (`column_staging.hpp:467-484`), converts the column in one call and searches the output for
       the zeros, with a second pass when a value holds a U+0000 of its own
       (`src/codec/string_codec.cpp:546-579`). Code-page bytes hold 0x00 as freely, which the masks
       never see as a boundary.
     - This is `code_page::DecodeColumn`, made in 091's x86 PR (#528) ahead of this spec: two
       blocks in (the staged bytes, offsets / lengths and validity), two out (the UTF-8, decoded
       straight into the vector's heap, and each row's `string_t`, written as `StringView16` into
       the vector's data -- no loop over the values after it). The kernel records the masks of
       each 16 input bytes that are not all ASCII (an ASCII step stores nothing), per 16 KB block;
       the granules' output positions come from a prefix over the masks. Measured on Apple M4
       against the production `DecodeChunkFromStaging` (`make bench-code-page-column`, one
       column of 2048 rows, staging to finished vector): 1.1-2.4x faster in every cell but long
       1251 text with dense typography (0.9-1.35x, by the text: the kernel's own branches). On
       x86-64 the AVX2 and AVX-512 kernels record the same masks (AVX-512: its 64-bit masks
       spread by pdep); an x86 with neither goes value by value.
       The unit test checks it against the server's answers; the fuzz oracle (equal to
       `DecodeValues` value by value) is to add.
  - A double-byte page (raw scans only, D10) takes `DecodeValues`: a pair must not cross two values,
    and its kernel has no masks.

## Audit: every place that relies on the NVARCHAR read form

Files: `ci` `src/catalog/mssql_column_info.cpp`, `rs` `src/codec/staging/row_stager.cpp`, `sc`
`src/codec/string_codec.cpp`, `co` `src/codec/staging/column_ops.cpp`, `fn`
`src/mssql_functions.cpp`, `sd` `src/dml/mssql_staged_dml.cpp`, `is`
`src/dml/insert/mssql_insert_statement.cpp`, `ts` `src/table_scan/table_scan.cpp`, `rr`
`src/query/mssql_result_stream.cpp`, `nw` `src/pushdown/mssql_node_writer_<x>.cpp`, `pk`
`src/catalog/mssql_primary_key.cpp`, `rp` `src/dml/insert/mssql_returning_parser.cpp`, `cm`
`src/include/tds/tds_column_metadata.hpp`, `qe` `src/query/mssql_query_executor.cpp`, `sw`
`src/include/pushdown/mssql_sql_writer.hpp`, `mp` `src/catalog/mssql_catalog_pushdown.cpp`.
"After" assumes `client_transcode` on and a single-byte page 091 has. A double-byte column keeps
"today" on every catalog and pushdown row; on a raw scan it is transcoded (D10).

| site | today | after |
|---|---|---|
| `BuildReadExpression` (`ci:526-575`) | casts char, varchar, text | bare char, varchar; text cast (D1) |
| `NeedsNVarcharConversion` (`ci:458-481`) | every code-page text | text; no table; double-byte (D10) |
| `TranscodeOnClientByDefault` (new) | - | page family and gate (D10) |
| `NVarcharLength` (`ci:488-499`) | MAX past 4000 bytes | cast path only; varchar(5000) bare |
| `BuildReturningExpression` (`ci:503-524`) | rowversion, char(n) | as the read (D6) |
| `BuildColumnExpression` (`ts:94-105`; `:237`, `:262`, `:314`) | the read, cast | passes the flag (D5) |
| INSERT RETURNING `OUTPUT` (`is:58-59`) | NVARCHAR, per value | varchar, per-value arm (D2) |
| `OUTPUT ... INTO #mssql_returning` (`is:77-94`) | NVARCHAR column | source varchar, collation (D6) |
| RETURNING parser (`rp:18-22`, `:52`) | verbatim per value | sets `code_page` (D2) |
| UPDATE / DELETE `#out` (`sd:79-95`, `:421-440`) | NVARCHAR | varchar, staged kernel |
| `mssql_convert_varchar_max` (`mssql_settings.cpp:585-593`) | MAX opt-out | cast path only (D9) |
| `ColumnMetadata` (`cm:15-27`) | 5 collation bytes | + `code_page`, by the stream (D2) |
| stream construction (`qe:52`, `:118`; `sd:436`) | no flag | flag, no default (D2) |
| stream, staged (`rs:600-624`) | binary kernel, verbatim | `FinalizeKernel::CodePage` (D2, D12) |
| stream, constant (`rs:777-808`) | `binary::DecodeFromTds` | `DecodeToUtf8` via the wrapper (D2) |
| stream, per value (`sc:915-925`) | verbatim, input trim | code-page arm, output trim (D3) |
| DuckDB wrapper (`codec/code_page/code_page_codec.cpp`) | - | new (D2) |
| back-to-back decode entry (091's module) | - | new, mask boundaries (D12) |
| fuzz `row_stager`, `bcp_encode` (`fuzz/README.md:80-84`) | `src/codec/*.cpp` | + `code_page/` (D2) |
| kernel switches (`rs:600`, `rs:785`, `co:479`) | 8 kernels | + `CodePage`, `-Wswitch` (D2) |
| `FINALIZE_KERNEL_COUNT` (`column_ops.hpp:120`) | literal 8 | 9 + `static_assert` (D2) |
| counter arrays (`row_stager.hpp:62-64`, `rr:707`) | sized by the count | unchanged |
| 079 options (`sw:126-131`; set at `mp:253`) | no flag | `client_transcode` (D8) |
| 079 select list (`nw` select `:438-445`) | cast; `plain_read` false | bare; `plain_read` true (D8) |
| 079 set operations (`nw` select `:1085`, `:1100-1106`) | code-page string vetoed | pushed (D8) |
| 079 computed value (`nw` values `:387-393`) | vetoed | allowed (D8) |
| 079 `column_types` (`fn:641-658`) | fits an NVARCHAR wire | fits a varchar wire |
| `NativeDuckDBType` (`ci:72-132`) | annotates | unchanged |
| `NativeStringType` (`fn:302-316`) | plain VARCHAR | annotated via describe, every page (D7) |
| `IsDeclaredTypeAmbiguous` (`fn:349-355`) | UTF-8 strings | + code-page char / varchar (D7) |
| describe cache (`mssql_describe_cache.hpp:55`) | key | unchanged; new texts, new entries |
| shape checks (`fn:401-446`, `:664-676`) | UTF-8 mismatch tolerated | unchanged |
| ORDER BY pushdown (`ci:345-377`) | no code-page string | unchanged (D11) |
| filter parameters (`mssql_expression_vocabulary.cpp:238-289`) | server-side | unchanged; 091 D8 |
| rowid (`mssql_rowid_key_choice.hpp:131-143`) | `Exact` | unchanged (D11) |
| `KeyComparand` (`pk:111-126`, `:263-275`) | varchar(n) on `SQL_` | unchanged (D11) |
| `#stage` (`sd:46-62`), MERGE `#src` (`mssql_merge_rewrite.cpp:66-101`) | write side | unchanged |
| warning (`rr:944-990`) | every code-page text | untranscoded only (D9) |

## Surface

- **Setting** `mssql_client_transcode`, BOOLEAN, default `true`, GLOBAL, read at ATTACH (D4).
- **ATTACH option** `client_transcode`, BOOLEAN, which wins over the setting for its catalog.
- **`mssql_warn_non_utf8_collation`** narrowed (D9). **`mssql_convert_varchar_max`** under D9.
- **No new function.** `EXPLAIN` of a catalog scan or a pushed statement shows the bare column of
  a single-byte page, and the cast of a double-byte one as today.

## What changes for a user, and how it is handled

- **A raw `mssql_scan` of code-page text returns UTF-8, not the stored bytes**, on every page 091
  has, double-byte included. A query that recovered the bytes (`::BLOB`, `encode()`) gets other
  bytes. The escape hatch, or `CAST(col AS varbinary(n))` in the T-SQL, gives the bytes. The change
  is listed under *Changed* in the CHANGELOG.
- **The raw-scan types change.** `typeof()` of such a column says `MSSQL_VARCHAR(n, collation)` (D7),
  `prepared := true` included. `native_types false` restores plain VARCHAR, as today for every
  string.
- **The pushed T-SQL changes** for single-byte columns, and more statements push (D8). Spec 079's
  D4, collation semantics for sets (`GROUP BY`, `DISTINCT`, join keys), then reaches more queries;
  it is documented already (`README.md:59`, `:76-90`). A double-byte column's T-SQL is unchanged.
- **On a `SQL_` collation, a pushed `DISTINCT`, set operation or derived-table key over a code-page
  column dedupes by the varchar rules, not the nvarchar rules.** `SQL_` collations compare varchar
  and nvarchar under different rules (`mssql_primary_key.cpp:111-112`). Today the server compares
  the read's NVARCHAR cast, where such a shape pushes at all; after, the column itself. That is
  spec 079's D4 for sets (CLAUDE.md, `mssql_remote_pushdown`), settled and not reopened here. A
  test pins it (Tests).
- **Tests that pin today's form** (the first draft's "nine SQL files assert the cast" was wrong:
  none of those nine asserts the cast text):
  - the cast in `EXPLAIN` text: `test/sql/catalog/char_padding.test:64`,
    `test/sql/pushdown/remote_pushdown_derived.test:71`, `remote_pushdown_single_table.test:832`;
  - a veto on it: `remote_pushdown_single_table.test:605-610`, `remote_pushdown_windows.test:511-515`;
  - the raw-scan types: `test/sql/mssql_scan.test:47-50` (`typeof(name)` of a code-page
    `varchar(50)` is `VARCHAR`), `test/sql/query/mssql_scan_native_types.test:18-22`, `:47`, `:58`,
    `:118-121`, `:128-134`, and `mssql_scan_describe_types.test:44-45` (`c_char`, `c_varchar`; the
    MAX and `text` rows at `:46`, `:50` stay VARCHAR);
  - the raw-scan warning: `test/sql/query/collation_metadata_warnings.test` (D9);
  - C++: `test/cpp/test_sql_writer.cpp:220-226`, `:381-387`, `:536`, `:700`, `:787-788`, `:820`,
    `:884`, `:981-983`, the derived-table and subquery vetoes "read through a CAST" (`:997`,
    `:1083`), and `test/cpp/test_batch_builder.cpp:414-415`, whose `code` column has an empty
    collation and needs a single-byte one to pin the change (D1 takes the page from the name).
  - Value tests over code-page columns that must pass unchanged, by 091's construction:
    `test/sql/rowid/rowid_unique_index.test`, `test/sql/catalog/varchar_declaration_seek.test`,
    `test/sql/catalog/legacy_lob_scan.test` (`text` keeps its cast), `test/sql/query/type_mapping.test`.
  - The C++ expectations get both forms: the `false` form stays pinned. The SQL files are updated
    where they pin today's form, and keep a `client_transcode false` case.
- **Text from an invalid double-byte sequence is the same as today**, by 091's construction (the
  server's own answer). It differs only if 091's tables differ from the server at hand (Risks).

## Measurement plan

- **Interleaved A/B in one DuckDB session only.** Two ATTACHes of one DSN: A with
  `client_transcode false`, B with it on. Queries alternate A, B, A, B, three rounds, median.
- **Environment:** local docker, the SQL Server 2022 container of `make docker-up` on the machine
  that runs DuckDB, for the gate (D10); arm64 decides, x86-64 is run on its own where one is at hand
  (D10, per architecture); a networked server where one is at hand, reported.
- **Client numbers come from `MSSQL_COUNTERS=1` only, never `MSSQL_DEBUG`** (CLAUDE.md; spec 057
  withdrew figures taken under it, `specs/057-write-representation-aware/spec.md:1322-1329`):
  - wire bytes, `wire_bytes_in` (`mssql_result_stream.cpp:338-340`);
  - the per-kernel ns per value (`CountColumn`, `row_stager.cpp:810`), the new `code_page` kernel
    against `string`;
  - the parse and read phases.
- **Wall** from the harness's timer.
- **Server CPU** from `sys.dm_exec_query_stats.total_worker_time / execution_count` for each
  variant's statement. The two texts differ (cast or bare), so the plan cache keeps them apart; the
  numbers are deltas between snapshots. `sys.dm_exec_requests` is sampled during the run.
- **Workloads**, 1M rows each, heap, `SELECT sum(length(x))` as in the #225 note
  (`utf8-wire-path.md:22-28`):

  | | column | collation | content |
  |---|---|---|---|
  | W1 | `varchar(64)` | `SQL_Latin1_General_CP1_CI_AS` (1252) | ~40-char Latin text, ~5% non-ASCII |
  | W1a | `varchar(64)` | same | ASCII only (D12's zero-copy chunk) |
  | W2 | `varchar(64)` | `Cyrillic_General_CI_AS` (1251) | ~40-char Cyrillic |
  | W2a | `varchar(64)` | same | W2 with 1 character in 40 a dash, quote or ellipsis |
  | W3 | `varchar(64)` | `Japanese_CI_AS` (932) | ~20 characters, kana / kanji / ASCII, ~40 bytes |
  | W4 | `varchar(max)` | 1252 | ~40 characters, and ~4 KB values (PLP) |
  | W5 | `char(40)` | 1252 | padded; the #405 path |

  W2a is the micro-benchmark's close call (091 Benchmark). W3 is a raw scan, the only path on which
  a double-byte page is transcoded: A is `mssql_scan` of `CAST(c AS nvarchar(64))`, B of `c`. The
  first draft's W3a-W3c (936, 949, 950) are dropped with the double-byte family's gate (D10).
- **Expected:**
  - single-byte pages near half the wire: `2 + n` against `2 + 2n` bytes a value;
  - W3 near 1.0x on the wire (66 against 70 KB in the micro-benchmark's chunk);
  - the server's CPU the unknown, and the reason for measuring.
- **D10's gate is decided from W1-W5 without W3**, by its rule; W3 is reported. The numbers go in
  the PR.

## Tests and acceptance

- **A live oracle per page.** Every byte 0x00-0xFF in a `varchar(4)` under one collation of each of
  1250-1258, 874, 437 and 850 (the last two through their `SQL_` collations), 932, 936, 949 and 950.
  A single-byte page is read through the catalog (bare) and through a raw scan; a double-byte page
  through a raw scan (transcoded) and through the catalog (cast, D10). Each is compared with
  `mssql_scan` of `CAST(c AS NVARCHAR(4))`, the server's own answer. Then the double-byte pairs,
  invalid sequences and a lone lead at the value's end, through a raw scan.
- **A PLP split.** A raw scan of a 932 `varchar(max)` value of one ASCII byte and 100k 2-byte
  characters, so that every even chunk boundary splits a character; read whole.
  - Synthetic PLP rows with the split at each offset, through `make test-row-stager-framing`
    (`Makefile:980`).
- **`char(n)`, `varchar(max)`, `text`.**
  - `char(n)`: #405's `char(4)` / `char(6)` join, `GROUP BY` and `DISTINCT` under 1252 (bare) and
    932 (`NCHAR(n)` kept), and a `char(5000)` read bare under 1252.
  - A 1252 `varchar(5000)` arrives as `BIGVARCHAR`.
  - `text` is cast on the catalog path, and transcoded on a raw scan.
- **The chunk pipeline (D12), without a server:** the new module entry against `DecodeValues` value
  by value, in `test/cpp/codec/test_code_page.cpp`, and as a `fuzz_code_page` oracle: an all-ASCII
  chunk, a first non-ASCII byte at every offset of a 32-byte step, a value boundary at every
  position of a step, values of 0-31 and of 32 bytes and more, an embedded 0x00. The zero-copy
  chunk through the wrapper: its strings outlive the staging's next chunk.
- **Raw scans.** `mssql_scan`, `mssql_scan_params`, `mssql_scan_unsafe`,
  `mssql_scan_params_unsafe`, `prepared := true`: the values, and the types (D7), with the describe
  (`prepared := true` asking it for a code-page column) and without (a statement run at bind).
- **The catalog scan and the pushdown vehicle.** `EXPLAIN` shows the bare column of a 1252 column
  and the cast of a 932 one. A pushed `COALESCE(cp_col, 'x')` over a 1252 column (D8) equals the
  unpushed result.
- **Sets on a `SQL_` collation.** A pushed `DISTINCT` and a `UNION` over a code-page column under
  `SQL_Latin1_General_CP1_CI_AS`, on values its varchar and nvarchar rules group differently, with
  each form's rows pinned. The values are found on the server first (`'ß'` against `'ss'` is the
  candidate, **to verify**).
- **The page check.** The catalog stream's check of the name's page against the wire's is a
  function of the two, unit-tested in `test/cpp`: agreement, a miss, a disagreement, the named
  error.
- **RETURNING.** INSERT with the bare `OUTPUT`; INSERT `OUTPUT ... INTO` beside a trigger (#471's
  334 path); staged UPDATE / DELETE RETURNING.
- **MERGE** with a code-page source and target, pushed and staged.
- **Rowid.** A single code-page `varchar` key, UPDATE / DELETE through VALUES and through `#stage`;
  a composite key, the STRUCT path (D1).
- **The escape hatch.** `client_transcode false` gives today's `EXPLAIN` text and today's results;
  the option wins over the setting.
- **Warnings.** `collation_metadata_warnings.test`: none under transcoding; as today under `false`.
- **`make counters-test`** passes: the counters path, with the new kernel name.

Acceptance:
1. Every catalog read of a code-page column equals the `client_transcode false` read, byte for
   byte, over the test databases.
2. A raw scan returns the same strings as the catalog read of the same column, on every page.
3. The D10 gate is decided and recorded in the PR with W1-W5, per architecture measured; W3 is
   reported.

## Docs

- **`DATAMODEL.md` Layer 5.** 092 owns the read path's paragraphs, 091 the module's:
  - 092: the verbatim copy and its warning (`:609-627`); the three decoders, which become four
    kernels (`:649-661`); "the catalog path is normally exempt" (`:663-667`) and the `CHAR(n)`
    paragraph (`:669-676`), rewritten; "the code page itself comes from the server" (`:695-699`),
    which now holds for the filter parameters only, the read path taking it from the name and the
    wire through 091; and the code-pages section's "Nothing reads it yet" (`:758-762`);
  - 091: the module's section, written (`:756-803`); the collation lookups in "Reading a column's
    collation" (`:678-694`) and the representability line (`:700-705`) with its D8.
- **The user docs:**
  - `website/docs/reading/types.md:33-68`, `:200-210`;
  - `website/docs/reference/troubleshooting.md:150-274`, `:310` (the "does not transcode" section
    goes, but for a collation newer than the tables);
  - `website/docs/reference/settings.md:72` (the `mssql_catalog_native_types` row: "a `varchar`
    there only ..."), `:73` (`mssql_convert_varchar_max`), plus the new row;
  - `website/docs/reading/queries.md:269`, whose ordering sentence stays true.
- **`README.md`** has no code-page note today: `:59` and `:76-90` are about comparison collation. One
  line in the feature list says that single-byte code-page text is transcoded on the client and
  every code page in a raw scan, with a link to `types.md`.
- **`CLAUDE.md`:** the settings rows `mssql_catalog_native_types` (`:223`, the raw-scan types and
  `prepared := true`), `mssql_convert_varchar_max` (`:227`), `mssql_warn_non_utf8_collation`
  (`:231`) and the new one; the `mssql_utf8_support` row's "like every code-page char(n)" (`:232`);
  the ATTACH table (`:241-255`, a `client_transcode` row) and the boolean-option list (`:257`); the
  read-path notes. The `mssql_scan_parameterize_filters` row (`:213`) is 091's (its D8).
- **CHANGELOG:** *Changed* (raw-scan values and types; single-byte catalog reads on the wire as
  bytes) and *Added* (the setting and option, and 091's module).
- **Source comments** that describe the cast or the verbatim bytes: `mssql_settings.cpp:131-143`,
  `:585-588`; `mssql_insert_config.hpp` (`:120-123`, `:142-147`); `column_ops.cpp:397-400`,
  `:407-408`, `:441-443`; `mssql_column_info.hpp:145-146`, `:172-173`; `mssql_column_info.cpp:434-435`,
  `:474-476`, `:551-559`; `string_codec.cpp:915-917`; `tds_column_metadata.hpp:65-71`;
  `mssql_functions.cpp:294-301`; `mssql_result_stream.cpp:944-957`;
  `mssql_node_writer_values.cpp:387-390`; `mssql_node_writer_windows.cpp:157-159`.

## Risks

- **A table that differs from the server at hand** silently returns other text than the server's
  `CAST` would. 091's tables come from SQL Server 2025 alone until its live CI sample runs on 2022
  (091 D10, a follow-up). The escape hatch is per catalog.
- **A double-byte raw scan costs more client CPU** (~11.6x the UTF-16 decode, 091 Benchmark):
  correctness over speed where there is no cast to keep (D10). A `CAST(c AS nvarchar(n))` in the
  query restores the server's conversion.
- **x86-64 runs the scalar single-byte kernel** until 091's x86-64 follow-up; D10 gates it on its
  own run.
- **A wrong wire-key derivation** (091 D9) fails a catalog statement by name at the page check
  (D1), and on a raw scan keeps today's bytes and the warning.
- **Users who relied on raw bytes** from a raw scan. Covered by the escape hatch and a CHANGELOG
  *Changed* entry.
- **More pushdown (D8)** means the 079 suites decide it. Every new shape is checked against the
  unpushed result there.
- **Synapse** carries code-page collations through the same TDS but has no test lane here. Fabric
  Warehouse uses UTF-8 collations and sees no change.

## Tasks

1. `mssql_client_transcode` and the `client_transcode` ATTACH option, read at ATTACH; the
   catalog's flag to the read expressions, the three stream construction sites, the RETURNING
   parser, `SQLWriterOptions` -- 1.5 d
2. `ColumnMetadata::code_page` set by the stream; the catalog stream's page check and its named
   error; `FinalizeKernel::CodePage` (`ResolveKernel`, the finalize switch, `DecodeFirstValue`,
   `FinalizeKernelName`, `FINALIZE_KERNEL_COUNT` and its `static_assert`); the per-value arm; the
   DuckDB wrapper (091's former task 6) with its sizing and the column's refusal; the two fuzz
   harnesses' source lists -- 3 d
3. The read expressions with `client_transcode` and no default (`BuildReadExpression`,
   `NeedsNVarcharConversion`, `BuildReturningExpression`), `TranscodeOnClientByDefault`, and all
   their callers -- 1 d
4. Raw-scan typing (`NativeStringType`, `IsDeclaredTypeAmbiguous` for `prepared := true`); the
   narrowed warning; `mssql_convert_varchar_max` semantics -- 1.5 d
5. The 079 writer: the veto lifted under the flag for single-byte pages, the `plain_read`
   consequences (select list, set operations, derived keys, subqueries, ORDER BY under DISTINCT),
   the 079 suites re-run (`test/sql/pushdown/remote_pushdown_*.test`: 13 files, `dml`,
   `dml_agreement` and `merge` among them spec 080's), every newly pushed shape checked against the
   unpushed result, their expectations updated -- 2.5 d
6. Tests: the live oracle per page (16 pages), the PLP split, `char` / `varchar` / `text`, raw scans
   (five functions, both typing paths), vehicles, the `SQL_` set semantics, the page check,
   RETURNING, MERGE, rowid, the escape hatch, warnings; the SQL files and the 11 C++ sites that pin
   today's form updated with both forms -- 5 d
7. Measurement: W1-W5 (W2a included) and W3 reported, three interleaved rounds, server CPU, on
   arm64 and, where at hand, x86-64; the D10 decision -- 2 d
8. Docs -- 1.5 d
9. The chunk pipeline (D12): the module's back-to-back entry with boundaries from the masks, the
   zero-copy chunk, the `CopyAscii` prefix; its unit test, fuzz oracle and benchmark row, each step
   kept only where it beats `DecodeValues` -- 2 d

**Total: 20 days, about 4 weeks.** Spec 091's module is built on this branch, and none of its
follow-ups (091 Tasks, 9.5 days) is needed here on arm64; on x86-64 the single-byte default waits
for 091's x86-64 kernel or for D10's own run there. Against the first revision's 17.5: + 1 d for
the DuckDB wrapper moved from 091, + 2 d for D12, - 0.5 d of measurement with the double-byte
family decided.

**Still to verify:** a value pair that `SQL_Latin1_General_CP1_CI_AS` groups differently as varchar
and as nvarchar (Tests); D10's x86-64 run and, if it fails, the owner's call on a per-build
default; 091's own list.
