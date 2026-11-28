// Spec 087: the cryptography of encrypted and TDE backups (backup_crypto.hpp). DuckDB-free.

#include "backup/backup_crypto.hpp"

#include "backup/backup_bytes.hpp"

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace duckdb {
namespace mssql {
namespace backup {

namespace {

constexpr uint32_t PVK_MAGIC = 0xB0B5F11E;
// The CryptoAPI ALG_IDs, under names of their own: wincrypt.h defines CALG_* as macros (review of #670), and
// the first include of windows.h here would turn the declarations into syntax errors.
constexpr uint32_t CAPI_ALG_RSA_KEYX = 0xA400;
constexpr uint32_t CAPI_ALG_AES_128 = 0x660E;
constexpr uint32_t CAPI_ALG_AES_192 = 0x660F;
constexpr uint32_t CAPI_ALG_AES_256 = 0x6610;
constexpr uint32_t CAPI_ALG_3DES = 0x6603;

uint32_t U32(const uint8_t *p) {
	return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
		   static_cast<uint32_t>(p[3]) << 24;
}

//! OpenSSL's error queue dropped (never shown: it may name what a guessed password decoded to).
void DropErrors() {
	ERR_clear_error();
}

struct BnDeleter {
	void operator()(BIGNUM *b) const {
		BN_clear_free(b);
	}
};
using Bn = std::unique_ptr<BIGNUM, BnDeleter>;

//! A little-endian CryptoAPI integer of `size` bytes as a BIGNUM.
Bn LittleEndian(const uint8_t *p, size_t size) {
	Bn bn(BN_lebin2bn(p, static_cast<int>(size), nullptr));
	if (!bn) {
		DropErrors();
		throw FormatError("backup key: out of memory reading an RSA component");
	}
	return bn;
}

//! A CryptoAPI PRIVATEKEYBLOB (after its 8-byte BLOBHEADER: "RSA2", bit length, public exponent, then
//! modulus, prime1, prime2, exponent1, exponent2, coefficient, private exponent) as an EVP_PKEY.
PrivateKey PrivateKeyBlob(const uint8_t *body, size_t size, const std::string &what) {
	if (size < 12 || std::memcmp(body, "RSA2", 4) != 0) {
		throw FormatError(what + ": the private key blob is not an RSA private key (no RSA2 magic)");
	}
	const uint32_t bits = U32(body + 4);
	if (bits < 1024 || bits > 16384 || bits % 16 != 0) {
		throw FormatError(what + ": an RSA key of " + std::to_string(bits) + " bits");
	}
	const size_t n8 = bits / 8;
	const size_t n16 = bits / 16;
	const size_t need = 12 + n8 + 5 * n16 + n8;
	if (size < need) {
		throw FormatError(what + ": the private key blob holds " + std::to_string(size) + " bytes where " +
						  std::to_string(bits) + "-bit RSA takes " + std::to_string(need));
	}
	size_t o = 12;
	Bn e(BN_new());
	if (!e || BN_set_word(e.get(), U32(body + 8)) != 1) {
		DropErrors();
		throw FormatError(what + ": OpenSSL could not hold the RSA public exponent");
	}
	Bn n = LittleEndian(body + o, n8);
	o += n8;
	Bn p = LittleEndian(body + o, n16);
	o += n16;
	Bn q = LittleEndian(body + o, n16);
	o += n16;
	Bn dp = LittleEndian(body + o, n16);
	o += n16;
	Bn dq = LittleEndian(body + o, n16);
	o += n16;
	Bn qi = LittleEndian(body + o, n16);
	o += n16;
	Bn d = LittleEndian(body + o, n8);

	OSSL_PARAM_BLD *bld = OSSL_PARAM_BLD_new();
	bool ok = bld && OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_N, n.get()) &&
			  OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_E, e.get()) &&
			  OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_D, d.get()) &&
			  OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_FACTOR1, p.get()) &&
			  OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_FACTOR2, q.get()) &&
			  OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_EXPONENT1, dp.get()) &&
			  OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_EXPONENT2, dq.get()) &&
			  OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_COEFFICIENT1, qi.get());
	OSSL_PARAM *params = ok ? OSSL_PARAM_BLD_to_param(bld) : nullptr;
	EVP_PKEY_CTX *ctx = params ? EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr) : nullptr;
	EVP_PKEY *pkey = nullptr;
	ok = ctx && EVP_PKEY_fromdata_init(ctx) == 1 && EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_KEYPAIR, params) == 1;
	EVP_PKEY_CTX_free(ctx);
	// The params hold copies of d, p, q, dp, dq and qi: wiped before they are freed (OSSL_PARAM_clear_free is not
	// in every OpenSSL 3 this builds with).
	for (OSSL_PARAM *param = params; param && param->key; param++) {
		if (param->data && param->data_size) {
			OPENSSL_cleanse(param->data, param->data_size);
		}
	}
	OSSL_PARAM_free(params);
	OSSL_PARAM_BLD_free(bld);
	if (!ok) {
		EVP_PKEY_free(pkey);
		DropErrors();
		throw FormatError(what + ": the private key blob does not make an RSA key");
	}
	PrivateKey key;
	key.Reset(pkey);
	return key;
}

