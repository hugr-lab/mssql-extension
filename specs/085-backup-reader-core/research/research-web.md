# Reading SQL Server .bak files without SQL Server: web research

Date: 2026-11-05. This is research only. "RE" means reverse-engineered; "IV" means Internals Viewer. Repository dates come from the GitHub API.

## Summary

- **No DuckDB extension reads .bak files natively.**
  - The community list has `duckbridge`, `mssql` and `mssql_ducklake` ([list](https://github.com/duckdb/community-extensions/tree/main/extensions)).
  - DuckBridge needs a SQL Server. `src/mssql/backup.rs` issues `BACKUP DATABASE`, `RESTORE FILELISTONLY` and `RESTORE DATABASE`. A grep of the whole repository finds no MTF or page parser ([duckbridge](https://github.com/singhpratech/duckbridge)).
- **Uncompressed, unencrypted full backups are solved twice, independently:**
  - [StefanMaron/BusinessCentral.DbReader](https://github.com/StefanMaron/BusinessCentral.DbReader): C#, **MIT**, clean-room, with every fact sourced in `PROVENANCE.md` and byte-validated against RESTORE.
  - [danny-sg/internals-viewer](https://github.com/danny-sg/internals-viewer): C#, **GPL-3.0**, active. It also reads **MS_XPRESS and ZSTD compressed** backups and decodes **columnstore**.
- **Unsolved in public:** backup encryption, TDE, QAT_DEFLATE framing, the SQL Server 2025 `json` binary format, and log replay.

## 1. Prior art

| Project | Lang / licence / last push | What it reads |
|---|---|---|
| [BusinessCentral.DbReader](https://github.com/StefanMaron/BusinessCentral.DbReader) | C#, MIT, 2026-10 | Full .bak and .bacpac; single data file. ROW/PAGE compression, LOB and overflow. Refuses `money`, `sql_variant`, `xml`, sparse, compressed and encrypted backups. No diff/log; no log replay. |
| [internals-viewer](https://github.com/danny-sg/internals-viewer) | C#, GPL-3.0, 2026-11 | Live server, MDF, full .bak (striped too). MS_XPRESS and ZSTD backups. Compressed records, sparse, columnstore incl. archive. Refuses encrypted backups. |
| [OrcaMDF](https://github.com/improvedk/OrcaMDF) | C#, GPL-3.0, last commit 2015 | MDF only, 2008 R2. Row compression yes, page compression no. LOB, sparse, sql_variant. |
| [mtf-rs](https://github.com/rroohhh/mtf-rs) + [mdf-rs](https://github.com/rroohhh/mdf-rs) | Rust, AGPL-3.0, 2021 | Uncompressed .bak, LOB. No compression or sparse. |
| [oxidized-mdf](https://github.com/schrieveslaach/oxidized-mdf) | Rust, GPL-3.0, 2021 | MDF. The only relevant crate on crates.io. |
| [baktoany](https://github.com/f-alotaibi/baktoany) | Go, MIT badge but **no LICENSE file**, 2026-10 | Naive: first image of a page wins, all temporal types treated as datetime. Its design mirrors mtf-rs. |
| [mssql-backup-checker](https://github.com/drexlma/mssql-backup-checker) | Go, no licence, 2026-09 | Triage: detects `MSSQLBAK` (compressed) and encrypted flags, carves pages into an MDF. |
| [SQLServerForensics](https://github.com/aarsakian/SQLServerForensics) + [MTF_Reader](https://github.com/aarsakian/MTF_Reader) | Go, AGPL **or** Apache+Commons Clause / none, 2026-10 | Extracts a .bak to MDF; MDF tables, LDF log records, carving. |
| [pfak/mssql-mdf-parser](https://github.com/pfak/mssql-mdf-parser) | Python, MIT, one commit 2026-07 | MDF from SQL Server 2000–2008 R2 only. No off-row LOBs, no compression, no .bak. |
| [serbod/DBReader](https://github.com/serbod/DBReader) | Pascal, MIT | Has MTF and MDF readers (not evaluated). |
| [go-mtf](https://pkg.go.dev/github.com/pbs-plus/go-mtf), [mtftar](https://github.com/sjmurdoch/mtftar), [mtf](https://github.com/KyleBruene/mtf) | MIT / GPL-2 / GPL-2 | Generic NTBackup MTF; not SQL Server-aware. |

**Commercial (reference only):**

- [Redgate SQL Data Compare](https://documentation.red-gate.com/sdc/working-with-other-data-sources/working-with-backups) reads full plus differential backups. It does **not** read compressed backups (2012+), encrypted backups or TDE, and "doesn't read the log records".
- [ApexSQL Recover](https://www.quest.com/sql-tools-recover/) reads compressed backups but not encrypted ones.
- [Stellar](https://www.stellarinfo.com/database-recovery/sql-backup.php) claims full, differential and log backups, but needs a running SQL Server.

**Licence position for an MIT project:**

| Status | Projects | Use |
|---|---|---|
| MIT | DbReader, pfak, go-mtf, pyxca, rust-lzxpress | Usable with attribution. |
| GPL / AGPL / Commons Clause | OrcaMDF, IV, mdf-rs, mtf-rs, mtftar, SQLServerForensics | Clean-room understanding only. |
| No licence | baktoany, MTF_Reader, bakcheck | Read-only reference. |

## 2. The MTF container

- **Spec:** MTF 1.00a rev 1.8 (1998). The original `laytongraphics.com/mtf/MTF_100a.PDF` is dead ([Wikipedia](https://en.wikipedia.org/wiki/Microsoft_Tape_Format)); a copy is on [Scribd](https://www.scribd.com/doc/112780281/MTF-100a).
- **Descriptor blocks (DBLK):**
  - 52-byte common header: type[4], u32 attributes, u16 offset to the first stream at +8, then a checksum.
  - The checksum is an XOR of 16-bit words that must come to 0. **Validate it:** the string "VOLB" occurs inside data and caused an infinite loop ([MTF_Reader PR #1](https://github.com/aarsakian/MTF_Reader/pull/1)).
- **Stream header (22 bytes):** id[4], two u16 attribute fields, u64 length, u16 encryption algorithm, u16 compression algorithm, u16 checksum. Streams are 4-byte aligned.
- **SQL Server's use** ([DbReader PROVENANCE](https://github.com/StefanMaron/BusinessCentral.DbReader/blob/main/PROVENANCE.md); IV and bakcheck agree):
  - DBLK sequence: `TAPE SFMB SSET VOLB MSCI MSDA×2 MSTL×2 MSLS … ESET`.
  - Streams: **MQCI** (configuration), **MQDA** (data pages), **MQTL** (log), **APAD** (pad so the payload starts near 4 KB), **SPAD** (pad, ends the chain).
  - **SFIN was observed by no one.** MSLS is unexplained.
- **Page layout:**
  - MQDA/MQTL payload: **2 unexplained bytes (0x0000) + N×8 KB**.
  - **Region 1:** allocated extents in ascending order per GAM interval. Padded to 1 MB with filler pages of type 0x65.
  - **Region 2:** a re-dump of the extents changed during the backup, plus the final DCM image.
  - RESTORE places pages **positionally**; "last page header wins" mis-mapped 29 pages that had stale headers (DbReader).
  - **IV indexes pages by their headers instead, and the two models disagree.**
- **What Microsoft documents:** that backups use MTF, and the media-header contents ([media sets](https://learn.microsoft.com/en-us/sql/relational-databases/backup-restore/media-sets-media-families-and-backup-sets-sql-server)). [RESTORE HEADERONLY](https://learn.microsoft.com/en-us/sql/t-sql/statements/restore-statements-headeronly-transact-sql) names the stored fields (FirstLSN, LastLSN, CheckpointLSN, DifferentialBaseLSN/GUID, BackupSetGUID, CompressionAlgorithm, KeyAlgorithm, EncryptorThumbprint, …). Their byte layout in SSET/MQCI is RE; IV parses it, under GPL.

## 3. Backup compression

- **The file starts with `MSSQLBAK`, not `TAPE`** ([bakcheck](https://github.com/drexlma/mssql-backup-checker); IV `CompressedBackupFormat.cs`).
- **Container:**
  - 480-byte file header; algorithm at +12 (**3 = MS_XPRESS, 131 = ZSTD**).
  - Chunks with a 28-byte header: marker 0x21 (compressed) or 0x24 (raw, 512 B); decoded size in 512-byte units; payload size; checksum at +24.
  - Each chunk decodes to ≤ 64 KB of the **inner MTF stream**.
- **MS_XPRESS = [MS-XCA](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-xca/a8b7cb0a-92a6-4187-a23b-5e14273b96f8) LZ77+Huffman, with three twists:**
  - Symbol 256 is not end-of-stream.
  - A block ends at its declared size.
  - **The match window spans chunks**, so decoding is sequential (checkpoints give random access).
  - MIT codecs to adapt: [pyxca](https://github.com/jborean93/pyxca) (C), [rust-lzxpress](https://github.com/MagnetForensics/rust-lzxpress).
- **ZSTD** ([2025](https://learn.microsoft.com/en-us/sql/database-engine/configure-windows/view-or-configure-the-backup-compression-algorithm-server-configuration-option)): every chunk is a self-contained frame. DuckDB bundles zstd.
- **QAT_DEFLATE** (2022, Windows): uses QATzip, with ISA-L in software mode; restore needs the driver, not the hardware ([MS](https://learn.microsoft.com/en-us/sql/relational-databases/integrated-acceleration/use-integrated-acceleration-and-offloading)). The codec is DEFLATE; **the framing is undocumented, with no RE found.**

## 4. Backup encryption

- **Documented** ([MS](https://learn.microsoft.com/en-us/sql/relational-databases/backup-restore/backup-encryption)):
  - AES-128/192/256 or 3DES.
  - Encryptor: a server certificate, or an asymmetric key held only in EKM. An EKM key cannot be exported, so such backups can never be read offline.
- **What a reader would need:** the certificate's private key, exported with [BACKUP CERTIFICATE … WITH PRIVATE KEY](https://learn.microsoft.com/en-us/sql/t-sql/statements/backup-certificate-transact-sql). That writes a DER `.cer` plus a **PVK** file (RC4; OpenSSL reads it), or `FORMAT='PFX'` since 2022.
- **Unknown:** how the per-backup key and IV are wrapped, and where they are stored. Every tool refuses encrypted backups.

## 5. TDE

- **Documented** ([MS](https://learn.microsoft.com/en-us/sql/relational-databases/security/encryption/transparent-data-encryption)):
  - Page-level encryption on write, AES or 3DES, page size unchanged.
  - The DEK is in the boot record, protected by a certificate in master (or EKM).
  - **Backups contain the encrypted pages.**
- **The boot page holds `FCBFileDEK`** (algorithm id, bit length, thumbprint) ([dbaglobe](https://www.dbaglobe.com/2012/08/where-dek-in-sql-tde-stored-and-when-it.html)), so part of it is plaintext.
- **The log is encrypted per VLF**, with the encrypted DEK in each VLF header ([MS blog](https://learn.microsoft.com/en-us/archive/blogs/sqlsecurity/dek-and-the-log)).
- [McAuliffe](https://medium.com/@s.mcauliffe_17464/the-anatomy-and-in-security-of-microsoft-sql-server-transparent-data-encryption-tde-or-how-to-d164eb08564) broke the key hierarchy, but recovered the data by restoring on a server, not by decrypting pages offline (seen through the article's summary only).
- **Unknown:** cipher mode, IV derivation, which page bytes stay plaintext, and the DEK blob format.

## 6. On-disk structures

- **Page header** ([Randal](https://www.sqlskills.com/blogs/paul/inside-the-storage-engine-anatomy-of-a-page/), [OrcaMDF](https://github.com/improvedk/OrcaMDF/blob/master/src/OrcaMDF.Core/Engine/Pages/PageHeader.cs)):
  - Offsets: +1 type, +2 typeFlagBits (0x80 = has a compression-info structure), +3 level, +4 flagBits, +6 indexId, +16 nextPage, +22 slotCnt, +24 objId, +32 pageId, +36 fileId, +40 LSN, +58 ghostRecCnt.
  - objId goes stale after a shrink, so **map pages through IAM chains**.
- **Page types:**

  | Type | Meaning |
  |---|---|
  | 1 | data |
  | 2 | index |
  | 3, 4 | text mix / text tree |
  | 8 | GAM |
  | 9 | SGAM |
  | 10 | IAM |
  | 11 | PFS |
  | 13 | boot (1:9) |
  | 15 | file header |
  | 16 | DCM |
  | 17 | BCM |

- **Allocation:**
  - A GAM interval covers 63,904 extents.
  - A PFS page every 8,088 pages; bit 0x40 marks a page allocated. Stale deallocated pages exist, so check PFS.
  - The boot page has the pointer to the first `sysallocunits` page at +612.
- **Records** ([Randal](https://www.sqlskills.com/blogs/paul/inside-the-storage-engine-anatomy-of-a-record/)): FixedVar — status bits, fixed data, column count, null bitmap, variable-offset array.
- **ROW compression (CD format)** ([MS row](https://learn.microsoft.com/en-us/sql/relational-databases/data-compression/row-compression-implementation) and [page](https://learn.microsoft.com/en-us/sql/relational-databases/data-compression/page-compression-implementation) docs are conceptual; byte layouts from DbReader):
  - A 4-bit code per column: 0 NULL, 1 EMPTY, 2–9 short data, 0xA long, 0xB bit=1, 0xC dictionary symbol.
  - Key columns are stored first; columns come in clusters of 30.
  - Integers are big-endian, sign-biased and trimmed; decimals use vardecimal; Unicode compression is **SCSU**.
- **PAGE compression:** a CI structure at +96 holds the anchor record and the dictionary.
- **Sparse vector** ([improve.dk](https://improve.dk/sparse-column-storage-ndash-the-sparse-vector/)): marker 5, then a count, column ids, offsets and data. Sparse columns exclude data compression.
- **LOB:** a 16-byte text pointer (text/ntext/image), or an in-row root of 12+12n bytes (MAX types, overflow), leading to SMALL / LARGE_ROOT / INTERNAL / DATA records.
- **Catalog:** Microsoft [lists the base tables](https://learn.microsoft.com/en-us/sql/relational-databases/system-tables/system-base-tables) by name only ("subject to change", DAC-only); `sysrscols` is not even listed. The layouts are RE (OrcaMDF, DbReader). Further references: Delaney's *SQL Server Internals* and the [improve.dk](https://improve.dk/introducing-orcamdf/) series.

## 7. Types on disk compared with TDS

- **Same bytes as TDS:**
  - Integers, float, uniqueidentifier.
  - date, time, datetime2, datetimeoffset.
  - nvarchar (UTF-16LE); varchar in its code page, or UTF-8 under `_UTF8` collations (matches TDS with UTF8SUPPORT).
  - decimal (sign byte + little-endian magnitude).
- **Different:**
  - **datetime** is ticks then days (TDS: days first).
  - **money** is a plain int64 (TDS: high dword first).
  - **bit** is packed in the fixed area.
  - **All compressed values** use the encodings in §6.
- geometry, geography and hierarchyid: [MS-SSCLRT](https://learn.microsoft.com/en-us/openspecs/sql_server_protocols/ms-ssclrt/77460aa9-8c2f-4449-a65e-1d649ebd77fa), same payload as on the wire. User CLR UDTs are opaque.
- xml: probably [MS-BINXML](https://learn.microsoft.com/en-us/openspecs/sql_server_protocols/ms-binxml/11ab6e8d-2472-44d1-a9e6-bddf000e12f6), to verify.
- `json` (2025): binary, strings UTF-8, **undocumented** ([docs](https://learn.microsoft.com/en-us/sql/t-sql/data-types/json-data-type)).
- `vector` (2025): binary float32 ([docs](https://learn.microsoft.com/en-us/sql/t-sql/data-types/vector-data-type)); the wire form is in MS-TDS, the disk form is unverified.
- `sql_variant`: RE in OrcaMDF.

## 8. Columnstore

- **Structure:**
  - Rowgroups of up to about 1M rows; one segment LOB per column, plus dictionary LOBs.
  - `encoding_type` 1–5 ([sys.column_store_segments](https://learn.microsoft.com/en-us/sql/relational-databases/system-catalog-views/sys-column-store-segments-transact-sql)).
  - Encodings: value-based or dictionary, RLE (runs of 64 or more) plus bit-packing, and Huffman-coded strings.
  - Delta stores and the deleted bitmap are rowstore B-trees.
  - Sources: [Larson et al.](https://www.microsoft.com/en-us/research/wp-content/uploads/2013/06/Apollo3-Sigmod-2013-final.pdf), [rusanu](https://rusanu.com/2012/05/29/inside-the-sql-server-2012-columnstore-indexes/).
- COLUMNSTORE_ARCHIVE "runs the Microsoft XPRESS compression algorithm" ([MS](https://learn.microsoft.com/en-us/sql/relational-databases/data-compression/data-compression)); in IV's code it is Xpress Huffman.
- **The segment format is undocumented. The only decoder is IV's, under GPL.** Feasible, but only clean-room, and costly.

## 9. Consistency of a full backup

- **Log range:** from MIN(checkpoint, oldest active transaction, oldest replicated transaction) to the end of the data copy. Restore runs REDO, then UNDO ([Randal](https://www.sqlskills.com/blogs/paul/more-on-how-much-transaction-log-a-full-backup-includes/), [myths](https://www.sqlskills.com/blogs/paul/debunking-a-couple-of-myths-around-full-database-backups/)).
- **Without replay, measured by DbReader on quiet databases:** the log tail was 64 KB. Only allocation pages and the catalog rows of objects created during the backup differed; no user rows differed.
- **On a busy database, expect:**
  - Uncommitted rows visible.
  - Committed changes missing.
  - Torn page splits: a row duplicated or missing.
  - Half-updated LOB trees.
- **Shortcuts:**
  1. Report the log size and the FirstLSN–LastLSN window.
  2. Flag a table *fuzzy* when any of its pages has an LSN above CheckpointLSN.
  3. List open transactions in MQTL.
  4. Later, redo row operations.

## 10. Differential backups

- **Contents:** the extents with a DCM bit set since the base, plus a log tail ([MS](https://learn.microsoft.com/en-us/sql/relational-databases/backup-restore/differential-backups-sql-server)). Differentials are cumulative.
- **Overlay:** the differential's extents supersede the base's; its GAM/IAM/PFS decide which pages are live.
- **Validation:** `DifferentialBaseGUID` = the base's `BackupSetGUID`, `DifferentialBaseLSN` = the base's FirstLSN, and FamilyGUID must match.
- **Prior art:** Redgate (commercial).

## 11. Log backups

- **Physical format:** log blocks are 512-byte multiples up to 60 KB, with a 72-byte header and **per-sector parity bytes** ([DFRWS 2023](https://dfrws.org/wp-content/uploads/2024/04/Forensic-analysis-of-SQL-server-transaction_2023_Forensic-Science-Internatio.pdf)).
- **Records** are known only through the undocumented `fn_dump_dblog` ([Randal](https://www.sqlskills.com/blogs/paul/using-fn_dblog-fn_dump_dblog-and-restoring-with-stopbeforemark-to-an-lsn/)):
  - INSERT/DELETE_ROWS carry full row images.
  - **MODIFY_ROW/MODIFY_COLUMNS carry byte-range diffs.**
- **So row-level CDC needs page state, which means implementing redo** on top of full + differential. Feasible, but large.
- **Readers:**

  | Project | Lang / licence | Notes |
  |---|---|---|
  | [SqlLogExplorer](https://github.com/rudi-bruchez/SqlLogExplorer) | C#, MIT | Needs `fn_dump_dblog` (LocalDB). |
  | [samaruk](https://github.com/samaruk/SQL-Server-Log-Reader) | C#, no licence | Native `.ldf`; no compression or LOB. |
  | [dbforensic](https://github.com/dbforensic/sql-transaction-log-parser) | Python, MIT | `.ldf` parsing. |
  | SQLServerForensics | AGPL | LDF parsing. |

## Feasibility

| Component | Status | Prior art | Risk | Effort |
|---|---|---|---|---|
| MTF walk + page index | Spec documented; SQL use RE | DbReader, IV | Low–Med | 2–3 wk |
| Backup header (LSNs, GUIDs) | RE (HEADERONLY is the oracle) | IV | Med | 1–2 wk |
| MS_XPRESS / ZSTD | Codec documented; framing RE | IV, pyxca | Low–Med | 1–2 wk |
| QAT_DEFLATE | Unknown framing | none | Med | 1 wk after samples |
| Backup encryption | Unknown | none | High | open-ended |
| TDE | Page scheme unknown | none | High | 1–2 months |
| Catalog → schema | RE | OrcaMDF, DbReader | Med (version drift) | 3–4 wk |
| Rows, B-trees, ROW/PAGE compression, LOB, sparse | RE | DbReader | Med | 5–6 wk |
| Scalar + spatial + xml types | Documented / same as TDS | mssql codec | Low–Med | 3 wk |
| json (2025) | Undocumented | none | High | unknown |
| Columnstore | RE (GPL only) | IV | High | 6–10 wk |
| Fuzzy flags (no replay) | Documented semantics | DbReader | Med | 1 wk |
| Differential overlay | RE + docs | Redgate | Med | 1–2 wk |
| Log redo / log backups → CDC | Community RE | log readers | High | 3–6 months |

A realistic v1 covers full and differential backups, uncompressed or MS_XPRESS/ZSTD, rowstore with ROW/PAGE compression and LOBs, and no log replay. Estimate: about 3–4 months.

## Biggest unknowns and the experiment for each

1. **Page placement under load and with multiple files** (DbReader's positional model against IV's header model). Back up under an OLTP load, RESTORE and take the database OFFLINE, then byte-compare where each model places every page.
2. **Header byte layout.** Take backups with known HEADERONLY values; search for the LSN triples and GUIDs on 2016, 2019, 2022 and 2025.
3. **MS_XPRESS across versions** (Redgate hints that 2008 R2 differs). Decompress each version's backups; the result must parse as MTF and equal an uncompressed backup.
4. **QAT_DEFLATE.** In a Windows VM with ISA-L in software mode, back up, then try raw/zlib/gzip inflate on a chunk.
5. **TDE.** Export the certificate as PFX and RSA-decrypt the boot-page DEK. Diff encrypted pages against plaintext ones and try AES-CBC/XTS with IV candidates (page id, LSN).
6. **The error without replay.** Run a write workload during BACKUP and compare the reader's output with a restored SELECT; check that the LSN flag catches every differing table.
7. **json and xml.** Write controlled values, inspect them with DBCC PAGE, and parse them against the specs.
8. **Catalog drift.** Restore a type-probe database on every version and diff the syscolpars/sysrscols layouts.
