//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_key_secret.hpp
//
// Spec 087 D1 - D4: the `mssql_backup_key` secret -- the only way key material reaches the reader -- and
// the selection of one for a backup file: the longest matching scope first, the thumbprint second, the
// chosen one's thumbprint required to be the backup's. [DuckDB]
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_crypto.hpp"
#include "duckdb/common/exception.hpp"

#include <memory>
#include <string>
#include <vector>

namespace duckdb {
class ClientContext;
class ExtensionLoader;

namespace mssql {
namespace backup {

//! D10's refusal: a backup whose key no secret the path selects holds. Its own type so mssql_backup_info, which
//! opens a backup as any call does, can fall back to what the file says in clear on exactly this (review of
//! 087: it used to read every file's clear headers first, on every call).
class MissingBackupKey : public InvalidInputException {
public:
	explicit MissingBackupKey(const std::string &message) : InvalidInputException(message) {}
};

//! The secret type `mssql_backup_key` (provider `config`): `certificate` / `certificate_data`, `private_key`
//! / `private_key_data`, `password` (redacted), and the secret's scope. D2: the files are read and checked
//! at CREATE SECRET -- the certificate parsed, its thumbprint computed, the private key opened with the
//! password and its modulus matched to the certificate's -- so a wrong password fails there, before any
//! backup is touched. The secret keeps the certificate, the still password-protected key container and the
//! password (both redacted), and shows the thumbprint.
void RegisterBackupKeySecret(ExtensionLoader &loader);

//! D3 / D4, on every call: the content key of the encrypted backup file `path` -- or of a TDE database, its
//! DEK (`what` names which in errors) -- whose wrapped key `wrapped` the certificate `thumbprint` names: the
//! `mssql_backup_key` secrets whose scope matches the path, the longest scope's, the one among them whose
//! thumbprint is the backup's; its private key unwraps the key. A file no such secret holds the key of is
//! refused by name (the path, the thumbprint, how many secrets matched the path), never a fall-through to a
//! secret outside the matching scopes.
//! D10: whether D3 selects a secret for `path` and `thumbprint` (a probe: never unwraps, never throws for none).
bool HasBackupKeySecret(ClientContext &context, const std::string &path, const Thumbprint &thumbprint);
//! D4: the secret D3 selects for `path` still holds the certificate `thumbprint` names, else D10's refusal (the
//! thumbprint, the secrets that match, CREATE SECRET's form). Selection only: nothing is decrypted, so a key
//! already unwrapped (a cached entry's, an attached backup's) is used only while a secret still holds it.
void RequireBackupKeySecret(ClientContext &context, const std::string &path, const Thumbprint &thumbprint,
							const std::string &what);

std::shared_ptr<const ContentKey> SelectContentKey(ClientContext &context, const std::string &path,
												   const Thumbprint &thumbprint, const std::vector<uint8_t> &wrapped,
												   const std::string &what);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
