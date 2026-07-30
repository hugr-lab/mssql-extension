# Spec 058 — Read framing: one pass over the wire

**Status:** design draft. Nothing here is scheduled until T0 below produces numbers.

**Sequencing:** follows 055 (read staging + batch decode). Independent of 056
(chunk analyzer / dictionary) and 057 (write representation-aware) — it touches
the parser and socket, not the codec or the BCP encoder. `bcp_row_encoder`'s own
checked accessors are explicitly NOT in scope; they belong to 056's
vectorization of that encoder.

---

## 1. Why

Spec 055 made the decode a small term. Per-family live-server numbers after it
are ~20–66 ns/value for the whole read path, of which the batch kernels
themselves are 3–5 ns/value (D10 counters). What remains is dominated by
everything *around* the values: getting bytes off the socket, assembling them
into tokens, and finding where each row ends.

That path currently makes **two passes over every row's bytes and one full copy
of every byte**, neither of which is inherent:

```
recv()          kernel -> TdsSocket::receive_buffer_        (copy 1, unavoidable)
Feed()          receive_buffer_ -> TokenParser::buffer_     (copy 2)
SkipRow()       walk the row to find its length             (pass 1)
StageRow()      walk the same bytes again, staging them     (pass 2 + copy 3)
finalize        staging -> output vector                    (copy 4, Var only)
```

`Feed` exists because a token may straddle a packet boundary and the parser
needs its bytes contiguous; the socket buffer holds whole *frames*, and every
frame carries an 8-byte header in the middle of the byte stream, so the payloads
are not contiguous there.

`SkipRow` exists because the parser must not hand up a row that is only
partially received. `StageRow` depends on that guarantee: it is the reason the
staged walk carries no bounds test per value at all (spec 055 T5), which is
worth keeping.

## 2. What is NOT the problem

**Exceptions are not the cost.** Table-driven EH is zero-cost on the
non-throwing path on every platform this ships to; a `throw` on a cold branch
costs no instruction in the hot one. Converting throws to error codes would buy
nothing by itself.

What DID cost, and what to look for instead, is **out-of-line calls**: a
function that validates and throws cannot inline, so the call itself is the
price. Spec 055's last commit removed two such calls per value
(`duckdb::vector::operator[]` + `duckdb::unique_ptr::operator*` behind
`arena_.Column(c)`) and measured −11.7% on `bigint`, −2 to −3 ns/value across
every family — a constant per value, which is the signature of a fixed call
removed rather than a proportional effect.

A survey of the frame path shows it is already clean of that specific pattern:

| module | checked-accessor calls | throw sites |
| --- | --- | --- |
| `tds_socket` | 0 | 1 |
| `tds_token_parser` | 0 | 1 |
| `tds_row_reader` | 0 | 5 |
| `tds_packet` | 0 | 2 |

So this spec is about **passes and copies**, not about exceptions.

## 3. T0 — the gate (must run first)

**T0a. Fix the instrumentation before trusting it.** The D4/D10 counters are
gated on `MSSQL_DEBUG>=2`, and so is `TDS_PARSER_DEBUG(2, "TryParseNext: …")` —
an `fprintf` per token, i.e. per row, *inside the function the parse-phase timer
wraps*. Every phase number taken at `MSSQL_DEBUG=2` therefore measures the
logging, not the parser. Give the counters their own switch
(`MSSQL_COUNTERS=1`) so they can run with verbose logging off.

An earlier reading of `parse=577 ns/row` vs `process=28 ns/row` came from that
confound and is **withdrawn**. The real split is unknown until T0a lands.

**T0b. Phase split, honestly.** With T0a in place, re-take `parse` / `read` /
`process` for: 1-column BIGINT, 4-column mixed, 15-column wide. Per row and per
value.

**T0c. Profile inside `parse`.** A sampling profile (`sample` on macOS,
`perf record` on the Linux lab) of a large scan, attributing time within
`TryParseNext` between: token dispatch, `SkipRow`'s per-value width walk,
`CompactBuffer`'s memmove, and `Feed`'s copy.

**T0d. Bound each candidate from first principles**, and compare against T0c —
if a candidate's measured share is far from its predicted one, the model is
wrong and the design changes before any code does:

- `Feed` copy: 38.5 MB per 500k rows of a 4-column row (D10 `wire_in`), so
  ~77 B/row; at ~10 GB/s that is **~8 ns/row**. Small. If T0c says otherwise,
  the memcpy is not the reason and something else in `Feed` is.
- `CompactBuffer`: erases `[0, buffer_pos_)` whenever `buffer_pos_` passes both
  half the buffer and 4 KB — so it moves ~S/2, then ~S/4, … per fed block,
  roughly one extra copy of the stream, another **~8 ns/row**. Also small,
  unless the trigger pattern is worse than this model.
- `SkipRow`: **MEASURED, 2026-07-30.** A build that calls `SkipRow` a second
  time and discards the result — no behaviour change, so the delta is exactly
  the marginal cost of one call — against the spec-055 tip, interleaved, 4
  pairs, order rotated, medians ns/value:

  | step | 1x skip | 2x skip | one SkipRow | share |
  | --- | ---: | ---: | ---: | ---: |
  | `read_int` | 20.1 | 22.8 | **2.7** | 13.4% |
  | `read_bigint` | 23.5 | 25.7 | **2.2** | 9.4% |
  | `read_dec38` | 33.8 | 36.9 | **3.1** | 9.2% |
  | `read_str16` | 68.8 | 71.8 | **3.0** | 4.4% |

  A LOWER bound: the second call runs on cache-warm data, so the real first
  call costs more. Single-column fixtures, so ns/value is ns/row; a wide row
  amortizes the per-row call overhead better and should show less per value.

  So `SkipRow` is 4–13% — straddling this spec's own abandon threshold, which
  is why D1 must not be scoped as "obviously worth it". Note the other two
  candidates land in the SAME range once normalized (~8 ns/row on a 4-column
  row is ~2 ns/value), so the case for this spec is the three together
  (~25–30% of a cheap-type read), not any one of them.

