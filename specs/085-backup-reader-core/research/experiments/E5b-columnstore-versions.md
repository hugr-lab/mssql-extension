# E5b: the columnstore format on 2019 and 2022

Date: 2026-11-28. Question (spec 088 Risks, E5 §6.1): E5 decoded the columnstore format on 2025 only. Does its
decoder read the other versions of the matrix, and which of what it takes as constant is not?

## Result

**E5's decoder, unchanged, reads every fixture table of 2019 and 2022 equal to the server's `SELECT`.** That
is 21 tables in three backups per version, 3,743,623 rows and 166 column checks per version, each one equal as a
multiset in E5's canonical form. E5's 168 checks minus `cs_misc`'s `json` and `vector` columns, which are 2025
types. Segment and dictionary versions, encodings per type, dictionary headers, Huffman string pages, the
ARCHIVE container and the delete bitmaps are the same on 2019, 2022 and 2025.

Two things differ, and neither changes a decoded value:

1. **Segment header word 3 is not a constant.** E5 §3 and `cs_decode.Segment` call header words 0-3 "1, 1, 0, 10,
   constant in every segment seen". That holds on 2025: 186 of 186 segments. Elsewhere:

   | version | word 3 |
   |---|---|
   | 2025 | 10 (186) |
   | 2022 | 9 (164), 0 (13), 17799 (6), 0xFFFFFFFF (1) |
   | 2019 | 10 (157), 0 (12) |

   The 0s are on encoding-1 segments of several tables (one 2022 segment is encoding 2). 17799 and 0xFFFFFFFF
   appear on three tables of the 2022 set, in encodings 1 and 3 (`w3_*.txt`). The decoder never reads the
   word.

   Consequence for 088: the rule in its Risks section is "constants not understood are checked against the
   observed values and refused when they differ". It would have refused valid 2022 and 2019 backups. The owner's
   rule since 2026-11-22 is that the reader never refuses a read. So a word the decoding does not use is not
   checked at all. Header words 0-2 (1, 1, 0) and the lob type (3) were constant on all three versions.
2. **Catalog columns:**
   - `syscscolsegments` has no `collation_id`, `min_deep_data` or `max_deep_data` on 2019. They are present on
     2022 and 2025, as E5 §6.1 expected.
   - `syscsdictionaries` and `syscsrowgroups` have the same columns on all three versions.

   D5 (fields read by name, these three optional) covers this.

Rowgroup states differ only in count: 2019 keeps more OPEN rowgroups (status 257, 29 against 7 on 2022 and
2025) after the same batches; TOMBSTONE (260) is 1 on every version. They are read the same way. A likely cause
is a parallel load splitting its rows into delta stores (E11 measured that on 2022); not checked.

## Not answered here

- **2017.** Its image crashes at start under Rosetta (sqlservr dumps core) on the M4 host, so it runs in the
  oracle lane, on x86_64.
- **E5 §5's cells "not produced on this server"** are not produced on 2019 or 2022 either:
  - nvarchar, UTF-8 varchar and `(max)` under encoding 5;
  - a non-string secondary dictionary with entries.

  Encoding 5 went to the same types on every version: char, varchar(n), binary, varbinary(n), decimal,
  uniqueidentifier and datetimeoffset (`enc_*.txt`). A fixture that forces them, for example values too long or too varied for a
  dictionary, is 088's task 8.
- **`json` and `vector` in a CCI** are 2025 only, as are the columns E5 checked.

## Procedure

`design/001-mssql-backup-reader/experiments/E5b/` is gitignored, like E5.

- `prep.py VER` copies E5's batches. Below 2022 it rewrites `GENERATE_SERIES` as an inline TVF that the driver
  creates in `model`, and drops ordered CCI's `ORDER (k)` (ordered CCI is a 2022 feature, so `cs_ord` is a plain
  CCI there). Below 2019 it would use a non-UTF-8 `_SC` collation, and below 2025 it drops `cs_misc`'s json and
  vector columns.
- The steps E5 ran by hand are written out:
  - `e2_runs`' four chunks, from its comment;
  - `e3_del`, reconstructed from E5's counts: 300,000 rows in two bulk chunks, `id % 5 IN (1, 2)` deleted, 300
    rows updated, giving 120,300 bitmap entries, 179,700 compressed rows and 300 delta rows, as E5 had.
- `run.sh VER PORT` runs the batches on a throwaway container of the pinned image (`test/backup/gen/images.txt`),
  with `mssql_query_timeout = 0` (11_dict2's REBUILD needs more than 30 s under Rosetta). It then takes E5's live
  oracle (`truth_rows.py`), the three backups and `cs_decode.py verify`.
- `survey.py`, `word3.py` and `enc_types.py` tally the header words, versions, encodings per type and catalog
  columns. Their outputs are kept beside them, and the backups' SHA-1s are in `E5b/SHA1SUMS`.

Servers: 2019 RTM-CU32-GDR (15.0.4500.2), 2022 RTM-CU27-GDR (16.0.4305.5), both linux/amd64 under Rosetta.
