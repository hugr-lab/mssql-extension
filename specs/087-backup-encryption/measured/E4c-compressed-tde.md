# E4c: a TDE database's compressed backup (2026-11-27, SQL Server 2025 17.0.5035.5)

Measured with a clean-room probe that does not use DuckDB: `build/probe087/e4c*.py`, `e4c*.cpp`, and
`test_backup_crypto` [7]. It was run over the fixtures that `build/fixtures087/gen.sh` makes on a throwaway
container. No key byte or password is printed by any probe.

## Fixtures

| File | Database / DEK | Compression | Other |
|---|---|---|---|
| `tde_small_plain` | `bak_tde_small` (`dbo.t`: (1,'one'), (2,'two'), (3,'three')), AES_256 under `tde_cert` | none | the reference |
| `tde_small_xpress` | same | MS_XPRESS | |
| `tde_small_zstd` | same | ZSTD | |
| `tde256_full_xpress` | `bak_tde256`, AES_256 | MS_XPRESS | CHECKSUM |
| `tde256_full_xpress_nock` | same | MS_XPRESS | NO_CHECKSUM |
| `tde256_full_zstd` | same, DEK under `tde_cert2` (after the rotation) | ZSTD | CHECKSUM |
| `tde128_full_xpress` | `bak_tde128`, AES_128 | MS_XPRESS | |
| `tde256_diff_xpress` | `bak_tde256`, DEK under `tde_cert2` | MS_XPRESS | a differential after a change |
| `tde256_enc_full` | `bak_tde256` | MS_XPRESS | `WITH ENCRYPTION` (AES_256, `enc_cert`) as well |

## Layout

- **Container flags.** Bits 0x8 and 0x20 are added to the compressed forms:

  | Flags | Meaning |
  |---|---|
  | `0x2B` | MS_XPRESS |
  | `0xAB` | ZSTD (E4b listed only 0x2B / 0x2F; the ZSTD one is 0xAB) |
  | `0x2F` | MS_XPRESS and `WITH ENCRYPTION` |

  `0xAF` (ZSTD, encrypted, TDE) was not made. It stays refused, and the refusal names the word.
- **The header keeps the DEK wrapped at 0x214**, in the boot page's form: u32 20, then the TDE
  certificate's SHA-1 thumbprint at 0x218, then u32 384 at 0x22C, then the RSA-wrapped DEK from 0x230. It is
  the same blob that `FCBFileDEK` holds and it unwraps the same way. This held in 7 of 7 files, and after the
  rotation it names `tde_cert2`.
  - In `0x2F`, the backup's own encryptor header is at 0x14 too (the 0x7 form: thumbprint at 0x18, the key at
    0x30). The two blobs are independent.
- **The media label** is a chain block at **0x438**, not 0x1E0, because the DEK's blob takes the space up
  to 0x3B0. It is block kind 0x61 (or 0x71), with 8 units, which is 4 KB of output holding the TAPE DBLK.
- **The chain starts at 0x2000.** Every block has one kind: **0x61** under `0x2B` / `0xAB`, and **0x71**
  under `0x2F`. The block header is the encrypted form's (E4b):

  | Bytes | Field |
  |---|---|
  | `[0]` | kind |
  | `[1]` | units of 512 bytes (the output) |
  | `[2:4]` | the compressed length, the decoder's input (0 = 65536) |
  | `[4:20]` | the IV |
  | `[20:24]` | the payload's length on disk, which is the compressed length rounded up to 16 (checked on every block of the nine files) |
  | `[24:28]` | not named; it differs per block |

  - In 0x61 the IV is not random. It is a small counter at `[4:8]`, zero at `[8:12]`, a per-run word at
    `[12:16]` and 0x0B at `[16:20]`.
  - In 0x71, `[4:8]` is a random-looking base plus the same counter.