PrivateKey ReadPvk(const std::vector<uint8_t> &bytes, const std::string &password, const std::string &what) {
	if (bytes.size() < 24) {
		throw FormatError(what + ": too short for a PVK header");
	}
	const uint32_t encrypted = U32(bytes.data() + 12);
	const uint32_t salt_size = U32(bytes.data() + 16);
	const uint32_t key_size = U32(bytes.data() + 20);
	if (encrypted > 1 || (encrypted == 1 && salt_size == 0) || salt_size > 64 ||
		key_size > bytes.size() - 24 - std::min<size_t>(salt_size, bytes.size() - 24) || key_size < 12) {
		throw FormatError(what + ": a PVK whose salt (" + std::to_string(salt_size) + ") or key (" +
						  std::to_string(key_size) + ") length does not fit its " + std::to_string(bytes.size()) +
						  " bytes");
	}
	// 12 bytes at least: the 8-byte BLOBHEADER, then the RSAPUBKEY's "RSA2" magic, compared below (review of
	// #670: 8 to 11 bytes read past the blob).
	const uint8_t *salt = bytes.data() + 24;
	SecretBytes blob(salt + salt_size, key_size);
	// The BLOBHEADER stays in clear: bType 7 (PRIVATEKEYBLOB), version 2, aiKeyAlg CAPI_ALG_RSA_KEYX.
	if (blob.data()[0] != 7 || blob.data()[1] != 2 || U32(blob.data() + 4) != CAPI_ALG_RSA_KEYX) {
		throw FormatError(what + ": a PVK whose key is not an RSA key-exchange PRIVATEKEYBLOB");
	}
	if (encrypted) {
		if (password.empty()) {
			throw FormatError(what + ": the PVK is encrypted and the secret gives no password");
		}
		SecretBytes material(salt_size + password.size());
		std::memcpy(material.data(), salt, salt_size);
		std::memcpy(material.data() + salt_size, password.data(), password.size());
		uint8_t digest[20];
		unsigned int digest_size = 0;
		if (EVP_Digest(material.data(), material.size(), digest, &digest_size, EVP_sha1(), nullptr) != 1) {
			DropErrors();
			throw FormatError(what + ": SHA-1 is not available");
		}
		Rc4(digest, 16, blob.data() + 8, blob.size() - 8);
		OPENSSL_cleanse(digest, sizeof(digest));
	}
	if (std::memcmp(blob.data() + 8, "RSA2", 4) != 0) {
		throw FormatError(what +
						  ": the PVK does not decrypt to an RSA private key: a wrong password, or not "
						  "the certificate's private key");
	}
	return PrivateKeyBlob(blob.data() + 8, blob.size() - 8, what);
}

