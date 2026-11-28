// Spec 087 D1 - D4: the mssql_backup_key secret and its selection (backup_key_secret.hpp). [DuckDB]

#include "backup/backup_key_secret.hpp"

#include "duckdb/common/string_util.hpp"

#include "backup/backup_bytes.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

namespace duckdb {
namespace mssql {
namespace backup {

namespace {

constexpr const char *TYPE = "mssql_backup_key";
constexpr const char *CERTIFICATE = "certificate";
constexpr const char *CERTIFICATE_DATA = "certificate_data";
constexpr const char *PRIVATE_KEY = "private_key";
constexpr const char *PRIVATE_KEY_DATA = "private_key_data";
constexpr const char *PASSWORD = "password";
constexpr const char *THUMBPRINT = "thumbprint";
//! A key container or certificate larger than this is not one (a PVK of RSA-16384 is under 10 KB).
constexpr idx_t MAX_KEY_FILE = 1 << 20;

std::vector<uint8_t> ReadKeyFile(ClientContext &context, const std::string &path, const char *what) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	const idx_t size = static_cast<idx_t>(handle->GetFileSize());
	if (size == 0 || size > MAX_KEY_FILE) {
		throw InvalidInputException("CREATE SECRET (TYPE %s): the %s \"%s\" is %llu bytes: not a %s", TYPE, what, path,
									static_cast<unsigned long long>(size), what);
	}
	std::vector<uint8_t> bytes(size);
	handle->Read(bytes.data(), size, 0);
	return bytes;
}

std::vector<uint8_t> FromBase64(const std::string &text, const char *what) {
	try {
		std::string decoded = Blob::FromBase64(string_t(text));
		WipeOnExit wipe(nullptr, &decoded);
		return std::vector<uint8_t>(decoded.begin(), decoded.end());
	} catch (std::exception &) {
		throw InvalidInputException("CREATE SECRET (TYPE %s): %s is not base64", TYPE, what);
	}
}

std::string ToBase64(const std::vector<uint8_t> &bytes) {
	return Blob::ToBase64(string_t(reinterpret_cast<const char *>(bytes.data()), static_cast<uint32_t>(bytes.size())));
}

std::string Option(const CreateSecretInput &input, const char *key) {
	auto it = input.options.find(key);
	if (it == input.options.end() || it->second.IsNull()) {
		return std::string();
	}
	return it->second.ToString();
}

//! The material of `input`: the certificate (DER) and the key container's bytes, from the paths or the
//! base64 data, exactly one of each (a PFX may stand for the certificate too).
void Material(ClientContext &context, const CreateSecretInput &input, std::vector<uint8_t> &certificate,
			  std::vector<uint8_t> &container) {
	const std::string cert_path = Option(input, CERTIFICATE);
	const std::string cert_data = Option(input, CERTIFICATE_DATA);
	const std::string key_path = Option(input, PRIVATE_KEY);
	const std::string key_data = Option(input, PRIVATE_KEY_DATA);
	if (!cert_path.empty() && !cert_data.empty()) {
		throw InvalidInputException("CREATE SECRET (TYPE %s): give certificate or certificate_data, not both", TYPE);
	}
	if (key_path.empty() == key_data.empty()) {
		throw InvalidInputException(
			"CREATE SECRET (TYPE %s): give one of private_key (a PVK, PFX or PEM file) or private_key_data (its "
			"bytes, base64)",
			TYPE);
	}
	if (!cert_path.empty()) {
		certificate = ReadKeyFile(context, cert_path, "certificate");
	} else if (!cert_data.empty()) {
		certificate = FromBase64(cert_data, CERTIFICATE_DATA);
	}
	container =
		key_path.empty() ? FromBase64(key_data, PRIVATE_KEY_DATA) : ReadKeyFile(context, key_path, "private key");
}

unique_ptr<BaseSecret> CreateBackupKeySecret(ClientContext &context, CreateSecretInput &input) {
	for (const auto &option : input.options) {
		const std::string key = option.first;
		if (key != CERTIFICATE && key != CERTIFICATE_DATA && key != PRIVATE_KEY && key != PRIVATE_KEY_DATA &&
			key != PASSWORD) {
			throw InvalidInputException(
				"CREATE SECRET (TYPE %s): '%s' is not a parameter (certificate, "
				"certificate_data, private_key, private_key_data, password, scope)",
				TYPE, key);
		}
	}
	std::vector<uint8_t> certificate_bytes;
	std::vector<uint8_t> container;
	Material(context, input, certificate_bytes, container);
	std::string password = Option(input, PASSWORD);
	WipeOnExit wipe(&container, &password);
	Certificate certificate;
	try {
		std::vector<uint8_t> pfx_certificate;
		PrivateKey key = ReadPrivateKey(container, password, "the private key", &pfx_certificate);
		if (certificate_bytes.empty()) {
			if (pfx_certificate.empty()) {
				throw FormatError("no certificate: give certificate or certificate_data (only a PFX carries its own)");
			}
			certificate_bytes = pfx_certificate;
		}
		certificate = ReadCertificate(certificate_bytes, "the certificate");
		if (!key.Matches(certificate.der)) {
			throw FormatError(
				"the private key is not the certificate's (its modulus differs): another certificate's "
				"key, or a wrong password");
		}
	} catch (FormatError &e) {
		throw InvalidInputException("CREATE SECRET (TYPE %s): %s", TYPE, e.what());
	}
	auto result = make_uniq<KeyValueSecret>(input.scope, input.type, input.provider, input.name);
	result->secret_map[CERTIFICATE] = Value(ToBase64(certificate.der));
	result->secret_map[PRIVATE_KEY] = Value(ToBase64(container));
	result->secret_map[PASSWORD] = Value(password);
	result->secret_map[THUMBPRINT] = Value(ThumbprintHex(certificate.thumbprint));
	result->redact_keys.insert(PRIVATE_KEY);
	result->redact_keys.insert(PASSWORD);
	return std::move(result);
}

//! A matching secret as the selection needs it: its name, storage, thumbprint and score -- never its material
//! (review of #670: every candidate was cloned, password included, even for a thumbprint compare). The chosen
//! one's material is fetched by name from its storage when a key is unwrapped (SelectContentKey).
struct Candidate {
	std::string name;
	std::string storage;
	std::string thumbprint;
	int64_t score = 0;
};

}  // namespace

void RegisterBackupKeySecret(ExtensionLoader &loader) {
	SecretType type;
	type.name = TYPE;
	type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	type.default_provider = "config";
	loader.RegisterSecretType(type);

	CreateSecretFunction create;
	create.secret_type = TYPE;
	create.provider = "config";
	create.function = CreateBackupKeySecret;
	create.named_parameters[CERTIFICATE] = LogicalType::VARCHAR;
	create.named_parameters[CERTIFICATE_DATA] = LogicalType::VARCHAR;
	create.named_parameters[PRIVATE_KEY] = LogicalType::VARCHAR;
	create.named_parameters[PRIVATE_KEY_DATA] = LogicalType::VARCHAR;
	create.named_parameters[PASSWORD] = LogicalType::VARCHAR;
	loader.RegisterFunction(std::move(create));
}

//! D3 over the `mssql_backup_key` secrets: those whose scope matches `path`, the longest scope's kept in
//! `candidates`; `matching` counts every one that matched; `names` lists them all (name and thumbprint).
static void FindCandidates(ClientContext &context, const std::string &path, std::vector<Candidate> &candidates,
						   size_t &matching, std::string &names) {
	auto &manager = SecretManager::Get(context);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	int64_t best = NumericLimits<int64_t>::Minimum();
	matching = 0;
	for (auto &entry : manager.AllSecrets(transaction)) {
		if (!entry.secret || entry.secret->GetType().GetIdentifierName() != TYPE) {
			continue;
		}
		const int64_t score = entry.secret->MatchScore(path);
		if (score == NumericLimits<int64_t>::Minimum()) {
			continue;
		}
		matching++;
		const auto &kv = dynamic_cast<const KeyValueSecret &>(*entry.secret);
		const std::string thumbprint = kv.TryGetValue(THUMBPRINT).ToString();
		names += (names.empty() ? "" : ", ") + entry.secret->GetName().GetIdentifierName() + " (" + thumbprint + ")";
		if (score < best) {
			continue;
		}
		if (score > best) {
			candidates.clear();
			best = score;
		}
		Candidate c;
		c.name = entry.secret->GetName().GetIdentifierName();
		c.score = score;
		c.storage = entry.storage_mode;
		c.thumbprint = thumbprint;
		candidates.push_back(std::move(c));
	}
}

//! D3 2 / 3: among the longest scope's, the one whose thumbprint is the backup's.
static const Candidate *Chosen(const std::vector<Candidate> &candidates, const std::string &wanted) {
	for (const auto &c : candidates) {
		if (c.thumbprint == wanted) {
			return &c;
		}
	}
	return nullptr;
}

//! D10 (owner, 2026-11-27): reading the data needs the key -- named before any row: the thumbprint, the secrets of
//! the type that match the path, and the form of the secret that would read it.
[[noreturn]] static void RefuseNoKey(const std::string &path, const std::string &what, const std::string &wanted,
									 const std::vector<Candidate> &candidates, size_t matching,
									 const std::string &names) {
	throw MissingBackupKey(StringUtil::Format(
		"mssql backup: \"%s\" is %s encrypted by the certificate with thumbprint %s, and no %s secret whose "
		"scope matches the path holds its key (%s): CREATE SECRET (TYPE %s, CERTIFICATE '<that certificate, "
		".cer>', PRIVATE_KEY '<its key, .pvk / .pfx / .pem>', PASSWORD '...', SCOPE '<a prefix of the path>'); "
		"mssql_backup_info reads what the file says in clear without it",
		path, what, wanted, TYPE,
		matching == 0 ? std::string("none matches")
					  : std::to_string(matching) + " match: " + names +
							(candidates.empty() ? "" : "; D3 takes the longest scope's, none of which is it"),
		TYPE));
}

void RequireBackupKeySecret(ClientContext &context, const std::string &path, const Thumbprint &thumbprint,
							const std::string &what) {
	const std::string wanted = ThumbprintHex(thumbprint);
	std::vector<Candidate> candidates;
	size_t matching = 0;
	std::string names;
	FindCandidates(context, path, candidates, matching, names);
	if (!Chosen(candidates, wanted)) {
		RefuseNoKey(path, what, wanted, candidates, matching, names);
	}
}

bool HasBackupKeySecret(ClientContext &context, const std::string &path, const Thumbprint &thumbprint) {
	std::vector<Candidate> candidates;
	size_t matching = 0;
	std::string names;
	FindCandidates(context, path, candidates, matching, names);
	return Chosen(candidates, ThumbprintHex(thumbprint)) != nullptr;
}

std::shared_ptr<const ContentKey> SelectContentKey(ClientContext &context, const std::string &path,
												   const Thumbprint &thumbprint, const std::vector<uint8_t> &wrapped,
												   const std::string &what) {
	const std::string wanted = ThumbprintHex(thumbprint);
	std::vector<Candidate> candidates;
	size_t matching = 0;
	std::string names;
	FindCandidates(context, path, candidates, matching, names);
	const Candidate *chosen = Chosen(candidates, wanted);
	if (!chosen) {
		RefuseNoKey(path, what, wanted, candidates, matching, names);
	}
	// The chosen secret alone, from its own storage (one name may be in two), its values read where the secret
	// keeps them: no Value or string copy of ours but the two wiped below (the SecretEntry the lookup returns is
	// DuckDB's own copy, as AllSecrets' are).
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	auto entry = SecretManager::Get(context).GetSecretByName(transaction, chosen->name, chosen->storage);
	if (!entry || !entry->secret) {
		throw InvalidInputException("mssql backup: the %s secret %s of storage %s is gone", TYPE, chosen->name,
									chosen->storage);
	}
	const auto &kv = dynamic_cast<const KeyValueSecret &>(*entry->secret);
	// Dropped and created again under the same name since the selection (review of #670): another key, named.
	if (kv.TryGetValue(THUMBPRINT).ToString() != chosen->thumbprint) {
		throw InvalidInputException(
			"mssql backup: the %s secret %s changed since it was selected (its thumbprint is "
			"no longer %s): run the statement again",
			TYPE, chosen->name, chosen->thumbprint);
	}
	auto container_value = kv.secret_map.find(PRIVATE_KEY);
	auto password_value = kv.secret_map.find(PASSWORD);
	if (container_value == kv.secret_map.end() || container_value->second.IsNull()) {
		throw InvalidInputException("mssql backup: the %s secret %s holds no private key", TYPE, chosen->name);
	}
	std::vector<uint8_t> container = FromBase64(StringValue::Get(container_value->second), PRIVATE_KEY);
	std::string password = password_value == kv.secret_map.end() || password_value->second.IsNull()
							   ? std::string()
							   : StringValue::Get(password_value->second);
	WipeOnExit wipe(&container, &password);
	try {
		PrivateKey key = ReadPrivateKey(container, password, "secret " + chosen->name + "'s private key");
		return std::make_shared<const ContentKey>(
			UnwrapContentKey(key, wrapped.data(), wrapped.size(), "mssql backup: \"" + path + "\" (" + what + ")"));
	} catch (FormatError &e) {
		throw InvalidInputException("%s", e.what());
	}
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
