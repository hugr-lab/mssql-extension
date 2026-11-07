# E4 — Encrypted backups and TDE: layout, and offline decryption with the certificate private key

Date: 2026-11-05. Server: SQL Server 2025 RTM-CU8 (17.0.4075.5), container `mssql-v2test`.
Research only; nothing committed. Scripts and samples under `experiments/E4/` (test passwords in
`experiments/E4/passwords.txt`, not reproduced here). This closes the "first look" part of §12 E4 and
removes "backup encryption" and "TDE" from the unsolved list in `RESEARCH.md` §2/§3 and
`research-web.md` §4–5 (`Unknown: how the per-backup key and IV are wrapped`; `Unknown: cipher mode,
IV derivation, which page bytes stay plaintext, and the DEK blob format`).

## Verdict

**Both are fully decryptable offline given the certificate's private key** — the certificate exported
`WITH PRIVATE KEY` is sufficient; no SQL Server, no RESTORE. Proven end to end on our own samples:

| Case | File | Result |
|---|---|---|
| `BACKUP … WITH ENCRYPTION (AES_256, SERVER CERTIFICATE)`, no compression | `bk_e4_enc.bak` | decrypted; 337/338 pages byte-identical to the plaintext backup (only the boot page differs — separate BACKUP, different LSN); all 500 rows of `t_heap` decoded with correct Unicode |
| same, **compressed** (MS_XPRESS) | `bk_e4_enc_comp.bak` | decrypt-then-decompress; same 337/338 identity; rows decoded |
| **TDE** database backup (DEK `ENCRYPTION BY SERVER CERTIFICATE`, AES_256) | `bk_e4_tde.bak` | DEK unwrapped; every page body decrypted; catalog bootstrapped from decrypted system pages; **all 800** rows of `t_tde` decoded, first record of every page included |

Decoders written clean-room from the bytes: `e4_pvk.py` (PVK private-key loader), `e4_decrypt_backup.py`
(encrypted-backup container), `e4_tde_decrypt.py` (TDE pages). All use only `cryptography` (RSA/AES/
SHA) plus the existing `tools/` MTF/page/XPRESS code.

The one documented impossibility stands: an **EKM-held** asymmetric key cannot be exported, so a backup
or DEK wrapped by an EKM key can never be read offline. This applies only to the certificate/asymmetric
case here.

## 1. Encrypted backup layout

An encrypted backup is **not** plain MTF — it is the same `MSSQLBAK` container used for compression
(encryption forces the container even with `NO_COMPRESSION`; `RESTORE HEADERONLY` reports
`Compressed = 1`). File header (offsets confirmed on both samples):

```
@0x00  "MSSQLBAK"
@0x08  u32  = 2                      container/format version
@0x0C  u32  flags                    5 = encrypted, 7 = encrypted+compressed (cf. 3 = compressed-only)
@0x14  u32  = 20                     EncryptorThumbprint length
@0x18  20B  EncryptorThumbprint      == sys.certificates.thumbprint  (here 96C8FB7D…6DFB)
@0x2C  u32  = 384 (0x180)            wrapped-key length  (= RSA-3072 ciphertext)
@0x30  384B wrapped key blob         RSA-OAEP(SHA-256) ciphertext, CryptoAPI little-endian (reversed)
```

`RESTORE HEADERONLY` is the oracle for the crypto fields: `KeyAlgorithm = aes_256`,
`EncryptorType = CERTIFICATE_OAEP_256`, `EncryptorThumbprint` = the 20 bytes above.

**Unwrapping the symmetric key.** Reverse the 384-byte blob (CryptoAPI stores big integers
little-endian), RSA-decrypt with **OAEP / SHA-256 / MGF1-SHA-256**. The plaintext is a CryptoAPI
`PLAINTEXTKEYBLOB` (44 bytes): `bType=0x08`, `bVersion=0x02`, `aiKeyAlg=0x6610` (**CALG_AES_256**),
`u32 len=32`, then the 32-byte AES-256 key. (PKCS#1 v1.5 and non-reversed both fail; reversed+OAEP-256
is the only match — this is what the `_256` in `CERTIFICATE_OAEP_256` means.)

**Block chain.** After a cleartext `SFMB` MTF block, a chain of 28-byte-headed blocks:

```
u8 kind, u8 unitflag(unit=flag*512), u16 clen(0=65536), u64 a, u64 b, u32 0, u32 crc
```

`kind` nibbles: high `0x3_` = encrypted (`0x2_` = plain), low `_1` = payload compressed, `_3` = stored.
So `0x33` = encrypted+stored (our `bk_e4_enc.bak`), `0x31` = encrypted+compressed
(`bk_e4_enc_comp.bak`), `0x21` = compressed-only (the earlier samples).

**Per-block decryption: AES-256-CBC, IV = the 16 header bytes `[4:20]`** (the block's `(a,b)`
offset/seek pair, which also exists in unencrypted compressed backups — SQL Server reuses it as a
unique per-block IV, so it is not secret). The ciphertext on disk is padded to a 16-byte boundary while
`clen` is the plaintext length: decrypt `ceil(clen/16)*16` bytes, keep `clen`. For `0x31`,
XPRESS-decompress the result to `unitflag*512`. Concatenated block plaintext **is** the MTF data set
(`SSET…MSLS`), identical to an uncompressed backup — hand it straight to the existing `pages.py`.

Pipeline: `certificate + PVK + password → AES key → per-block AES-CBC(IV=hdr[4:20]) [→ XPRESS] → MTF`.

## 2. The exported private key (PVK)

