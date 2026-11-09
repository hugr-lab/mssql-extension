# E13 — ordered passes, and finding a page in a compressed backup without walking its chain

2026-11-09, on the local samples (SQL Server 2025, 17.0; the tools are in
`design/001-mssql-backup-reader/experiments/E13/`, git-ignored with the samples). Question: must the
open of a compressed backup walk its whole block chain (a header per ≤ 64 KB block, which on object
storage reads most of the file), and must a striped set be read whole for its page headers (085 D4a,
§ Risks)? Or can a page be found by searching the file, because a pass is ordered?

## Results

| # | finding | evidence |
|---|---|---|
| 1 | **Every data pass is strictly ascending by (file, extent)**, counting the groups that name an extent: pass 1 and pass 2, fulls and differentials, plain and compressed, and each family of a striped set on its own | 210 of 210 passes in 105 files (81,391 + 8,844 extents), 0 breaks (`order.cpp`) |
| 2 | **In a container, every block inside a data pass outputs exactly 64 KB and starts on a unit boundary**: pass units start at logical 8192, so block *k* of a pass is its extent group *k*, one extent per block | 16 containers (15 ZSTD, 1 MS_XPRESS), every pass: odd-sized blocks inside a pass 0, unit phases 1 (always 0) (`e13_chain.cpp`) |
| 3 | The only blocks that are not 64 KB are the first (8 KB, the MTF header region) and the 4 KB ones between passes and at the trailer; **the seek index after the chain names exactly those boundaries** (each pass's end and the trailer), so a pass's logical extent is known from the seek index alone | same |
| 4 | The block header has no block number and no logical offset; bytes 24–27 are not CRC32, CRC32C or Adler-32 of the payload or the header (unidentified); bytes 4–23 are nearly constant within a file (1–5 distinct values) | `e13_chain.cpp`, Python check on FC1, r0, FC3 |
| 5 | **A block can be found from any file offset**: scan forward to the first offset whose header and the three headers its chain leads to are plausible (kind 0x21 / 0x23, 1–128 units, a stored block's length = its output, inside the chain), and take the **fourth** header. A false start meets the true chain within a header or two, so the fourth header is always a true block | 3.6 M probes (every 251 bytes of 14 chains) + 13,304 on the XPRESS probe: 0 wrong blocks with the fourth header; with the second, up to 8 %; scanned 2–21 KB on average, at most 64 KB (`e13_resync.cpp`) |
| 6 | **Searching works**: bisection over the file by extent order (probe = resync + decode one block), samples kept, and one read for the last ≤ 1 MiB, finds every extent of pass 1 and shows absent ones absent | 16 containers, 0 wrong answers. Cold: 6–8 requests and ~1 MB a lookup (3.6 on the 3 MB probe); warm (samples kept, random order): 0.4–1.9; 64 targets spread evenly, ascending: 1.1–2.6 (`e13_search.cpp`) |

Interpolation on the (file, extent) key was worse than bisection cold (the key jumps between files);
an interpolation on the block index is what the rank path should use (below).

## What follows for 085

- **A compressed backup need not walk its chain at open.** Pass boundaries come from the seek index;
  a page's block is found by searching (bisection, samples kept). On the rank path (a single-family
  full, E12) the extent's rank gives its block index exactly (finding 2: block = rank + 1 in pass 1),
  so the search is an interpolation on the block index, corrected by each probe's own extent.
- **A striped set need not be read whole.** Each family's pass is ascending (finding 1), and in a
  plain family file group *g* is at a computed offset, so a bisection over groups finds an extent in
  each family in log₂(groups) reads of 64 KB.
- **Order is checked, not trusted**: a probe that finds a key out of order with its neighbours'
  samples refuses the search for that pass, which then falls back to its header scan (D4a: a loss of
  speed, never of correctness).

## Extrapolated to 10 TB (not measured)

- Cold lookup in a 3 TB compressed file: log₂(3 TB / 1 MiB) ≈ 22 probes of 80–260 KB, then a 1 MiB read.
- The open's 2,560 GAMs in ascending order: by bisection with samples ≈ 11 requests each (28 k
  requests, ~5 GB); by interpolation on the block index, 2–3 each (~7 k requests, ~1 GB). Either is
  two to three orders below the chain walk (≈ 3 TB).

## Open

- The other versions (CI matrix, D14): order (finding 1) and the 64 KB block (finding 2) are 2025's.
- Encrypted containers (E4, spec 087): not looked at.
- A busy backup's pass 2 is in the samples (E6 F2 / F3 / X0: 1,352–1,385 blocks); a pass 2 of
  terabytes was not.
