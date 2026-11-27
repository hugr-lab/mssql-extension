# Spec 087: encrypted backups and TDE

Status: **draft**, 2026-11-05; **revised 2026-11-27** against the owner rules that came after it (no read
refusals, the ATTACH catalog primary, the per-instance cache: § Owner rules) and E4b's measurements (§ What
E4b measured). Depends on spec 085 (container, page store). Independent of 086. Series overview:
`../README.md`. Markers and source abbreviations as in 085.

## Problem

`BACKUP … WITH ENCRYPTION` and backups of TDE databases were the two cases no public tool reads
(W§4–§5). E4 removed them from the unsolved list: both decrypt offline with the certificate's exported
private key, and a reader needs nothing else (E4 verdict). Spec 085 refuses them at open. This spec
reads them, takes the key material only from a DuckDB secret, and selects the secret by the owner's
rule.

## What E4 established (2026-11-05, (2025))

| | encrypted backup | TDE backup |
|---|---|---|
| container | always `MSSQLBAK`, even `NO_COMPRESSION` (HEADERONLY says `Compressed = 1`) | ordinary MTF |
| thumbprint in plaintext | `MSSQLBAK` header @0x18 (20 bytes, after u32 length 20 @0x14) | boot page `FCBFileDEK` (u32 20, then 20 bytes) |
| wrapped key | u32 length (384 = RSA-3072) after the thumbprint, then the blob | same shape, after the boot page thumbprint |
| unwrap | reverse the blob (CryptoAPI is little-endian), RSA-OAEP with SHA-256 and MGF1-SHA-256 → a 44-byte `PLAINTEXTKEYBLOB`: `bType 0x08`, `bVersion 0x02`, `aiKeyAlg 0x6610` (CALG_AES_256), u32 32, the key. PKCS#1 v1.5 and the unreversed blob both fail | same |
| cipher | AES-256-CBC per container block; IV = block header bytes [4:20]; the ciphertext is padded to 16 while the header's length is the plaintext's: decrypt `ceil(clen / 16) × 16`, keep `clen`; no padding check | AES-256-CBC per page over bytes [96:8192] (8,096 = 506 × 16); IV = u32 page id ‖ u16 file id ‖ 10 zero bytes |
| left in plaintext | the 4 KB container header, the SFMB (0x2000 when stored, 0x1000 when compressed) | the 96-byte page header of every page; the whole boot page (13) and file-header page (15) |
| after decryption | the block outputs are the MTF data set, byte-identical to a plain backup (337 / 338 pages; the boot page differs between any two backups) | an ordinary page store; every row of `t_tde` (800) decoded, first record of every page included |
| block kinds | high nibble 0x3 = encrypted (0x2 plain); low nibble 1 = compressed, 3 = stored: 0x33, 0x31 | — |
| header flags @0x0C | 5 = encrypted, 7 = encrypted and compressed (3 = compressed only) | — |
| oracle | `RESTORE HEADERONLY`: `KeyAlgorithm = aes_256`, `EncryptorType = CERTIFICATE_OAEP_256`, `EncryptorThumbprint` | `sys.dm_database_encryption_keys` |

The exported private key (E4§2): `BACKUP CERTIFICATE … WITH PRIVATE KEY (FILE = …, ENCRYPTION BY
PASSWORD = …)` writes a DER `.cer` and a Microsoft PVK: magic `0xB0B5F11E`, keytype 1, encrypted 1,
salt length 16, key length 1,748 (an RSA-3072 `PRIVATEKEYBLOB`); the 8-byte `BLOBHEADER` (`07 02 00 00
00 a4 00 00`) stays in clear, the rest is **RC4** with key `SHA1(salt ‖ ASCII password)[:16]`.
`FORMAT = 'PFX'` was refused by this build's `BACKUP CERTIFICATE`.