`BACKUP CERTIFICATE … WITH PRIVATE KEY (FILE=…pvk, ENCRYPTION BY PASSWORD=…)` writes a DER `.cer` and a
**standard Microsoft PVK** `.pvk` (magic `0xB0B5F11E`): header `magic,reserved,keytype=1,encrypted=1,
saltlen=16,keylen=1748`, 16-byte salt, then the key blob. `keylen=1748` = exactly a CryptoAPI
`RSA2` `PRIVATEKEYBLOB` for RSA-3072.

Decryption (recovered empirically, validated by matching the cert modulus): **RC4**, key =
`SHA1(salt ‖ ascii_password)[:16]`, applied **starting at blob offset 8** — the 8-byte `BLOBHEADER`
(`07 02 00 00 00 a4 00 00`) is stored cleartext and excluded from the RC4 stream; the `RSAPUBKEY`
(`RSA2`) onward is encrypted. (Strong 128-bit key, not 40-bit; ASCII password, no null terminator.)
`FORMAT='PFX'` was **rejected** by this build's `BACKUP CERTIFICATE` (syntax error), so only the PVK was
captured; a PFX can be synthesised locally from the recovered key if that import path needs testing.

A reader needs, per encrypted backup: the **DER certificate**, the **PVK** (or a PFX), and the **PVK
password**. With those three it computes everything above.

## 3. TDE layout

A TDE backup is an **ordinary MTF container** (not backup-encrypted); the encryption is in the pages.

- **Granularity: per page.** The 96-byte page **header is plaintext** (pages self-identify by type/id,
  so `pages.py hist` classifies them unchanged); the **body `[96:8192]` (8096 B = 506×16) is
  AES-256-CBC encrypted** (measured entropy ≈ 7.97 bits/byte vs ≈ 1.1 for the plaintext boot page).
- **Not encrypted:** the **boot page (type 13)** and the **file-header page (type 15)** only. PFS, GAM,
  SGAM, IAM, data and index pages are all encrypted.
- **Per-page IV = `pageId`(u32 LE) ‖ `fileId`(u16 LE) ‖ 10 zero bytes** — the 6-byte page pointer,
  left-justified in a 16-byte block. Confirmed: the first record of page `1:352` decrypts to
  `30 00 08 00 01 00 00 00 …` (statusA=0x30, pminlen=8, id=1), a textbook clustered record, and all 800
  rows decode including the first on every page.
- **The DEK lives in the boot page** as `FCBFileDEK`, the same shape as the backup header: `u32
  thumb_len=20`, 20-byte thumbprint, `u32 wrapped_len=384`, then the 384-byte RSA-OAEP(SHA-256)-wrapped
  DEK (byte-reversed) → `PLAINTEXTKEYBLOB` → AES-256 DEK. `sys.dm_database_encryption_keys` reports the
  same `encryptor_thumbprint` and `encryptor_type = CERTIFICATE_OAEP_256`. (The DEK is deliberately on
  one of the two plaintext pages, else it could not bootstrap.)

Pipeline: `certificate + PVK + password → unwrap DEK from boot page → per-page AES-CBC(IV=pageId‖fileId)
on body[96:8192] → normal MTF/page reader`.

## 4. What a reader needs, and secret selection

- **Secret contents:** DER certificate + private key (PVK or PFX) + the private-key password. One secret
  type serves both encrypted backups and TDE backups — the wrap (RSA-OAEP-SHA256 → AES-256 key/DEK) and
  the certificate selection are identical.
- **Thumbprint-based selection works and is cheap.** The 20-byte encryptor thumbprint is **plaintext**
  in both cases — in the `MSSQLBAK` header at a fixed offset for an encrypted backup, and in the boot
  page `FCBFileDEK` for TDE — and equals `sys.certificates.thumbprint`. This matches the design in
  `RESEARCH.md` §6.1: among `TYPE mssql_backup_key` secrets whose path scope matches, pick the one whose
  certificate thumbprint equals the backup's. The reader can read the thumbprint without any key, then
  fetch the matching secret.
- **Fail-closed cases:** an EKM-held key (cannot be exported — documented, unchanged); a thumbprint with
  no matching secret; a wrong password (RC4 of the PVK yields a modulus ≠ the cert's — detectable before
  any backup byte is touched).

## 5. Container-header fields still not fully named (not blocking)

`@0x08`=2 and the exact meaning of the `@0x0C` flag bits and the `(a,b)` block fields beyond "offsets
reused as the IV" are inferred, not needed to decrypt. The 4 KB region at `0x1000` in the
uncompressed-encrypted container (between header and the `SFMB` at `0x2000`) was not decoded; the block
chain and all page data decrypt without it. TDE page type `21` (96 instances, a system page) was
classified but not decoded. None affects row recovery.

## 6. Artifacts (local only; server cleaned)

`e4_pvk.py`, `e4_decrypt_backup.py`, `e4_tde_decrypt.py` (run with `E4PVKPASS` in the env — never a CLI
arg); `01_db_and_backups.sql` (full T-SQL, password placeholders); `bk_e4_plain/enc/enc_comp/tde.bak`,
`bk_e4_cert.cer`, `bk_e4_cert.pvk`, `cert_thumbprint.txt`. Cleanup done: `bk_e4` and `bk_e4_tde` dropped
(TDE turned off first), certificate `bk_e4_cert` and the master key dropped (the DMK was created by this
experiment — master had none before), all `bk_e4_*` server files `xp_delete_files`'d. An accidental
master key + `bk_e4_cert` first created in `TestDB` (the session's default DB) were dropped before the
master ones were created; `TestDB` had no user DMK before and has none now.