PrivateKey ReadPfx(const std::vector<uint8_t> &bytes, const std::string &password, const std::string &what,
				   std::vector<uint8_t> *certificate) {
	const unsigned char *p = bytes.data();
	PKCS12 *p12 = d2i_PKCS12(nullptr, &p, static_cast<long>(bytes.size()));
	if (!p12) {
		DropErrors();
		throw FormatError(what + ": not a PFX (PKCS#12)");
	}
	EVP_PKEY *pkey = nullptr;
	X509 *cert = nullptr;
	const int ok = PKCS12_parse(p12, password.c_str(), &pkey, &cert, nullptr);
	PKCS12_free(p12);
	if (ok != 1 || !pkey) {
		// The whole queue is asked (review of 087): the last error is PKCS12's own (a parse or PBE error); the
		// cipher's "unsupported", from EVP or a provider, lies deeper.
		bool unsupported = false;
		for (unsigned long err = ERR_get_error(); err != 0; err = ERR_get_error()) {
			unsupported = unsupported || ERR_GET_REASON(err) == EVP_R_UNSUPPORTED_ALGORITHM ||
						  ERR_GET_REASON(err) == ERR_R_UNSUPPORTED || ERR_GET_LIB(err) == ERR_LIB_PROV;
		}
		X509_free(cert);
		EVP_PKEY_free(pkey);
		throw FormatError(what + (unsupported
									  ? ": the PFX is protected by a cipher this build does not carry (RC2 / RC4, an "
										"old Windows export): re-export it, e.g. `openssl pkcs12 -export -keypbe "
										"AES-256-CBC -certpbe AES-256-CBC`"
									  : ": the PFX does not open: a wrong password, or not a PFX"));
	}
	if (certificate && cert) {
		unsigned char *der = nullptr;
		const int n = i2d_X509(cert, &der);
		if (n > 0) {
			certificate->assign(der, der + n);
		}
		OPENSSL_free(der);
	}
	X509_free(cert);
	PrivateKey key;
	key.Reset(pkey);
	return key;
}

int PemPassword(char *buf, int size, int, void *u) {
	const std::string &password = *static_cast<const std::string *>(u);
	if (password.size() > static_cast<size_t>(size)) {
		return -1;
	}
	std::memcpy(buf, password.data(), password.size());
	return static_cast<int>(password.size());
}

PrivateKey ReadPem(const std::vector<uint8_t> &bytes, const std::string &password, const std::string &what) {
	BIO *bio = BIO_new_mem_buf(bytes.data(), static_cast<int>(bytes.size()));
	std::string pw = password;
	EVP_PKEY *pkey = bio ? PEM_read_bio_PrivateKey(bio, nullptr, PemPassword, &pw) : nullptr;
	BIO_free(bio);
	OPENSSL_cleanse(&pw[0], pw.size());
	if (!pkey) {
		DropErrors();
		throw FormatError(what + ": the PEM private key does not open: a wrong password, or not a private key");
	}
	PrivateKey key;
	key.Reset(pkey);
	return key;
}

bool StartsWith(const std::vector<uint8_t> &bytes, const char *text) {
	const size_t n = std::strlen(text);
	return bytes.size() >= n && std::memcmp(bytes.data(), text, n) == 0;
}

//! Whether the first 4 KB of `bytes` hold `text`, searched in place: no copy of a key container (an unencrypted
//! PEM is the private key's text) is made.
bool HoldsText(const std::vector<uint8_t> &bytes, const char *text) {
	const size_t n = std::strlen(text);
	const size_t end = std::min<size_t>(bytes.size(), 4096);
	if (n == 0 || n > end) {
		return n == 0;
	}
	for (size_t i = 0; i + n <= end; i++) {
		if (std::memcmp(bytes.data() + i, text, n) == 0) {
			return true;
		}
	}
	return false;
}

}  // namespace

void Wipe(void *data, size_t size) {
	OPENSSL_cleanse(data, size);
}

std::string ThumbprintHex(const Thumbprint &thumbprint) {
	static const char *const HEX = "0123456789ABCDEF";
	std::string s;
	for (const uint8_t b : thumbprint) {
		s += HEX[b >> 4];
		s += HEX[b & 15];
	}
	return s;
}

SecretBytes::SecretBytes(SecretBytes &&other) noexcept : bytes_(std::move(other.bytes_)) {
	other.bytes_.clear();
}

