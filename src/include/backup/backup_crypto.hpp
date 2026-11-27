//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_crypto.hpp
//
// Spec 087: the cryptography an encrypted or TDE backup needs, through the statically linked OpenSSL
// (D6): a certificate's thumbprint, its private key from a PVK (RC4 of our own, D6), a PFX or a PEM,
// the backup's wrapped key unwrapped (RSA-OAEP / SHA-256 / MGF1-SHA-256 over the reversed blob, E4; PKCS#1
// v1.5 the fallback, D7) into a CryptoAPI PLAINTEXTKEYBLOB, and a block or a page decrypted (AES-CBC or
// 3DES-CBC, padding off). Key bytes live in a holder that zeroises them (OPENSSL_cleanse); nothing here
// logs, prints or puts a key, an IV or a password in an error. DuckDB-free.
//===----------------------------------------------------------------------===//

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace duckdb {
namespace mssql {
namespace backup {

using Thumbprint = std::array<uint8_t, 20>;

//! The thumbprint in upper-case hex, as RESTORE HEADERONLY and sys.certificates show it.
std::string ThumbprintHex(const Thumbprint &thumbprint);

//! Bytes zeroised when the holder goes (or is reassigned). Not copyable: a key has one owner.
class SecretBytes {
public:
	SecretBytes() = default;
	explicit SecretBytes(size_t size) : bytes_(size) {}
	SecretBytes(const uint8_t *data, size_t size) : bytes_(data, data + size) {}
	SecretBytes(SecretBytes &&other) noexcept;
	SecretBytes &operator=(SecretBytes &&other) noexcept;
	SecretBytes(const SecretBytes &) = delete;
	SecretBytes &operator=(const SecretBytes &) = delete;
	~SecretBytes();

	uint8_t *data() {
		return bytes_.data();
	}
	const uint8_t *data() const {
		return bytes_.data();
	}
	size_t size() const {
		return bytes_.size();
	}
	bool empty() const {
		return bytes_.empty();
	}
	void Clear();

private:
	std::vector<uint8_t> bytes_;
};

//! A content key: a backup's (encrypted backup) or a database's (TDE's DEK), unwrapped.
enum class KeyAlgorithm : uint8_t { Aes128, Aes192, Aes256, TripleDes };

//! "aes_128" / "aes_192" / "aes_256" / "triple_des_3key", as RESTORE HEADERONLY's KeyAlgorithm names them.
const char *KeyAlgorithmName(KeyAlgorithm algorithm);

struct ContentKey {
	KeyAlgorithm algorithm = KeyAlgorithm::Aes256;
	SecretBytes key;
	//! The cipher's block: 16 for AES, 8 for 3DES.
	size_t BlockSize() const {
		return algorithm == KeyAlgorithm::TripleDes ? 8 : 16;
	}
};

//! An RSA private key (OpenSSL's EVP_PKEY), owned.
class PrivateKey {
public:
	PrivateKey();
	~PrivateKey();
	PrivateKey(PrivateKey &&other) noexcept;
	PrivateKey &operator=(PrivateKey &&other) noexcept;
	PrivateKey(const PrivateKey &) = delete;
	PrivateKey &operator=(const PrivateKey &) = delete;

	bool Valid() const {
		return key_ != nullptr;
	}
	//! The modulus' size in bytes (384 for RSA-3072).
	size_t ModulusBytes() const;
	//! Whether its public half is the certificate's (`der`), E4§4: a wrong password gives another modulus.
	bool Matches(const std::vector<uint8_t> &certificate_der) const;

	void *Native() const {
		return key_;
	}
	void Reset(void *key);

private:
	void *key_ = nullptr;  // EVP_PKEY
};

//! A certificate (DER, or PEM "CERTIFICATE") parsed: its DER and thumbprint (SHA-1 of the DER). Throws a
//! FormatError naming `what` when it is neither.
struct Certificate {
	std::vector<uint8_t> der;
	Thumbprint thumbprint{};
};
Certificate ReadCertificate(const std::vector<uint8_t> &bytes, const std::string &what);

//! A private key from its container: a Microsoft PVK (magic 0xB0B5F11E; encrypted with RC4 under
//! SHA1(salt || password)[:16], E4§2; or not encrypted), a PFX / PKCS#12, or a PEM (PKCS#8, encrypted or
//! not). `password` may be empty for an unencrypted one. Throws a FormatError naming `what` and why (never
//! the password). A PFX also gives its certificate into `pfx_certificate` when that is not null.
PrivateKey ReadPrivateKey(const std::vector<uint8_t> &bytes, const std::string &password, const std::string &what,
						  std::vector<uint8_t> *pfx_certificate = nullptr);

//! RC4 over `data` in place (PVK's cipher, D6: OpenSSL 3 keeps RC4 in its legacy provider).
void Rc4(const uint8_t *key, size_t key_size, uint8_t *data, size_t size);

//! A backup's or a database's wrapped key (`blob`, `blob_size` = the modulus' size) unwrapped with `key`:
//! RSA-OAEP / SHA-256 / MGF1-SHA-256 over the reversed blob, else PKCS#1 v1.5 (D7), into a PLAINTEXTKEYBLOB
//! that must be well formed (bType 8, version 2, a known aiKeyAlg, its key length). `what` names the backup
//! in an error ("the key does not unwrap ..." -- the wrong certificate's key, say).
ContentKey UnwrapContentKey(const PrivateKey &key, const uint8_t *blob, size_t blob_size, const std::string &what);

//! The content key a PLAINTEXTKEYBLOB holds (what an unwrap decrypts to): bType 8, version 2, a known aiKeyAlg
//! and its key's length, else a FormatError naming `what` ("does not unwrap" for a blob that is not one: the
//! wrong certificate's key). Exposed for the fuzz harness (spec 087 task 8).
ContentKey ReadPlaintextKeyBlob(const uint8_t *blob, size_t size, const std::string &what);

//! `size` bytes at `data` decrypted in place with `key`, CBC, no padding; `size` a multiple of the block,
//! `iv` the block's own (16 bytes read; 3DES takes the first 8).
void DecryptCbc(const ContentKey &key, const uint8_t *iv, uint8_t *data, size_t size);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