**Scope rule:** if T0c says the second walk is under 10% of read CPU, this spec
is not worth doing and should be closed rather than scaled down.

## 4. D1 — skip the skip (the main item)

### What the second walk actually costs

`SkipValue` reads only FRAMING — length prefixes, PLP chunk headers — and steps
over the payload without touching it. So the second walk is **O(values), not
O(bytes)**: a multi-megabyte MAX value is skipped in a couple of header reads.

That is the correct cost model, and it is smaller than "a second pass over the
row" suggests: roughly one extra indirect dispatch plus one prefix load per
value, call it 2–5 ns/value against a whole-read-path cost of ~20–25 ns/value.
**10–20%, which is close enough to the §3 abandon threshold that T0c genuinely
decides whether this ships.**

It also settles the PLP question below: for a MAX-typed column the skip is
already nearly free, so there is nothing there to win.

### The design: one comparison per row replaces the walk

`SkipRow` exists to prove the row is fully received. When no column in the
result set is unbounded, the largest a row can possibly be is a **column-set
constant** — the declared widths plus their framing — computed once after
COLMETADATA. Then:

```
available >= max_row_bytes ?
    yes -> the row is certainly complete: run StageRow exactly as it is today,
           with no per-value test anywhere, and never call SkipRow at all
    no  -> today's path: SkipRow, then stage
```

At a 64 KB receive budget and a ~77-byte maximum row, the `no` branch is one row
in ~800 — the last one in the buffer. The second walk therefore disappears for
substantially every row, at the price of one comparison.

What this design does NOT need, and deliberately avoids:

- **No resume state.** Nothing is remembered between calls; the slow branch is
  today's code, unchanged.
- **No rollback.** Nothing partial is ever staged.
- **No new per-value bounds test.** `StageRow` is untouched — the property spec
  055 built (fixed arms carry no bounds test at all) is preserved by
  construction rather than re-argued.
- **No change for MAX columns.** A result set containing one takes the slow
  branch for every row, exactly as today. Since the skip is nearly free for
  those, that costs nothing.

### What still has to be worked out

- `max_row_bytes` must be an upper bound that is provable from COLMETADATA, not
  from the catalog: an `nvarchar(20)` is at most 40 payload bytes + 2 prefix,
  a `datetime2(7)` at most 8 + 1, and so on. Any type whose bound cannot be
  derived from its metadata makes the whole result set take the slow branch.
- NBC rows add the null-bitmap width to the constant; a NULL only ever makes a
  row shorter, so the bound still holds.
- The comparison needs the parser's remaining-bytes count at the row token,
  which it already has.

### Alternative, if T0c says the dispatch itself is the cost

`SkipValue` switches on `col.type_id` — a ~30-case switch on a different
selector than the staging walk's resolved `AppendArm`, so a straddling row pays
two unrelated indirect jumps per value. Resolving the skip width per column, the
way `ResolveColumnOps` already resolves the append arm, is a much smaller change
than D1 and captures part of the same win. Worth pricing separately.

## 5. D2 — parse without the `Feed` copy

Only if T0c says the copy is worth it (§3 predicts it is not).

Options, cheapest first:

1. **Leave it.** 8 ns/row is not worth a contract change.
2. **recv() payloads straight into the parser buffer**: read the 8-byte header,
   then the payload directly at the parser buffer's tail. Removes the copy;
   costs a second syscall per frame, which at 16 KB frames is a bad trade unless
   the header read is served from an already-buffered socket.
3. **Frame-aware parser** — a reader that knows about frame boundaries and
   handles a split multi-byte read. Removes the copy entirely, and makes every
   read in the parser a branch. Almost certainly a net loss; recorded so it is
   not re-proposed.

## 6. D3 — instrumentation hygiene (lands with T0a)

- `MSSQL_COUNTERS` separate from `MSSQL_DEBUG`.
- The counters' close summary prints the phase split per row AND per value, so
  it is directly comparable with the per-family bench cells.
- A note in the counter block naming which numbers are wall and which are CPU.

## 7. Acceptance

1. Interleaved same-session A/B with an untouched control family; medians over
   ≥3 pairs; per-pass deltas printed. A control that moves invalidates the run.
2. `diff_check.sh` 13/13 byte-identical — this spec changes no value's bytes.
3. Full suite green, including the multi-packet and PLP tests, plus a new test
   that drives the SLOW branch on every row: `mssql_tds_packet_size = 512` makes
   `available` small enough that `max_row_bytes` rarely fits, so the fallback
   gets the same coverage the fast path does. A test that asserts both branches
   return identical bytes for the same query is the real gate here.
4. No regression on the cheap path: `bigint` / `int` within 2%.

## 8. Open questions

- ~~Can the resume point sit inside a PLP chunk list?~~ **Settled**: MAX-typed
  columns keep the two-pass path. Their skip reads a few chunk headers and steps
  over the data, so there is no win to chase, and no resume state is introduced
  anywhere.
- Does a result set mixing one MAX column with twenty bounded ones deserve
  better than "the whole set takes the slow branch"? A per-row bound that adds
  the MAX column's actual declared chunk total is not knowable up front, so
  probably not — but measure a wide table with one `nvarchar(max)` before
  accepting it.
- `CompactBuffer`'s trigger is `pos > size/2 && pos > 4096`. Is a ring buffer
  simpler than the erase, given the parser already carries `buffer_pos_`?
- Does any of this change under TLS, where the socket's own framing already
  copies through the BIO?