SecretBytes &SecretBytes::operator=(SecretBytes &&other) noexcept {
	if (this != &other) {
		Clear();
		bytes_ = std::move(other.bytes_);
		other.bytes_.clear();
	}
	return *this;
}

SecretBytes::~SecretBytes() {
	Clear();
}

void SecretBytes::Clear() {
	if (!bytes_.empty()) {
		OPENSSL_cleanse(bytes_.data(), bytes_.size());
	}
	bytes_.clear();
}

const char *KeyAlgorithmName(KeyAlgorithm algorithm) {
	switch (algorithm) {
	case KeyAlgorithm::Aes128:
		return "aes_128";
	case KeyAlgorithm::Aes192:
		return "aes_192";
	case KeyAlgorithm::Aes256:
		return "aes_256";
	case KeyAlgorithm::TripleDes:
		return "triple_des_3key";
	}
	return "unknown";
}

PrivateKey::PrivateKey() = default;

PrivateKey::~PrivateKey() {
	Reset(nullptr);
}

PrivateKey::PrivateKey(PrivateKey &&other) noexcept : key_(other.key_) {
	other.key_ = nullptr;
}

PrivateKey &PrivateKey::operator=(PrivateKey &&other) noexcept {
	if (this != &other) {
		Reset(other.key_);
		other.key_ = nullptr;
	}
	return *this;
}

void PrivateKey::Reset(void *key) {
	if (key_ && key_ != key) {
		EVP_PKEY_free(static_cast<EVP_PKEY *>(key_));
	}
	key_ = key;
}

size_t PrivateKey::ModulusBytes() const {
	return key_ ? static_cast<size_t>(EVP_PKEY_get_size(static_cast<EVP_PKEY *>(key_))) : 0;
}

bool PrivateKey::Matches(const std::vector<uint8_t> &certificate_der) const {
	if (!key_) {
		return false;
	}
	const unsigned char *p = certificate_der.data();
	X509 *cert = d2i_X509(nullptr, &p, static_cast<long>(certificate_der.size()));
	if (!cert) {
		DropErrors();
		return false;
	}
	EVP_PKEY *pub = X509_get0_pubkey(cert);
	const bool same = pub && EVP_PKEY_eq(pub, static_cast<EVP_PKEY *>(key_)) == 1;
	X509_free(cert);
	DropErrors();
	return same;
}

Certificate ReadCertificate(const std::vector<uint8_t> &bytes, const std::string &what) {
	Certificate c;
	X509 *cert = nullptr;
	if (HoldsText(bytes, "-----BEGIN CERTIFICATE-----")) {
		BIO *bio = BIO_new_mem_buf(bytes.data(), static_cast<int>(bytes.size()));
		cert = bio ? PEM_read_bio_X509(bio, nullptr, nullptr, nullptr) : nullptr;
		BIO_free(bio);
	} else {
		const unsigned char *p = bytes.data();
		cert = d2i_X509(nullptr, &p, static_cast<long>(bytes.size()));
	}
	if (!cert) {
		DropErrors();
		throw FormatError(what + ": not a certificate (DER or PEM)");
	}
	unsigned char *der = nullptr;
	const int n = i2d_X509(cert, &der);
	X509_free(cert);
	if (n <= 0) {
		DropErrors();
		throw FormatError(what + ": the certificate does not re-encode");
	}
	c.der.assign(der, der + n);
	OPENSSL_free(der);
	unsigned int size = 0;
	if (EVP_Digest(c.der.data(), c.der.size(), c.thumbprint.data(), &size, EVP_sha1(), nullptr) != 1 || size != 20) {
		DropErrors();
		throw FormatError(what + ": SHA-1 is not available");
	}
	return c;
}