- **0x61 = a compressed block under the DEK.**
  - The payload is AES-CBC (or the DEK's algorithm) over the compressor's output, with the header's IV and no
    padding.
  - The decryption gives the MS_XPRESS / ZSTD stream, and that stream gives the data set: `SSET` first.
  - Neither an all-zero IV nor `[4:8]` alone decodes. Without decryption the stream is not a valid
    MS_XPRESS stream.
- **0x71 = the same, with the backup's key outside.** The payload is decrypted under the backup's key, then
  under the DEK, each with the same header IV. That order decodes, and no other order, nor either key alone.
- **The pages in the data set are in clear.** SQL Server decrypts each page before it compresses. The
  pages of `tde_small_xpress` and `tde_small_zstd`, laid against `tde_small_plain`'s pages decrypted under
  the DEK, compare as follows:
  - **369 of 376 are equal byte for byte**, once the header is put in the clear form below.
  - The other 7 carry a different `m_lsn`: they were written between the two backups (the file header, the
    boot page, and a few data pages).
- **A page that TDE encrypts says so in its header**, so the risk "no per-page encrypted indicator is known"
  is answered:
  - `m_flagBits` (u16 @4) bit **0x0800** is set on 374 of 374 encrypted pages. It is clear on the boot
    page (13), on the file header (15), and on every page of the compressed form.
  - An encrypted page carries two checksums: @60 is the checksum of the stored (encrypted) bytes, and @64 is
    the checksum of the page in clear.
  - The compressed form's page has the clear checksum @60, zero @64, and the flag clear.
- **The MTF streams** still say nothing of the encryption: every stream's encryption field is 0, as E4b
  found.
- **The seek index after the chain names offsets inside its last block.** This was seen in 4 of 4 such
  files: 0x344000 is the block's start, then 0x345254, 0x346254 and 0x347254 follow at the same physical
  offset. An encrypted backup of a database without TDE had one entry per block. Those offsets are taken
  as positions inside the named block, and no checkpoint is made of them.

## As built

- `ReadContainerHeader` accepts `0x2B` / `0xAB` / `0x2F` and reads the DEK at 0x214 into
  `ContainerHeader::tde`, `tde_thumbprint` and `wrapped_dek`. A thumbprint that is not 20 bytes, or a
  wrapped length outside 128..1024, is refused by name.
- `ContainerKeys`, which holds the backup key and the DEK, replaces the single key in these readers:
  - `ContainerStream`, `ReadContainerTape` (the label at 0x438 when `tde`), `ContainerPassMedium`, and the
    page index's `ContainerFamily`;
  - `OpenedFamily::keys`.
- A block's layers are bits of its kind: 0x10 is the backup key's layer and 0x40 the DEK's. The backup's
  layer is decrypted first.
- `OpenFamily` selects the DEK's secret by the header's thumbprint (D3), as it does for an encrypted backup,
  and on every call for a cached entry (D4, `OpenedBackup::RequireKeysHeld`).
- `PageIndex::DecryptPage` now decides by the page's flag 0x0800, where it used to decide by its type.
  - After decrypting, it writes the header as the server does for a page in clear: the flag cleared, the
    clear checksum moved to @60, and @64 zeroed.
  - So a page reads the same from either form of backup, and a compressed TDE backup's pages, which lack
    the flag, are left as they are. The boot page's DEK is still read (`ApplyTde`), and is needed only
    by the uncompressed form.
- Seek index: an entry at the previous entry's physical offset is accepted when its logical offset lies
  inside that block (`ParseSeekIndex`, `ReadSeekIndex`, `OpenChainFromTail`).

## Checked (`test_backup_crypto` [7], `test_backup_container`)

- `tde_small_xpress` / `tde_small_zstd`:
  - 369 of 376 pages equal the plain backup's decrypted pages, and the 7 others moved their `m_lsn`;
  - `dbo.t`'s 'three' is found in a data page;
  - the media label is at 0x438.
- `tde256_full_xpress`, `_nock`, `_zstd` (tde_cert2), `tde128_full_xpress` and `tde256_enc_full` (both
  keys): 229 / 245 / 245 / 236 / 229 live data pages each, read through `AllocationMap` / `PageStore`, with
  every slot array sound.
- `tde256_diff_xpress`: its DEK is named under `tde_cert2`, and its data set is read with one SSET.
- The container's header checks are covered: the three words are accepted, a missing DEK is refused, and
  0xAF is refused.

## Not established from the bytes

- **`[24:28]` of a block header.** It differs per block and is not the length or the IV. A checksum is
  likely, but none of CRC-32 / XXH32 over the plaintext or ciphertext was tried. The reader does not check it.
- **`0xAF`** (ZSTD with `WITH ENCRYPTION` on a TDE database) was not produced. It stays a refusal that
  names the word. If the server makes it, the bits say 0x71 blocks and ZSTD, but that has not been seen.
- **The overlay of `tde256_diff_xpress` over its full.** This is 085's path: `OpenOverlay`, and the
  differential's DCM pages through the DEK. It was not run here, because it needs the DuckDB build and the
  secret. It should be checked with `mssql_backup_scan(…, diff :=)` on the main session's server.
- **`mssql_backup_info` without a key (D10).** A compressed TDE backup's SSET is inside the DEK's ciphertext,
  so only the container header is in clear: the flags and the DEK's thumbprint. The owner decided on
  2026-11-27 that info shows those fields and NULL for the rest, with a WARNING (spec D10).

## The log tail (D9, measured with the same probe)

The tail of every TDE fixture fails the log block checks in both forms:

- uncompressed: "record 1 … outside the block's records";
- compressed, its stream decrypted under the DEK: "a block of size 0".

A plain encrypted backup's tail parses. So SQL Server does not decrypt the log before it compresses, as it
does the pages. The tail stays E10's, and the reader refuses to parse it by name (spec D9).
