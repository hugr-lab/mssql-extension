# Spec 087: encrypted backups and TDE

Status: **draft**, 2026-11-05. Not started. Depends on spec 085 (container, page store). Independent of
086. Series overview: `../README.md`. Markers and source abbreviations as in 085.

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
older versions; ZSTD with encryption; the exact offset of `FCBFileDEK` (the prototype finds it by its
pattern in boot-page bytes 600–1200); the 4 KB region at 0x1000 of an uncompressed encrypted
container; TDE page type 21; a database mid-way through an encryption scan or a DEK rotation; the
**log tail of a TDE backup** (the log is encrypted per VLF, W§5; E4 did not look); EKM keys, which can
never be read offline (documented).

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
  headers. Nothing above the page store knows about encryption.

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

- **D8 — algorithms.** AES-256 is verified. AES-128 (0x660E) and AES-192 (0x660F) take the same path
  and ship behind CI fixtures. TRIPLE_DES_3KEY (0x6603, 8-byte blocks, IV unknown) is refused by name
  until a fixture shows its layout.

- **D9 — TDE backups have an unknown fuzzy status until their log tail is readable.** The tail is
  expected to be encrypted per VLF (W§5). Until spike E10 reads it, a TDE backup reports
  `fuzzy_status = 'unknown'` for every table, which `fuzzy := 'error'` refuses and which spec 090's
  recovery refuses. Consequence for the lake (owner: no dirt): mssql-ducklake refuses a TDE backup
  until E10 lands, because an `unknown` table may hold dirt (090 D7: never dirt with a warning).
  Browsing it with `fuzzy := 'warn'` stays possible.

- **D10 — what works without a key.** `mssql_backup_info` never needs a secret. For an encrypted
  backup it reports `encrypted`, `encryptor_thumbprint` and the media fields outside the encrypted
  stream, and NULL for the rest (the SSET and SCIN are inside the encrypted blocks), with
  `needs_key = true`. For TDE it reports the 085 fields that come from MTF and the boot page (both
  plaintext), plus `tde = true` and `tde_thumbprint`; `page_count` needs the encrypted GAM / PFS pages
  and the database-level fuzzy fields need the log tail (D9), so they are NULL without a key, and the
  fuzzy fields stay NULL until E10. Every other function needs the key.

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
  payload `(clen + 15) & ~15`; IV = header[4:20]; decrypt, keep `clen`, then decompress to
  `unit × 512`.
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
11. **E10 spike** — the TDE log tail: is MQTL encrypted, where is each VLF's DEK record, what is the
    IV; decrypt the 64 KB tail of a quiet TDE backup and a loaded one — 3–5 d, timeboxed, not in the
    total

**Total ≈ 16.5 days, 3–3.5 weeks**, plus E10. R§8 phase 1b gave 1–2 weeks for the decryption itself;
the secret type, the selection rule, the CI fixtures and the docs are the rest.

## Risks and open questions

- **The TDE log tail (D9)** is the one gap that touches the owner's "no dirt" rule: until E10, a TDE
  backup cannot be shown consistent or recovered.
- **Older versions' encryptor type**: if 2017 / 2019 wrap with PKCS#1 v1.5 or SHA-1 OAEP, D7's
  fallback covers it only if the guess is right; the matrix decides.
- **Partially encrypted databases** (an encryption or decryption scan in progress, a DEK rotation):
  no per-page "encrypted" indicator is known. Such a backup is refused if its state can be read from
  the boot page (field to locate); otherwise D5's page check catches a wrongly decrypted page by name.
- **PFX** could not be produced on this build (E4§2); its support rests on OpenSSL's PKCS#12 and a
  locally synthesised file.
- **Persistent secrets** store the key container and its password unencrypted (DuckDB's design).
- Open for the owner: whether an explicit `key_secret := 'name'` override should exist beside D3
  (this draft has none, to keep D3 the only rule; DQ-29 in [decisions.md](../085-backup-reader-core/decisions.md)).

## References

- RESEARCH.md §2 (verdict on encryption), §6.1 layer 1 (the secret and the selection rule).
- research-web.md §4 (backup encryption), §5 (TDE: DEK in the boot record, the log per VLF).
- E4 §1 (encrypted layout), §2 (PVK), §3 (TDE), §4 (what a reader needs, selection), §5 (unnamed
  fields); `experiments/E4/e4_decrypt_backup.py`, `e4_pvk.py`, `e4_tde_decrypt.py`.
- 085 D5 (page store), D9 (cache), D10 (block index).