PrivateKey ReadPrivateKey(const std::vector<uint8_t> &bytes, const std::string &password, const std::string &what,
						  std::vector<uint8_t> *pfx_certificate) {
	if (bytes.size() >= 4 && U32(bytes.data()) == PVK_MAGIC) {
		return ReadPvk(bytes, password, what);
	}
	if (HoldsText(bytes, "-----BEGIN")) {
		return ReadPem(bytes, password, what);
	}
	if (StartsWith(bytes, "\x30")) {  // a DER SEQUENCE: PKCS#12
		return ReadPfx(bytes, password, what, pfx_certificate);
	}
	throw FormatError(what + ": not a private key container this reader knows (PVK, PFX or PEM)");
}

void Rc4(const uint8_t *key, size_t key_size, uint8_t *data, size_t size) {
	uint8_t s[256];
	for (int i = 0; i < 256; i++) {
		s[i] = static_cast<uint8_t>(i);
	}
	uint8_t j = 0;
	for (int i = 0; i < 256; i++) {
		j = static_cast<uint8_t>(j + s[i] + key[i % key_size]);
		std::swap(s[i], s[j]);
	}
	uint8_t a = 0;
	uint8_t b = 0;
	for (size_t k = 0; k < size; k++) {
		a = static_cast<uint8_t>(a + 1);
		b = static_cast<uint8_t>(b + s[a]);
		std::swap(s[a], s[b]);
		data[k] ^= s[static_cast<uint8_t>(s[a] + s[b])];
	}
	OPENSSL_cleanse(s, sizeof(s));
}

ContentKey UnwrapContentKey(const PrivateKey &key, const uint8_t *blob, size_t blob_size, const std::string &what) {
	if (!key.Valid() || blob_size != key.ModulusBytes()) {
		throw FormatError(what + ": the wrapped key is " + std::to_string(blob_size) +
						  " bytes, not the RSA key's modulus size (" + std::to_string(key.ModulusBytes()) + ")");
	}
	std::vector<uint8_t> reversed(blob, blob + blob_size);
	std::reverse(reversed.begin(), reversed.end());	 // CryptoAPI is little-endian (E4)
	auto *pkey = static_cast<EVP_PKEY *>(key.Native());
	// Two wraps, by version (measured, review of #670): SQL Server 2025's CERTIFICATE_OAEP_256 is RSA-OAEP with
	// SHA-256 / MGF1-SHA-256 (E4 / E4b); 2019 and 2022 (EncryptorType CERTIFICATE, the backup's key and a TDE
	// database's DEK alike) wrap under PKCS#1 v1.5, `00 02 ... 00` then the 44-byte key blob. OAEP is tried first,
	// then v1.5, and only with OpenSSL's implicit rejection on: a padding that does not check gives a
	// deterministic pseudo-random result instead of an error (the Marvin / Bleichenbacher defence). So whatever a
	// crafted backup sends, the one message below is all that comes out -- for a wrong padding, a wrong key and a
	// blob that is not a key alike, never which of them it was. Without implicit rejection (an OpenSSL before 3.2)
	// v1.5 is not tried at all.
	const std::string does_not_unwrap = what +
										": the key does not unwrap with the secret's private key (not the "
										"certificate the backup was encrypted with)";
	auto decrypt = [&](bool oaep, SecretBytes &plain) {
		EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(pkey, nullptr);
		bool ok = ctx && EVP_PKEY_decrypt_init(ctx) == 1;
		if (oaep) {
			ok = ok && EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) == 1 &&
				 EVP_PKEY_CTX_set_rsa_oaep_md(ctx, EVP_sha256()) == 1 &&
				 EVP_PKEY_CTX_set_rsa_mgf1_md(ctx, EVP_sha256()) == 1;
		} else {
			ok = ok && EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING) == 1 &&
				 EVP_PKEY_CTX_ctrl_str(ctx, "rsa_pkcs1_implicit_rejection", "1") > 0;
		}
		size_t out_size = 0;
		ok = ok && EVP_PKEY_decrypt(ctx, nullptr, &out_size, reversed.data(), reversed.size()) == 1;
		if (ok) {
			SecretBytes out(out_size);
			if (EVP_PKEY_decrypt(ctx, out.data(), &out_size, reversed.data(), reversed.size()) == 1) {
				plain = SecretBytes(out.data(), out_size);
			}
		}
		EVP_PKEY_CTX_free(ctx);
		DropErrors();
	};
	SecretBytes plain;
	decrypt(true, plain);
	if (plain.empty()) {
		decrypt(false, plain);
	}
	if (plain.empty()) {
		throw FormatError(does_not_unwrap);
	}
	try {
		return ReadPlaintextKeyBlob(plain.data(), plain.size(), what);
	} catch (FormatError &) {
		throw FormatError(does_not_unwrap);	 // under v1.5 a padding that did not check reads as no key blob
	}
}