Not established: AES-128 / 192 and TRIPLE_DES_3KEY; the `CERTIFICATE` (non-OAEP) encryptor type of
older versions (refused, its message naming it: no PKCS#1 v1.5 fallback -- the review of the build found that one
would be a Bleichenbacher oracle on the user's key; a fixture that needs it brings it back with implicit
rejection on); ZSTD with encryption; the exact offset of `FCBFileDEK` (the prototype finds it by its
pattern in boot-page bytes 600–1200); the 4 KB region at 0x1000 of an uncompressed encrypted
container; TDE page type 21; a database mid-way through an encryption scan or a DEK rotation; the
**log tail of a TDE backup** (the log is encrypted per VLF, W§5; E4 did not look); EKM keys, which can
never be read offline (documented).

## What E4b measured (2026-11-27, SQL Server 2025 17.0.5035.5)

The fixtures (synthetic databases, test-only certificates; generator `test/backup/gen/encrypted.sql`):
`bak_enc` backed up `WITH ENCRYPTION` under AES_128 / AES_192 / AES_256 / TRIPLE_DES_3KEY, each with
`NO_COMPRESSION`, `COMPRESSION` (MS_XPRESS) and `COMPRESSION (ALGORITHM = ZSTD)`, with and without
`CHECKSUM` (24 sets); striped; by a second certificate; a differential; TDE databases under AES_256 and
AES_128, backed up plain and compressed; an encrypted backup of a TDE database; a DEK rotated to a second
certificate, then a full and a differential. Read by a clean-room probe (DuckDB-free, from E4's facts, no
SQL Server source), structure only. **(2025)** throughout.

- **Every algorithm the server offers is written** -- TRIPLE_DES_3KEY too (deprecated, not refused). And
  every one unwraps the same way: RSA-OAEP / SHA-256 / MGF1-SHA-256 over the reversed blob (E4), to a
  `PLAINTEXTKEYBLOB` of **44 bytes whatever the key's length** (bType 8, version 2, then `aiKeyAlg`:
  `0x660E` AES-128 with 16 key bytes, `0x660F` AES-192 with 24, `0x6610` AES-256 with 32, `0x6603`
  3DES with 24; the rest zeros).
- **The container's flags word**: `0x5` encrypted, stored; `0x7` encrypted, MS_XPRESS; **`0x87`
  encrypted, ZSTD** (085 refused it as unseen). And two new words, of backups of a **TDE** database
  with compression: `0x2B` (not encrypted) and `0x2F` (encrypted). Bits `0x8` and `0x20` mark them.
