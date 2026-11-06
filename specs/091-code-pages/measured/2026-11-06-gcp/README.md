# Code-page kernels on seven CPUs, 2026-11-06

`tools/code_pages/bench_gcp.sh` ran `tools/code_pages/bench_remote.sh` on Google Cloud Spot VMs
(us-central1-a, Ubuntu 24.04, g++ 13.3, `-O3 -DNDEBUG`). Every VM was deleted after its run, so
this file and the raw outputs beside it are the record.

Each `<cpu>.txt` holds the following, from the tree of #528 at `c99d502` (`BENCH_ALIGN_OFFSETS="0 1"`):
- `lscpu`'s model and the flags the kernels choose by;
- the unit test's verdict for every instruction set the CPU has (all passed);
- `bench_code_page` for the best instruction set, with the payload 0 and 1 bytes past a 64-byte boundary;
- `bench_code_page` again for each lower instruction set.

The cleaned-up commit after it, which went back to aligned `std::vector`s, only changes how the
tables are allocated, not where they land.

The workloads are 2048 values a chunk, as `test/cpp/bench_code_page.cpp` generates them. The
columns:
- `column`: `code_page::DecodeColumn`, the code-page bytes to UTF-8 plus every `string_t`;
- `today`: a copy of today's NVARCHAR read path. The UTF-16 the server sends after its CAST, staged with a U+0000 after each value, then one simdutf call, the delimiter walk and every `string_t`. Its views are checked against `column`'s.
- `direct`: the kernel alone over the whole payload;
- `utf16`: simdutf's conversion alone.

| file | GCP type | CPU | instruction sets run |
|---|---|---|---|
| zen3.txt | n2d-standard-2, min AMD Milan | AMD EPYC 7B13 (Zen 3) | avx2, scalar |
| zen4.txt | c3d-standard-4 | AMD EPYC 9B14 (Zen 4) | avx512, avx2, scalar |
| zen5.txt | c4d-standard-2 | AMD EPYC 9B45 (Zen 5) | avx512, avx2, scalar |
| ice-lake.txt | n2-standard-2, min Intel Ice Lake | Intel Xeon @ 2.60GHz (Ice Lake) | avx512, avx2, scalar |
| sapphire-rapids.txt | c3-standard-4 | Intel Xeon Platinum 8481C | avx512, avx2, scalar |
| ampere.txt | t2a-standard-2 | Ampere Altra (Neoverse N1) | neon, scalar |
| axion.txt | c4a-standard-2 | Google Axion (Neoverse V2) | neon, scalar |

## `today` / `column`: best instruction set, payload 64-byte aligned

Higher means `DecodeColumn` is faster.

| CPU | ISA | ascii 4-20 B | ascii 200 B | latin 1252 | cyrillic 1251 | 1251 + typography | 1252 + typography | thai 874 |
|---|---|---|---|---|---|---|---|---|
| Zen 3 | AVX2 | 1.76x | 1.34x | 1.41x | 2.24x | 1.94x | 1.11x | 2.11x |
| Zen 4 | AVX-512 | 1.62x | 1.16x | 1.28x | 1.84x | 1.99x | 1.56x | 3.05x |
| Zen 5 | AVX-512 | 1.73x | 1.56x | 1.41x | 2.13x | 2.20x | 1.63x | 3.05x |
| Ice Lake | AVX-512 | 1.28x | 1.48x | 1.30x | 1.97x | 2.53x | 1.79x | 2.76x |
| Sapphire Rapids | AVX-512 | 1.48x | 1.29x | 1.31x | 2.02x | 2.72x | 1.90x | 2.84x |
| Neoverse N1 | NEON | 1.84x | 3.15x | 1.84x | 2.14x | 1.68x | 1.89x | 2.02x |
| Neoverse V2 | NEON | 1.79x | 2.14x | 1.36x | 2.15x | 1.90x | 1.13x | 2.04x |

## AVX2: `vpgatherdd` → `vpshufb` tables

`today` / `column` with AVX2, from the runs before and after `5b21351`. Those raw outputs are
not kept; the "after" column matches this run's AVX2 tables within noise.

| CPU (AVX2) | 1251 + typography | 1252 + typography |
|---|---|---|
| Zen 3 | 0.92 → 1.92 | 0.47 → 1.10 |
| Zen 4 | 0.59 → 1.36 | 0.47 → 1.17 |
| Zen 5 | 0.41 → 1.49 | 0.32 → 1.21 |
| Ice Lake | 0.66 → 1.67 | 0.47 → 1.38 |
| Sapphire Rapids | 1.38 → 1.65 | 0.95 → 1.38 |

The kernel alone (`direct`) went from 31-43 ns a value to 5.6-14 on typography.

## The payload 1 byte off a 64-byte boundary: time(1) / time(0), `column`

| CPU | ascii 4-20 B | ascii 200 B | the other workloads |
|---|---|---|---|
| Zen 3 (AVX2) | 1.02 | 1.10 | 0.95-1.03 |
| Zen 4 (AVX-512) | 1.00 | 1.15 | 1.00-1.01 |
| Zen 5 (AVX-512) | 1.07 | 1.23 | 1.00-1.01 |
| Ice Lake (AVX-512) | 1.02 | 1.09 | 1.01-1.03 |
| Sapphire Rapids (AVX-512) | 1.01 | 1.05 | 1.02-1.04 |
| Neoverse N1 | 1.02 | 1.10 | 1.02-1.13 |
| Neoverse V2 | 1.01 | 1.18 | 1.01-1.03 |

`DecodeColumn` walks the payload from its start in blocks that are multiples of 64, so the staging
buffer's own alignment decides this. Spec 092 D12 therefore allocates that buffer 64-byte aligned.

## Against the production function itself (Apple M4)

`make bench-code-page-column` links libduckdb and runs the production
`codec::string::DecodeChunkFromStaging` on the same text. Like this run, it is
`today` / `column`, so higher means faster:

| text | 4-16 B | 20-60 B | 200-1000 B |
|---|---|---|---|
| ascii | 1.87x | 1.38x | 2.46x |
| latin 1252 | 1.70x | 1.36x | 2.63x |
| 1252 + typography | 1.53x | 1.19x | 1.86x |
| cyrillic 1251 | 1.61x | 1.57x | 1.63x |
| 1251 + typography | 1.40x | 1.25x | **0.85-1.35x** |
| thai 874 | 1.87x | 2.04x | 1.44x |

**The one cell that can be slower than today:** long 1251 text with dense typography on NEON.
Whether it is depends on the text: the NEON kernel's per-half choice between `Pack2` and `Pack3` is
data-dependent there. Branch-free, sticky-tier and one-decision-per-step variants were all slower.

The two tables measure different things, so their numbers are not directly comparable:
- the seven-CPU table is `column` against the `today` copy on GCP;
- this one is against the production function on M4.

## Reproduce

```
tools/code_pages/bench_gcp.sh <gcp-project> <results-dir>     # the seven Spot VMs, ~15 min
BENCH_ALIGN_OFFSETS="0 1" tools/code_pages/bench_gcp.sh <gcp-project> <results-dir>
make bench-code-page-column                                   # the production function, needs a release build
```