ContentKey ReadPlaintextKeyBlob(const uint8_t *p, size_t size, const std::string &what) {
	// PLAINTEXTKEYBLOB: bType 8, bVersion 2, reserved, aiKeyAlg, then u32 the key's length and the key (E4b: 44
	// bytes whatever the key's length, the rest zeros).
	// One message for every blob this reader does not take, naming none of its bytes (review of 087): what an
	// RSA unwrap gave is never echoed.
	const std::string not_a_key = what +
								  ": the unwrapped key is not a key blob this reader knows (an AES-128 / "
								  "192 / 256 or TRIPLE_DES_3KEY PLAINTEXTKEYBLOB)";
	if (size < 12 || p[0] != 8 || p[1] != 2) {
		throw FormatError(not_a_key);
	}
	const uint32_t alg = U32(p + 4);
	const uint32_t length = U32(p + 8);
	ContentKey content;
	uint32_t expected = 0;
	switch (alg) {
	case CAPI_ALG_AES_128:
		content.algorithm = KeyAlgorithm::Aes128;
		expected = 16;
		break;
	case CAPI_ALG_AES_192:
		content.algorithm = KeyAlgorithm::Aes192;
		expected = 24;
		break;
	case CAPI_ALG_AES_256:
		content.algorithm = KeyAlgorithm::Aes256;
		expected = 32;
		break;
	case CAPI_ALG_3DES:
		content.algorithm = KeyAlgorithm::TripleDes;
		expected = 24;
		break;
	default:
		throw FormatError(not_a_key);
	}
	if (length != expected || size < 12 + length) {
		throw FormatError(not_a_key);
	}
	content.key = SecretBytes(p + 12, length);
	return content;
}

void DecryptCbc(const ContentKey &key, const uint8_t *iv, uint8_t *data, size_t size) {
	const EVP_CIPHER *cipher = nullptr;
	switch (key.algorithm) {
	case KeyAlgorithm::Aes128:
		cipher = EVP_aes_128_cbc();
		break;
	case KeyAlgorithm::Aes192:
		cipher = EVP_aes_192_cbc();
		break;
	case KeyAlgorithm::Aes256:
		cipher = EVP_aes_256_cbc();
		break;
	case KeyAlgorithm::TripleDes:
		cipher = EVP_des_ede3_cbc();
		break;
	}
	if (!cipher || key.key.size() != static_cast<size_t>(EVP_CIPHER_get_key_length(cipher))) {
		// A ContentKey is a public struct (a fuzz harness builds one): never a key shorter than its cipher reads.
		throw FormatError("backup: a " + std::to_string(key.key.size()) + "-byte key for " +
						  KeyAlgorithmName(key.algorithm) + ", not its cipher's length");
	}
	if (size % key.BlockSize() != 0) {
		throw FormatError("backup: " + std::to_string(size) + " encrypted bytes, not a whole number of " +
						  std::to_string(key.BlockSize()) + "-byte blocks");
	}
	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	int out = 0;
	int last = 0;
	const bool ok = ctx && EVP_DecryptInit_ex(ctx, cipher, nullptr, key.key.data(), iv) == 1 &&
					EVP_CIPHER_CTX_set_padding(ctx, 0) == 1 &&
					EVP_DecryptUpdate(ctx, data, &out, data, static_cast<int>(size)) == 1 &&
					EVP_DecryptFinal_ex(ctx, data + out, &last) == 1;
	EVP_CIPHER_CTX_free(ctx);
	if (!ok) {
		DropErrors();
		throw FormatError("backup: a block or page does not decrypt (the cipher refused it)");
	}
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