- **An encrypted container's block** (the 28-byte header): `[0]` kind, `[1]` output units of 512,
  `[2:4]` the payload's **plaintext** length (`clen`; 0 = 65536), `[4:20]` the IV (an AES block; 3DES
  takes `[4:12]`), **`[20:24]` the payload's length on disk** -- `clen` rounded up to 16, **for 3DES too**
  (its blocks are 8: the header's field, not the cipher's block, sizes the read) -- `[24:28]` not yet
  named (differs per block). Kinds: `0x33` encrypted stored, `0x31` encrypted compressed (MS_XPRESS and
  ZSTD alike: the algorithm is the flags word's); one kind a chain. A chain starts after the SFMB:
  **`0x2000` when compressed, `0x3000` when stored**. A stored block's plaintext opens the data set
  with `SSET` (E4's D7 check holds for every algorithm).
- **A TDE backup with compression is another container form**: flags `0x2B` (MS_XPRESS), `0xAB` (ZSTD),
  `0x2F` (and `WITH ENCRYPTION`); its chain's kinds **`0x61`** and **`0x71`**. Decoded by **E4c**
  ([measured/E4c-compressed-tde.md](measured/E4c-compressed-tde.md)): the header keeps the DEK wrapped at
  0x214 (the boot page's form) and the media label at 0x438; a block is the encrypted form's (IV `[4:20]`,
  on-disk length `[20:24]`), its payload the compressed stream under the DEK (0x61), and under the backup's
  key outside it (0x71); the pages inside are in clear.
- **TDE** (uncompressed, plain MTF): **`FCBFileDEK` sits on the boot page (9, 1) at offset 912**: u32 20,
  the thumbprint at 916, u32 384 at 936, the wrapped DEK from 940. A full backup holds the boot page twice
  (both data passes). AES_128 and AES_256 DEKs decrypt E4's way: AES-CBC over bytes [96:8192], IV = u32
  page id ‖ u16 file id ‖ 10 zero bytes; every data page's slot array is sound after it (228 of 228).
- **A DEK rotated** to another certificate (`ALTER DATABASE ENCRYPTION KEY ENCRYPTION BY SERVER
  CERTIFICATE`) is the **same key, re-wrapped**: the boot page names the new certificate, the pages
  decrypt with the key it unwraps. The log keeps both certificates' thumbprints in records near the
  tail (E10's lead).
- **The MTF streams of a TDE backup say nothing of it**: every stream's encryption field is 0 (MQDA,
  MQTL, the rest), as in a plain backup's. The pages and the log are encrypted inside the streams, so the
  log tail's parser meets its ciphertext unwarned (D9: E10 is to read it).
- **Page type 21** appears in TDE backups only (101 of a full's 534 pages): unused pages (no slots,
  `m_freeData` 96, the body zeros once decrypted, a few holding stale bytes). Read as a free page.
- **The exported keys**: the PVK of E4 (1,788 bytes, salt 16, an RSA-3072 `PRIVATEKEYBLOB` of 1,748,
  e = 65537), for every certificate. **`BACKUP CERTIFICATE … WITH FORMAT = 'PFX'` is accepted on this
  build** (E4's was refused): PBES2 / PBKDF2 / HMAC-SHA256 / AES-256-CBC, all in OpenSSL's default
  provider.

## Owner rules since this spec was written

- **No read refusals** (owner, 2026-11-22): a table always reads; a value that cannot be read is NULL
  with a WARNING. Encryption leaves that rule's scope as it is: a **file** whose key no secret holds
  cannot be read at all, which is a named refusal of the file (the path, its thumbprint, the secrets that
  matched), not of a table, and nothing to do with decoding. A page that does not decrypt to a sound page
  (a wrong key past D7's checks, a damaged page) is the page store's damaged page: its rows NULL with a
  WARNING as 085 does, never a failed table.
- **The ATTACH catalog is the primary interface** (owner, 2026-11-22): `ATTACH 'x.bak' AS bk (TYPE
  mssql_backup)` reads an encrypted or TDE backup like any other, its secret found by D3 at the ATTACH
  (the catalog's states are built there) and again by every statement that opens the files (D4).
- **The opened-file cache is per DuckDB instance** (085 D9): an entry of an encrypted backup holds the
  unwrapped key in its holder, zeroised when the entry goes; D4 re-runs the selection before the cached
  entry is used, so a `DROP SECRET` takes effect at the next statement.

## Decisions

- **D1 — key material only through a secret (owner, R§6.1).** Inline parameters would end up in
  generated SQL and query logs. A new secret type `mssql_backup_key`, never a storage secret.

- **D2 — the secret holds the files' contents, read once at CREATE SECRET.**

  ```sql
  CREATE SECRET sales_key (
      TYPE mssql_backup_key,
      CERTIFICATE 's3://keys/sales_cert.cer',   -- DER or PEM, read through DuckDB's FileSystem
      PRIVATE_KEY 's3://keys/sales_cert.pvk',   -- PVK, PFX or PEM (PKCS#8)
      PASSWORD    '…',                          -- the private key's password; redacted
      SCOPE       's3://backups/sales/'
  );
  ```

  `CERTIFICATE_DATA` / `PRIVATE_KEY_DATA` (base64) replace the paths where files are not reachable;
  a PFX carries its certificate, so `CERTIFICATE` may then be omitted. At creation the secret reads the
  files, parses the certificate, computes its thumbprint (SHA-1 of the DER), decrypts the private key
  with the password, and checks that its modulus equals the certificate's (E4§4: a wrong password
  gives a different modulus; the error comes before any backup byte is touched). It stores the
  certificate, the **still password-protected** key container and the password (redacted), and shows
  the thumbprint in `duckdb_secrets()` (it is public: it sits in plaintext in every backup). A decrypted
  private key is never stored. Reason: a secret that only names files would fail later, far from its
  cause, and could not be persisted meaningfully; this way creation proves the material works.

- **D3 — selection: scope first, thumbprint second (owner, R§6.1).** For each backup file:
  1. Candidates are the `mssql_backup_key` secrets whose scope matches the file's path (a secret
     without a scope matches every path, as DuckDB scopes do); the longest matching scope wins.
  2. Only when several candidates share that longest scope does the thumbprint decide: the one whose
     certificate thumbprint equals the backup's (the `MSSQLBAK` header's for an encrypted backup, the
     boot page's `FCBFileDEK` for TDE).
  3. The chosen secret's thumbprint must equal the backup's; otherwise the call fails, naming the path,
     the backup's thumbprint and how many secrets matched the path. It never falls through to a secret
     outside the matching scopes.

  DuckDB's `LookupSecret` returns only the single longest match, so the reader iterates
  `SecretManager::AllSecrets`, filters the type, and applies the scope match itself
  (`main/secret/secret_manager.hpp:132,142`). Two candidates equal in scope and thumbprint (a
  duplicate) are ordered by DuckDB's storage priority (temporary first). The stripes of one media set
  must all select the same secret; a full and its overlaid differential select one each.

- **D4 — selection runs on every call, even on a cached backup.** A cached decrypted backup (085 D9)
  keeps its unwrapped key while the entry lives; re-running D3 before using it makes a `DROP SECRET`
  or a changed scope effective at once. The key is zeroised when the entry is destroyed.

- **D5 — decryption sits in the layers 085 already has.** Encrypted backups decrypt in the
  `Container`, per block, before decompression: the block index records each block's IV, so random
  access survives. TDE decrypts in the `PageStore`, per page, at read time, below the allocation map:
  PFS, GAM and IAM pages are encrypted too (E4§3), while the page index reads only the plaintext
  headers. Nothing above the page store knows about encryption. Resources (085 D17): a block or a page
  is decrypted in the buffer its read already holds, one at a time per thread, so decryption adds
  nothing that grows with the file.

- **D6 — crypto through the statically linked OpenSSL**, except RC4: RSA-OAEP (`EVP_PKEY_decrypt` with
  OAEP / SHA-256 / MGF1-SHA-256), AES-CBC with padding off, SHA-1, PKCS#12, PEM. RC4 (for PVK) is
  twenty lines of our own, because OpenSSL 3 keeps it in the legacy provider, which the static
  vcpkg build need not carry. A PFX protected by a cipher that needs the legacy provider (RC2, RC4:
  common in old Windows exports; PBE-SHA1-3DES stays in the default provider) is refused by name with the re-export hint (`openssl pkcs12 -export -keypbe AES-256-CBC`).

- **D7 — the unwrap is validated twice.** OAEP-SHA256 first; if it fails, PKCS#1 v1.5 (the older
  `CERTIFICATE` encryptor type is assumed, unverified); either result must be a well-formed
  `PLAINTEXTKEYBLOB` (bType 8, version 2, a known `aiKeyAlg`, a length that matches it), and for an
  encrypted backup the first decrypted block must start with a valid SSET DBLK (checksum included).
  A wrong key is therefore a named error, never garbage pages.

- **D8 — algorithms.** AES-128 (0x660E), AES-192 (0x660F) and AES-256 (0x6610), and TRIPLE_DES_3KEY
  (0x6603: 3DES-CBC, IV the header's `[4:12]`), every one measured by E4b. (Revised 2026-11-27: 3DES was
  refused while its layout was unknown.)

- **D9 — TDE backups have an unknown fuzzy status until their log tail is readable.** The tail is
  expected to be encrypted per VLF (W§5). Until spike E10 reads it, a TDE backup reports
  `fuzzy_status = 'unknown'` for every table, which `fuzzy := 'error'` refuses and which spec 090's
  recovery refuses. Consequence for the lake (owner: no dirt): mssql-ducklake refuses a TDE backup
  until E10 lands, because an `unknown` table may hold dirt (090 D7: never dirt with a warning).
  Browsing it with `fuzzy := 'warn'` stays possible. As built (2026-11-27): measured, the tail of every TDE
  fixture is ciphertext in both forms (the compressed one's too, its stream decrypted: the log blocks fail
  their checks; E4c), so no reader of the log parses it -- `OpenedHeaders::TdeLogRefusal` names the DEK's
  certificate and E10, `OpenedBackup::Fuzzy` keeps it as the tail's error (so the scan, the catalog and the
  change listing warn, or refuse under `fuzzy := 'error'`; `mssql_backup_info`'s D11 columns are NULL with
  the WARNING), `mssql_backup_transactions` / `_transaction_records` refuse by name, and
  `mssql_backup_tables.fuzzy_status` is `'unknown'`.

- **D10 — what works without a key** (owner's decision, 2026-11-27, for every encrypted or TDE form,
  the compressed TDE ones -- `0x2B` / `0xAB` / `0x2F` -- included): **`mssql_backup_info` shows what is
  readable in clear and nothing invented.** Without the key it never errors: it reports what the container
  header gives -- `compressed`, `compression_algorithm`, `encrypted`, `encryptor_thumbprint`, `tde`,
  `tde_thumbprint` (and `position`, the set asked for) -- every column that needs the decrypted stream NULL
  (the SSET, SCIN, the media label, FILELISTONLY's list and `key_algorithm`, which only the unwrapped key
  tells), and one WARNING naming the certificate (thumbprint) whose key would read the rest and CREATE
  SECRET's form. An uncompressed TDE backup's MTF headers are in clear, so it is described whole without a
  key, `tde_thumbprint` (and `files.tde_thumbprint`, FILELISTONLY's TDEThumbprint) from its data files'
  SFIN records, which carry the DEK as the boot page does (+452: u32 20, the thumbprint, u32 384, the
  wrapped DEK; 13 of 13 TDE fixtures, none of 55 other backups). `page_count` stays NULL (085), the D11
  columns NULL with a WARNING until E10 (D9). The header carries no field for the key algorithm in clear
  (0x10 is 3 in every sample, the next bytes vary; not named). **Reading the data requires the key**:
  `mssql_backup_scan`, `mssql_backup_catalog`, the listings that need the catalog (`_tables`, `_views`,
  `_columns`, `_schemas`), the change and log functions, a history over encrypted files, and `ATTACH …
  (TYPE mssql_backup)` fail before any row with a named error: the thumbprint needed, the
  `mssql_backup_key` secrets that match the path (by name and thumbprint), and CREATE SECRET's form. A
  wrong key (a secret that holds the thumbprint but does not unwrap) is D7's error, in info too.

## Design

### Surface

- Secret type `mssql_backup_key`, provider `config`; parameters `certificate`, `certificate_data`,
  `private_key`, `private_key_data`, `password` (redacted), `scope`.
- `mssql_backup_info` gains `needs_key`, `key_algorithm` (`aes_128` / `aes_192` / `aes_256`, known
  after the unwrap; NULL without a key), `encryptor_type` (to locate; NULL until then), `tde`,
  `tde_thumbprint`, `tde_algorithm`.
- No new function parameters: the secret is found by D3.

### Code

```text
src/backup/
  backup_crypto.cpp          RSA unwrap, PLAINTEXTKEYBLOB, AES-CBC block / page decrypt, zeroising key holder
  backup_key_container.cpp   PVK (with RC4), CryptoAPI PRIVATEKEYBLOB → EVP_PKEY, PFX, PEM; DuckDB-free
  backup_tde.cpp             FCBFileDEK on the boot page, DEK unwrap, page decryption hook
src/backup/functions/
  mssql_backup_key_secret.cpp   secret type, creation-time load and validation, D3 selection  [DuckDB]
```

Rules:
- Encrypted container: header @0x14 thumbprint length (must be 20), @0x18 thumbprint, @`0x18 + 20`
  u32 wrapped length (must equal the RSA modulus size), then the blob. Chain start: the first valid
  block header after the plaintext SFMB (0x2000 compressed, 0x3000 stored, E4 `find_chain_start`),
  confirmed by the next header being valid too.
- Block decryption: `encrypted = (kind & 0xF0) == 0x30`, `compressed = (kind & 0x0F) == 1`; on-disk
  payload the header's u32 at `[20:24]` (E4b: `clen` rounded up to 16 for every algorithm; checked to be
  that); IV = header[4:20] (3DES: [4:12]); decrypt, keep `clen`, then decompress to `unit × 512`.
- TDE: detected by a well-formed `FCBFileDEK` on the boot page (u32 20, 20 bytes, u32 ∈ {256, 384,
  512}); its offset is pinned per version by task 5 instead of the prototype's scan. A page is
  decrypted when its type is not 13 or 15 (pads and zero units are skipped earlier). A page whose
  decrypted body fails 085's page checks is a named error naming the page, never skipped silently.
- Keys: the AES key lives in a holder that zeroises on destruction (`OPENSSL_cleanse`); no key, IV of
  a page, password or private-key byte is logged, printed by `MSSQL_DEBUG`, or put in an error.

## Security and hardening

- The PVK parser bounds salt and key lengths against the file, checks the magic, the `BLOBHEADER`
  (bType 7, version 2, `aiKeyAlg` 0xA400), the `RSA2` magic, and that every component length follows
  from the bit length; an unencrypted PVK (encrypted = 0) is accepted.
- The `PLAINTEXTKEYBLOB` check of D7, and the RSA ciphertext length equal to the modulus size.
- A thumbprint length other than 20, a wrapped length other than the modulus size, an unknown flags
  word or block kind: refused by name.
- Passwords: ASCII verified (E4§2); a non-ASCII password is tried as UTF-8, then UTF-16LE (as
  `e4_pvk.py` does; both unverified), and the error says so if both fail.
- Persistent secrets: DuckDB stores them unencrypted on disk (`~/.duckdb/stored_secrets/`, mode
  0600). With this type that means the key container **and** its password. The docs say so and
  recommend temporary secrets or a secret-manager extension.
- Decrypted pads are still pads (085): skipped, never exposed.
- Fuzz harnesses: `fuzz_backup_pvk` (PVK and `PRIVATEKEYBLOB`), `fuzz_backup_keyblob`
  (`PLAINTEXTKEYBLOB` and the header fields), `fuzz_backup_enc_chain` (the encrypted block chain under
  a fixed test key, so the fuzzer reaches the decompressor behind it). DuckDB-free.

## As built (2026-11-27, first cut)

- `backup_crypto` (DuckDB-free): RC4; PVK (RC4 under SHA1(salt || password)[:16]) and its `PRIVATEKEYBLOB`
  as an OpenSSL RSA key; PFX (PKCS#12) and PEM through OpenSSL; a certificate's thumbprint; the unwrap
  (OAEP-SHA256 over the reversed blob, then PKCS#1 v1.5 with OpenSSL's implicit rejection turned off, either
  result held to a well-formed `PLAINTEXTKEYBLOB`, else "does not unwrap"); AES / 3DES CBC; `SecretBytes`,
  zeroised.
- `backup_container`: flags `0x87` accepted; E4c's `0x2B` / `0xAB` / `0x2F` read, their DEK from the header
  (`ContainerKeys`: the backup's key and the DEK, a block's layers its kind's bits 0x10 / 0x40), `0xAF` (not
  seen) refused naming it; seek index entries inside the last block accepted; the encrypted kinds
  `0x31` / `0x33`, their on-disk length from the header's `[20:24]` (checked to be the plaintext rounded up
  to 16), the IV `[4:20]`; the chain from `ChainStart` (0x3000 for an encrypted stored one); the media label
  of an encrypted stored container runs past the 4 KB header (into the 4 KB before its SFMB at 0x2000).
  `ContainerStream`, `ReadContainerTape` and the pass search's `ContainerPassMedium` decrypt a block (into a
  buffer of their own, never the read window) before they decompress it.
- `backup_key_secret` [DuckDB]: the secret type (D1, D2: checked at CREATE SECRET -- the certificate, the
  key opened with the password, its modulus the certificate's -- the container and the password kept,
  both redacted, the thumbprint shown) and D3's selection (`SelectContentKey`); `OpenFamily` selects for an
  encrypted family, and a cached entry selects again on every call (D4, `SelectKeysAgain`).
- TDE: `OpenBackupPages` reads `FCBFileDEK` from the boot page (9, 1) at 912 before the allocation map is
  built; the DEK (`OpenPagesOptions::tde_key`, D3 from `EnsureTables`) is set on the page index, whose
  `ReadPage` decrypts every page whose `m_flagBits` carries 0x0800 (E4c) and then writes its header as the
  server does for a page in clear (flag cleared, the clear checksum @64 moved to @60, @64 zero), so either
  form of backup reads the same page.
- `mssql_backup_info`: `encrypted`, `encryptor_thumbprint`, `key_algorithm` (the unwrapped key's), `tde`,
  `tde_thumbprint`, `files.tde_thumbprint`; D10's row without a key (`ReadClearHeaders`, `MissingKeys`:
  D3 asked through `HasBackupKeySecret`, which never unwraps) and its WARNING. D9 as above.
- Fuzz (task 8): `fuzz_backup_pvk` (ReadPrivateKey's PVK path), `fuzz_backup_keyblob` (`ReadPlaintextKeyBlob`,
  exposed for it, then `DecryptCbc`), `fuzz_backup_enc_chain` (every encrypted container form, fixed keys, a page
  key); seeds from `gen_backup_seeds` (the container seed in each form, a TDE-paged one, a blob per algorithm, a
  generated PVK plain and encrypted). `fuzz/build.sh` compiles `backup_crypto` and links libcrypto
  (`OPENSSL_PREFIX`, else pkg-config; the CI images install libssl-dev); the leak lane, `make
  fuzz-backup-replay` and the weekly ClusterFuzzLite run take the three.
- Fixtures (task 9): `test/backup/gen/encrypted.sql` (fixture `encrypted`, the oracle matrix; not run by default)
  and `encryption_samples.sql` (the committed set: `enc_sample`, `tde_sample`, `tde_sample_xpress`,
  `tde_sample_enc`, test-only certificates committed with their PVKs and password, under
  `test/data/backup/encryption`, made by its generate.sh); `compare_info.py --keys` creates a secret per
  certificate; `test/sql/backup/mssql_backup_encryption.test` over the committed set.
- Docs (task 10): the backups page's section, the columns, README, DATAMODEL.
- Not yet: E10 (the TDE log), the oracle lane's leg for `encrypted` (its keys stored beside the backups;
  compare_rows.sh's secrets), the committed samples themselves (made on a server: the main session).

## Testing and acceptance

Fixtures:
- **Generated in CI** (oracle lane): `CREATE MASTER KEY`, `CREATE CERTIFICATE`, then `BACKUP
  CERTIFICATE … TO FILE … WITH PRIVATE KEY` onto the shared volume; backups `WITH ENCRYPTION` for
  AES_128 / 192 / 256 and TRIPLE_DES_3KEY (the refusal), each uncompressed and with MS_XPRESS and ZSTD,
  with and without `CHECKSUM`, striped once; a TDE database under AES_256 and AES_128; an encrypted
  backup of a TDE database (two certificates); a certificate rotation of the DEK (`ALTER DATABASE
  ENCRYPTION KEY ENCRYPTION BY SERVER CERTIFICATE …`). Oracle: RESTORE on the same server.
- **Committed**: one small encrypted backup and one small TDE backup made with a **test-only**
  certificate created for these samples alone, committed with its PVK and password; nothing else
  ever uses that certificate. They feed the server-free lane and the fuzz seeds.

Secret tests (server-free): longest scope wins; a tie decided by the thumbprint; the mismatch error's
wording (path, thumbprint, count); a secret without a scope matches everything but loses to a scoped
one; other secret types ignored; stripes selecting different secrets refused; full and differential
selecting their own; `DROP SECRET` makes the next call on a cached backup fail; a wrong password fails
at `CREATE SECRET`.

Version matrix: 2017 / 2019 / 2022 / 2025; the encryptor type (`CERTIFICATE` vs
`CERTIFICATE_OAEP_256`) per version is what the matrix establishes for D7.

Acceptance:
1. Every encrypted and TDE fixture equals RESTORE + SELECT, and equals the plaintext backup of the
   same database where one exists.
2. Every D3 case behaves as stated, with no fall-through to a non-matching scope.
3. A wrong key, a wrong password, an EKM-protected backup, an unknown algorithm: each a named error
   before any row; none an InternalException or garbage.
4. No key byte or password appears in logs, errors, `duckdb_secrets()` or `MSSQL_DEBUG` output
   (grep gate in the lane, as spec 047's `mssql_pool_stats` redaction gate).
5. TDE backups report `fuzzy_status = 'unknown'` until E10, and `fuzzy := 'error'` refuses them.

## Tasks

1. Secret type: registration, parameters, creation-time load through the FileSystem, modulus check,
   redaction, thumbprint shown — 2.5 d
2. Key containers: PVK with RC4, `PRIVATEKEYBLOB` → `EVP_PKEY`, PFX, PEM — 2 d
3. Selection (D3, D4) — 1.5 d
4. Encrypted container: header fields, unwrap (D7), per-block AES-CBC, decrypt-then-decompress,
   encrypted blocks in the block index, AES-128 / 192 — 2.5 d
5. TDE: pin `FCBFileDEK` per version, DEK unwrap, page decryption in the page store, detection — 2 d
6. `mssql_backup_info` fields with and without a key; the named errors — 1 d
7. Key hygiene: holder, zeroising, the redaction grep gate — 0.5 d
8. Fuzz harnesses pvk / keyblob / enc_chain — 1.5 d
9. Fixtures: CI generation and the committed test-only samples — 2 d
10. Docs: the secret, the selection rule, persistent-secret warning, TDE limits — 1 d
11. **E4c spike** (done 2026-11-27, [measured](measured/E4c-compressed-tde.md)) — a TDE database's compressed backup (flags `0x2B` / `0x2F`, block kinds `0x61` /
    `0x71`): its block header, whether the payload is the DEK's ciphertext of compressed pages or the
    pages' own encryption under a compressed stream, and the encrypted variant's two keys — 2–3 d,
    timeboxed, before task 5 ships TDE as done
12. **E10 spike** — the TDE log tail: is MQTL encrypted, where is each VLF's DEK record, what is the
    IV; decrypt the 64 KB tail of a quiet TDE backup and a loaded one — 3–5 d, timeboxed, not in the
    total

**Total ≈ 16.5 days, 3–3.5 weeks**, plus E10. R§8 phase 1b gave 1–2 weeks for the decryption itself;
the secret type, the selection rule, the CI fixtures and the docs are the rest.

## Risks

- **The TDE log tail (D9)** is the one gap that touches the owner's "no dirt" rule: until E10, a TDE
  backup cannot be shown consistent or recovered.
- **Older versions' encryptor type**: if 2017 / 2019 wrap with PKCS#1 v1.5 or SHA-1 OAEP, D7's
  fallback covers it only if the guess is right; the matrix decides.
- **Partially encrypted databases** (an encryption or decryption scan in progress, a DEK rotation): a page
  says whether it is encrypted (E4c: `m_flagBits` 0x0800, 374 of 374), and the reader decrypts by it, so a
  half-encrypted database reads page by page; not yet measured on a backup taken mid-scan.
- **PFX**: produced on 17.0.5035.5 (E4b, PBES2 / AES-256); an old Windows export under RC2 / RC4 needs
  OpenSSL's legacy provider and is refused by name (D6).
- **TDE with compression (E4c)**: decoded (2026-11-27). Open: `[24:28]` of a block header (unchecked) and
  `0xAF` (not produced). Without its key, such a backup's info is its container header's (D10).
- **Persistent secrets** store the key container and its password unencrypted (DuckDB's design).
- Decided: no explicit `key_secret := 'name'` override in v1, so D3 stays the only rule (DQ-29,
  accepted, [decisions.md](../085-backup-reader-core/decisions.md)).

## References

- RESEARCH.md §2 (verdict on encryption), §6.1 layer 1 (the secret and the selection rule).
- research-web.md §4 (backup encryption), §5 (TDE: DEK in the boot record, the log per VLF).
- E4 §1 (encrypted layout), §2 (PVK), §3 (TDE), §4 (what a reader needs, selection), §5 (unnamed
  fields); `experiments/E4/e4_decrypt_backup.py`, `e4_pvk.py`, `e4_tde_decrypt.py`.
- 085 D5 (page store), D9 (cache), D10 (block index).
