// Spec 087: the cryptography of encrypted backups (backup_crypto) and the encrypted container
// (backup_container): RC4 on its published vector; the fixtures' certificates, PVKs and PFX; and every
// encrypted fixture of SQL Server 2025 (E4b) read: its key unwrapped, its chain walked from where it
// starts, its data set decrypted (and decompressed) to the SSET the backup set's header reads from.
//
// The fixtures are not committed (spec 087 task 9 commits two small ones): MSSQL_BACKUP_087_FIXTURES names
// their directory and MSSQL_BACKUP_087_PASSWORD_FILE the PVK password's file; without them only the
// self-contained checks run. No password, key or IV is printed. DuckDB-free.

#include "backup/backup_allocation.hpp"
#include "backup/backup_container.hpp"
#include "backup/backup_crypto.hpp"
#include "backup/backup_mtf.hpp"
#include "backup/backup_page.hpp"
#include "backup/backup_page_index.hpp"
#include "backup/backup_set.hpp"
#include "backup/backup_xpress.hpp"
#include "backup/backup_zstd.hpp"

#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using namespace duckdb::mssql::backup;

static int failures = 0;

static void Check(bool ok, const std::string &what) {
	std::cout << (ok ? "  ok   " : "  FAIL ") << what << std::endl;
	if (!ok) {
		failures++;
	}
}

template <class F>
static std::string ErrorOf(F f) {
	try {
		f();
	} catch (std::exception &e) {
		return e.what();
	}
	return std::string();
}

static std::vector<uint8_t> Load(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static std::string Trim(std::string s) {
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
		s.pop_back();
	}
	return s;
}

int main() {
	std::cout << "[1] RC4" << std::endl;
	{
		// The cipher's published vector: key "Key", plaintext "Plaintext" -> BBF316E8D940AF0AD3.
		uint8_t data[] = {'P', 'l', 'a', 'i', 'n', 't', 'e', 'x', 't'};
		const uint8_t key[] = {'K', 'e', 'y'};
		Rc4(key, sizeof(key), data, sizeof(data));
		const uint8_t want[] = {0xBB, 0xF3, 0x16, 0xE8, 0xD9, 0x40, 0xAF, 0x0A, 0xD3};
		Check(std::memcmp(data, want, sizeof(want)) == 0, "RC4(\"Key\", \"Plaintext\") is the published vector");
	}
	std::cout << "[2] what refuses before any key" << std::endl;
	{
		Check(ErrorOf([]() { ReadCertificate(std::vector<uint8_t>(10, 0x41), "x"); }).find("not a certificate") !=
				  std::string::npos,
			  "ten bytes are not a certificate");
		Check(ErrorOf([]() { ReadPrivateKey(std::vector<uint8_t>(10, 0x41), "", "x"); }).find("not a private key") !=
				  std::string::npos,
			  "ten bytes are no key container");
		std::vector<uint8_t> pvk(24, 0);
		const uint32_t magic = 0xB0B5F11E;
		std::memcpy(pvk.data(), &magic, 4);
		pvk[16] = 200;	// a salt longer than the file
		Check(ErrorOf([&]() { ReadPrivateKey(pvk, "", "x"); }).find("does not fit") != std::string::npos,
			  "a PVK whose salt does not fit its file is refused");
		// An unencrypted PVK whose key blob is 8 bytes, its BLOBHEADER alone (review of #670: the "RSA2" compare read
		// four bytes past it, ASan's heap-buffer-overflow). The same bytes are
		// fuzz/corpus/backup_pvk/key_size_8_review_670.
		std::vector<uint8_t> short_key(24, 0);
		std::memcpy(short_key.data(), &magic, 4);
		short_key[8] = 1;	// key type
		short_key[20] = 8;	// key length
		const uint8_t blob_header[] = {7, 2, 0, 0, 0, 0xA4, 0, 0};
		short_key.insert(short_key.end(), blob_header, blob_header + sizeof(blob_header));
		Check(ErrorOf([&]() { ReadPrivateKey(short_key, "", "x"); }).find("does not fit") != std::string::npos,
			  "a PVK whose key blob is shorter than a BLOBHEADER and its RSA magic is refused");
	}
	std::cout << "[S] the committed samples (test/data/backup/encryption; run on every PR, review of #670)"
			  << std::endl;
	{
		const std::string dir = "test/data/backup/encryption";
		const auto password_bytes = Load(dir + "/pvk_password");
		const std::string password = Trim(std::string(password_bytes.begin(), password_bytes.end()));
		const Certificate enc_cert = ReadCertificate(Load(dir + "/sample_enc_cert.cer"), "sample_enc_cert.cer");
		const Certificate tde_cert = ReadCertificate(Load(dir + "/sample_tde_cert.cer"), "sample_tde_cert.cer");
		PrivateKey enc_key = ReadPrivateKey(Load(dir + "/sample_enc_cert.pvk"), password, "sample_enc_cert.pvk");
		PrivateKey tde_key = ReadPrivateKey(Load(dir + "/sample_tde_cert.pvk"), password, "sample_tde_cert.pvk");
		Check(enc_key.Matches(enc_cert.der) && tde_key.Matches(tde_cert.der) && !enc_key.Matches(tde_cert.der),
			  "the samples' PVKs open and match their certificates");
		const BlockDecoder xpress = [](const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size) {
			XpressHuffmanDecode(in, in_size, out, out_size);
		};
		{
			// enc_sample: WITH ENCRYPTION, its key unwrapped, its chain walked, its data set decrypted to the SSET.
			const auto bytes = Load(dir + "/enc_sample.bak");
			MemoryAccess file("enc_sample", bytes.data(), bytes.size());
			const ContainerHeader header = ReadContainerHeader(file);
			Check(header.encrypted && header.encryptor_thumbprint == enc_cert.thumbprint,
				  "enc_sample's header names its certificate");
			ContainerKeys keys;
			keys.backup = std::make_shared<ContentKey>(
				UnwrapContentKey(enc_key, header.wrapped_key.data(), header.wrapped_key.size(), "enc_sample"));
			BlockChain chain = WalkChain(file, ChainStart(header));
			ContainerStream stream(file, chain, header.compressed ? xpress : BlockDecoder(), keys);
			uint8_t magic[4];
			stream.ReadAt(0, magic, 4);
			const BackupSetHeader set = ReadBackupSet(stream, WalkMtfDataSet(stream));
			Check(std::memcmp(magic, "SSET", 4) == 0 && !set.database_name.empty(),
				  std::string("enc_sample: ") + KeyAlgorithmName(keys.backup->algorithm) + ", " +
					  std::to_string(chain.block_count) + " blocks decrypted, the data set's SSET names " +
					  set.database_name);
			Check(ErrorOf([&]() {
					  UnwrapContentKey(tde_key, header.wrapped_key.data(), header.wrapped_key.size(), "x");
				  }).find("does not unwrap") != std::string::npos,
				  "another certificate's key does not unwrap it: one message, no garbage");
		}
		{
			// SQL Server 2019 / 2022's wrap (EncryptorType CERTIFICATE, review of #670): a PLAINTEXTKEYBLOB under
			// PKCS#1 v1.5 with the certificate's public key, reversed as CryptoAPI writes it -- unwrapped under
			// OpenSSL's implicit rejection; and a blob of random bytes gives the one message, whichever path it took.
			uint8_t blob[44] = {8, 2, 0, 0, 0x10, 0x66, 0, 0, 32, 0, 0, 0};
			for (int i = 0; i < 32; i++) {
				blob[12 + i] = static_cast<uint8_t>(0xA0 + i);
			}
			const unsigned char *der = enc_cert.der.data();
			X509 *x509 = d2i_X509(nullptr, &der, static_cast<long>(enc_cert.der.size()));
			EVP_PKEY *pub = x509 ? X509_get_pubkey(x509) : nullptr;
			EVP_PKEY_CTX *ctx = pub ? EVP_PKEY_CTX_new(pub, nullptr) : nullptr;
			std::vector<uint8_t> wrapped(enc_key.ModulusBytes());
			size_t wrapped_size = wrapped.size();
			const bool encrypted = ctx && EVP_PKEY_encrypt_init(ctx) == 1 &&
								   EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING) == 1 &&
								   EVP_PKEY_encrypt(ctx, wrapped.data(), &wrapped_size, blob, sizeof(blob)) == 1 &&
								   wrapped_size == wrapped.size();
			EVP_PKEY_CTX_free(ctx);
			EVP_PKEY_free(pub);
			X509_free(x509);
			std::reverse(wrapped.begin(), wrapped.end());
			bool same = false;
			if (encrypted) {
				const ContentKey key = UnwrapContentKey(enc_key, wrapped.data(), wrapped.size(), "v1.5");
				same = key.algorithm == KeyAlgorithm::Aes256 && key.key.size() == 32 &&
					   std::memcmp(key.key.data(), blob + 12, 32) == 0;
			}
			Check(same, "a key wrapped under PKCS#1 v1.5 (2019 / 2022) unwraps, implicit rejection on");
			std::vector<uint8_t> noise(enc_key.ModulusBytes());
			for (size_t i = 0; i < noise.size(); i++) {
				noise[i] = static_cast<uint8_t>(i * 131 + 7);
			}
			noise.back() = 0x01;  // below the modulus once reversed
			Check(ErrorOf([&]() {
					  UnwrapContentKey(enc_key, noise.data(), noise.size(), "x");
				  }).find("x: the key does not unwrap with the secret's private key") != std::string::npos,
				  "a blob of no wrap at all gives the one message, as a wrong key does");
		}
		{
			// tde_sample: uncompressed TDE, the DEK from the boot page (FCBFileDEK at 912), every live page decrypted
			// and its checksum verified (PageStore::Read throws UnreadablePageError for one that does not).
			const auto bytes = Load(dir + "/tde_sample.bak");
			MemoryAccess file("tde_sample", bytes.data(), bytes.size());
			PageIndex index = BuildPageIndex(file, WalkMtf(file), true);
			std::vector<uint8_t> boot;
			const bool found = index.ReadPage(1, 9, boot, true);
			uint32_t thumb_length = 0;
			uint32_t wrapped_length = 0;
			Thumbprint thumb{};
			if (found && boot.size() == PAGE_SIZE) {
				std::memcpy(&thumb_length, boot.data() + 912, 4);
				std::memcpy(&wrapped_length, boot.data() + 936, 4);
				std::memcpy(thumb.data(), boot.data() + 916, 20);
			}
			if (!found || thumb_length != 20 || thumb != tde_cert.thumbprint ||
				wrapped_length != tde_key.ModulusBytes()) {
				Check(false, "tde_sample: FCBFileDEK at 912 of the boot page names its certificate");
			} else {
				auto dek = std::make_shared<ContentKey>(
					UnwrapContentKey(tde_key, boot.data() + 940, wrapped_length, "tde_sample"));
				index.SetPageKey(dek);
				AllocationMap allocation(index, {1});
				PageStore store(index, allocation);
				size_t live = 0;
				size_t failed = 0;
				for (uint32_t page = 0; page < allocation.FileSize(1); page++) {
					Page out;
					try {
						live += store.Read(1, page, out);
					} catch (UnreadablePageError &) {
						failed++;
					}
				}
				Check(live > 50 && failed == 0,
					  "tde_sample: " + std::to_string(live) + " live pages decrypted, every checksum verified");
				// The same page under another key: CBC takes it, its checksum does not (review of #670).
				auto other = std::make_shared<ContentKey>();
				other->algorithm = dek->algorithm;
				other->key = SecretBytes(dek->key.data(), dek->key.size());
				other->key.data()[0] ^= 0x5A;
				PageIndex wrong = BuildPageIndex(file, WalkMtf(file), true);
				wrong.SetPageKey(other);
				AllocationMap wrong_allocation(index, {1});
				PageStore wrong_store(wrong, wrong_allocation);
				size_t refused = 0;
				size_t data_pages = 0;
				for (uint32_t page = 0; page < allocation.FileSize(1); page++) {
					Page out;
					if (!allocation.IsLive(1, page) || page == 9 || page < 2) {
						continue;
					}
					data_pages++;
					try {
						wrong_store.Read(1, page, out);
					} catch (UnreadablePageError &) {
						refused++;
					}
				}
				Check(data_pages > 0 && refused == data_pages,
					  "under a wrong DEK every encrypted page is refused by its checksum (" + std::to_string(refused) +
						  " of " + std::to_string(data_pages) + "), none read as garbage");
			}
		}
		{
			// tde_sample_xpress: a TDE database's compressed backup, the DEK in its container header (E4c).
			const auto bytes = Load(dir + "/tde_sample_xpress.bak");
			MemoryAccess file("tde_sample_xpress", bytes.data(), bytes.size());
			const ContainerHeader header = ReadContainerHeader(file);
			Check(header.tde && header.tde_thumbprint == tde_cert.thumbprint,
				  "tde_sample_xpress names its DEK's certificate");
			ContainerKeys keys;
			keys.dek = std::make_shared<ContentKey>(
				UnwrapContentKey(tde_key, header.wrapped_dek.data(), header.wrapped_dek.size(), "tde_sample_xpress"));
			BlockChain chain = WalkChain(file, ChainStart(header));
			ContainerStream stream(file, chain, xpress, keys);
			const BackupSetHeader set = ReadBackupSet(stream, WalkMtfDataSet(stream));
			Check(!set.database_name.empty(),
				  "tde_sample_xpress: its blocks decrypt under the DEK, the SSET names " + set.database_name);
		}
	}
	const char *dir_env = std::getenv("MSSQL_BACKUP_087_FIXTURES");
	const char *password_env = std::getenv("MSSQL_BACKUP_087_PASSWORD_FILE");
	if (!dir_env || !password_env) {
		std::cout << "[3] fixtures skipped (MSSQL_BACKUP_087_FIXTURES / MSSQL_BACKUP_087_PASSWORD_FILE unset)"
				  << std::endl;
	} else {
		const std::string dir = dir_env;
		const auto password_bytes = Load(password_env);
		const std::string password = Trim(std::string(password_bytes.begin(), password_bytes.end()));
		std::cout << "[3] the certificates and their keys" << std::endl;
		const Certificate cert = ReadCertificate(Load(dir + "/enc_cert.cer"), "enc_cert.cer");
		const Certificate cert2 = ReadCertificate(Load(dir + "/enc_cert2.cer"), "enc_cert2.cer");
		Check(ThumbprintHex(cert.thumbprint).size() == 40 && cert.thumbprint != cert2.thumbprint,
			  "two certificates, two thumbprints");
		PrivateKey key = ReadPrivateKey(Load(dir + "/enc_cert.pvk"), password, "enc_cert.pvk");
		Check(key.Valid() && key.ModulusBytes() == 384, "the PVK opens: RSA-3072");
		Check(key.Matches(cert.der) && !key.Matches(cert2.der), "its modulus is its certificate's, not the other's");
		Check(ErrorOf([&]() {
				  ReadPrivateKey(Load(dir + "/enc_cert.pvk"), password + "x", "enc_cert.pvk");
			  }).find("wrong password") != std::string::npos,
			  "a wrong password is named, not a key");
		std::vector<uint8_t> pfx_cert;
		PrivateKey from_pfx = ReadPrivateKey(Load(dir + "/enc_cert.pfx"), password, "enc_cert.pfx", &pfx_cert);
		Check(from_pfx.Valid() && from_pfx.Matches(cert.der) &&
				  ReadCertificate(pfx_cert, "the PFX's").thumbprint == cert.thumbprint,
			  "the PFX (PBES2 / AES-256) gives the same key and certificate");
		Check(ErrorOf([&]() {
				  ReadPrivateKey(Load(dir + "/enc_cert.pfx"), "nope", "enc_cert.pfx");
			  }).find("wrong password") != std::string::npos,
			  "the PFX under a wrong password is named");
		PrivateKey key2 = ReadPrivateKey(Load(dir + "/enc_cert2.pvk"), password, "enc_cert2.pvk");

		std::cout << "[4] every encrypted fixture read (MS_XPRESS and stored; ZSTD's decoder is the extension's)"
				  << std::endl;
		const char *algs[] = {"aes_128", "aes_192", "aes_256", "triple_des_3key"};
		const char *comps[] = {"none", "xpress", "zstd"};
		const char *cks[] = {"ck", "nock"};
		const BlockDecoder xpress = [](const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size) {
			XpressHuffmanDecode(in, in_size, out, out_size);
		};
		size_t read = 0;
		for (const char *alg : algs) {
			for (const char *comp : comps) {
				for (const char *ck : cks) {
					const std::string name = std::string("enc_") + alg + "_" + comp + "_" + ck;
					const auto bytes = Load(dir + "/" + name + ".bak");
					MemoryAccess file(name, bytes.data(), bytes.size());
					const ContainerHeader header = ReadContainerHeader(file);
					const std::string what = name + ": ";
					if (!header.encrypted || header.encryptor_thumbprint != cert.thumbprint ||
						header.wrapped_key.size() != 384) {
						Check(false, what + "the header names the certificate and a 384-byte wrapped key");
						continue;
					}
					auto content = std::make_shared<ContentKey>(
						UnwrapContentKey(key, header.wrapped_key.data(), header.wrapped_key.size(), name));
					if (std::string(KeyAlgorithmName(content->algorithm)) != alg) {
						Check(false, what + "the key's algorithm is " + KeyAlgorithmName(content->algorithm));
						continue;
					}
					if (std::string(comp) == "zstd") {
						Check(header.algorithm == CompressionAlgorithm::Zstd && header.flags == 0x87,
							  what + "flags 0x87: encrypted, ZSTD; key unwrapped");
						continue;
					}
					BlockChain chain = WalkChain(file, ChainStart(header));
					ContainerKeys keys;
					keys.backup = content;
					ContainerStream stream(file, chain, header.compressed ? xpress : BlockDecoder(), keys);
					uint8_t magic[4];
					stream.ReadAt(0, magic, 4);
					const MtfLayout layout = WalkMtfDataSet(stream);
					const BackupSetHeader set = ReadBackupSet(stream, layout);
					auto tape = ReadContainerTape(file, header.compressed ? xpress : BlockDecoder(), keys);
					const bool ok = std::memcmp(magic, "SSET", 4) == 0 && set.database_name == "bak_enc" &&
									chain.block_count > 50 && std::memcmp(tape.data(), "TAPE", 4) == 0;
					Check(ok, what + std::to_string(chain.block_count) + " blocks from 0x" +
								  (ChainStart(header) == 0x3000 ? std::string("3000") : std::string("2000")) +
								  ", the data set's SSET names " + set.database_name + ", the media label decrypts");
					read += ok;
				}
			}
		}
		Check(read == 16, "16 sets read whole (4 algorithms x stored / MS_XPRESS x CHECKSUM or not)");

		std::cout << "[5] the wrong certificate's key" << std::endl;
		{
			const auto bytes = Load(dir + "/enc_aes_256_xpress_ck.bak");
			MemoryAccess file("enc_aes_256_xpress_ck", bytes.data(), bytes.size());
			const ContainerHeader header = ReadContainerHeader(file);
			Check(ErrorOf([&]() {
					  UnwrapContentKey(key2, header.wrapped_key.data(), header.wrapped_key.size(), "x");
				  }).find("does not unwrap") != std::string::npos,
				  "another certificate's key does not unwrap it: named, no garbage");
			const auto other = Load(dir + "/enc_cert2_full.bak");
			MemoryAccess other_file("enc_cert2_full", other.data(), other.size());
			Check(ReadContainerHeader(other_file).encryptor_thumbprint == cert2.thumbprint,
				  "the second certificate's backup names its thumbprint");
		}
		std::cout << "[6] TDE: the DEK from the boot page, every page of the database decrypted" << std::endl;
		PrivateKey tde_key = ReadPrivateKey(Load(dir + "/tde_cert.pvk"), password, "tde_cert.pvk");
		PrivateKey tde_key2 = ReadPrivateKey(Load(dir + "/tde_cert2.pvk"), password, "tde_cert2.pvk");
		const Certificate tde_cert = ReadCertificate(Load(dir + "/tde_cert.cer"), "tde_cert.cer");
		const Certificate tde_cert2 = ReadCertificate(Load(dir + "/tde_cert2.cer"), "tde_cert2.cer");
		for (const char *name : {"tde256_full", "tde128_full", "tde256_rotated_full"}) {
			const auto bytes = Load(dir + "/" + std::string(name) + ".bak");
			MemoryAccess file(name, bytes.data(), bytes.size());
			const MtfLayout layout = WalkMtf(file);
			PageIndex index = BuildPageIndex(file, layout, true);
			std::vector<uint8_t> boot;
			const bool found = index.ReadPage(1, 9, boot, true);
			uint32_t thumb_length = 0;
			uint32_t wrapped_length = 0;
			std::memcpy(&thumb_length, boot.data() + 912, 4);
			std::memcpy(&wrapped_length, boot.data() + 936, 4);
			Thumbprint thumb{};
			std::memcpy(thumb.data(), boot.data() + 916, 20);
			const bool rotated = std::string(name) == "tde256_rotated_full";
			const bool names = thumb == (rotated ? tde_cert2.thumbprint : tde_cert.thumbprint);
			if (!found || boot[1] != 13 || thumb_length != 20 || wrapped_length != 384 || !names) {
				Check(false, std::string(name) + ": FCBFileDEK at 912 of the boot page names the certificate");
				continue;
			}
			auto dek = std::make_shared<ContentKey>(
				UnwrapContentKey(rotated ? tde_key2 : tde_key, boot.data() + 940, 384, name));
			index.SetPageKey(dek);
			AllocationMap allocation(index, {1});
			PageStore store(index, allocation);
			size_t data_pages = 0;
			size_t sound = 0;
			for (uint32_t page = 0; page < 700; page++) {
				Page out;
				if (!store.Read(1, page, out) || out.header.type != 1) {
					continue;
				}
				data_pages++;
				uint16_t slots = 0;
				std::memcpy(&slots, out.bytes.data() + 22, 2);
				bool ok = slots > 0;
				for (uint16_t i = 0; ok && i < slots; i++) {
					uint16_t at = 0;
					std::memcpy(&at, out.bytes.data() + PAGE_SIZE - 2 * (i + 1), 2);
					ok = at >= 96 && at < PAGE_SIZE;
				}
				sound += ok;
			}
			Check(data_pages > 100 && sound == data_pages,
				  std::string(name) + ": " + KeyAlgorithmName(dek->algorithm) + " DEK; " + std::to_string(data_pages) +
					  " live data pages read through the allocation map, every slot array sound" +
					  (rotated ? " (the DEK re-wrapped by tde_cert2)" : ""));
		}
		std::cout << "[7] E4c: a TDE database's compressed backup -- its blocks under the DEK, its pages in clear"
				  << std::endl;
		const BlockDecoder zstd = [](const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size) {
			ZstdDecode(in, in_size, out, out_size);
		};
		PrivateKey enc_key = ReadPrivateKey(Load(dir + "/enc_cert.pvk"), password, "enc_cert.pvk");
		auto key_of = [&](const Thumbprint &t) -> const PrivateKey & {
			if (t == tde_cert.thumbprint) {
				return tde_key;
			}
			if (t == tde_cert2.thumbprint) {
				return tde_key2;
			}
			return enc_key;
		};
		struct Opened {
			std::vector<uint8_t> bytes;
			std::unique_ptr<MemoryAccess> file;
			ContainerHeader header;
			ContainerKeys keys;
			std::unique_ptr<ContainerStream> stream;
			MtfLayout layout;
			std::unique_ptr<PageIndex> index;
			// The extension's way (backup_opened): the chain located from the tail, its seek index, the pass
			// searched through the container's blocks (ContainerPassMedium) or, under trust_layout, by resync.
			BlockChain chain;
			std::vector<SeekEntry> seek;
			bool from_tail = false;
			ContainerFamily family;
			std::unique_ptr<PageIndex> searched[2];
		};
		auto open_compressed = [&](const std::string &name) {
			std::unique_ptr<Opened> o(new Opened());
			o->bytes = Load(dir + "/" + name + ".bak");
			o->file.reset(new MemoryAccess(name, o->bytes.data(), o->bytes.size()));
			o->header = ReadContainerHeader(*o->file);
			if (o->header.encrypted) {
				o->keys.backup = std::make_shared<ContentKey>(UnwrapContentKey(key_of(o->header.encryptor_thumbprint),
																			   o->header.wrapped_key.data(),
																			   o->header.wrapped_key.size(), name));
			}
			o->keys.dek = std::make_shared<ContentKey>(UnwrapContentKey(
				key_of(o->header.tde_thumbprint), o->header.wrapped_dek.data(), o->header.wrapped_dek.size(), name));
			o->from_tail = OpenChainFromTail(*o->file, o->chain, o->seek, ChainStart(o->header));
			if (!o->from_tail) {
				o->chain = WalkChain(*o->file, ChainStart(o->header));
				o->seek = ReadSeekIndex(*o->file, o->chain);
			}
			const BlockDecoder &decoder = o->header.algorithm == CompressionAlgorithm::Zstd ? zstd : xpress;
			o->stream.reset(new ContainerStream(*o->file, o->chain, decoder, o->keys));
			o->layout = WalkMtfDataSet(*o->stream);
			o->index.reset(new PageIndex(BuildPageIndex(*o->stream, o->layout, true)));
			o->family.file = o->file.get();
			o->family.stream = o->stream.get();
			o->family.seek = &o->seek;
			o->family.chain_end = o->chain.end;
			o->family.decoder = decoder;
			o->family.keys = o->keys;
			for (int trust = 0; trust < 2; trust++) {
				FamilyStream f;
				f.stream = o->stream.get();
				f.layout = &o->layout;
				f.container = &o->family;
				PageIndexOptions options;
				options.full_backup = true;
				options.trust_layout = trust == 1;
				options.data_files = {1};
				o->searched[trust].reset(new PageIndex(BuildPageIndex({f}, options)));
			}
			auto tape = ReadContainerTape(*o->file, decoder, o->keys, true);
			if (std::memcmp(tape.data(), "TAPE", 4) != 0) {
				throw std::runtime_error(name + ": no TAPE at 0x438");
			}
			return o;
		};
		{
			// The same tiny database three ways: every page of the compressed ones equal to the plain one's decrypted.
			const auto plain_bytes = Load(dir + "/tde_small_plain.bak");
			MemoryAccess plain_file("tde_small_plain", plain_bytes.data(), plain_bytes.size());
			const MtfLayout plain_layout = WalkMtf(plain_file);
			PageIndex plain = BuildPageIndex(plain_file, plain_layout, true);
			std::vector<uint8_t> boot;
			plain.ReadPage(1, 9, boot, true);
			plain.SetPageKey(std::make_shared<ContentKey>(UnwrapContentKey(tde_key, boot.data() + 940, 384, "plain")));
			for (const char *name : {"tde_small_xpress", "tde_small_zstd"}) {
				auto o = open_compressed(name);
				size_t compared = 0;
				size_t equal = 0;
				size_t moved = 0;
				size_t row_values = 0;
				const std::string utf16_three("t\0h\0r\0e\0e\0", 10);
				for (uint32_t page = 0; page < 400; page++) {
					std::vector<uint8_t> a;
					std::vector<uint8_t> b;
					const bool in_plain = plain.ReadPage(1, page, a);
					const bool in_packed = o->index->ReadPage(1, page, b);
					if (!in_plain && !in_packed) {
						continue;
					}
					compared++;
					if (!in_plain || !in_packed) {
						continue;
					}
					// A page written between the two backups (m_lsn @44 moved: the file header, the boot page,
					// a data page or two) is not the same page; every other is, byte for byte.
					if (std::memcmp(a.data() + 44, b.data() + 44, 10) != 0) {
						moved++;
						continue;
					}
					equal += a == b;
					// dbo.t's rows, (1,'one'), (2,'two'), (3,'three'), in a data page of the compressed form.
					const std::string bytes(b.begin(), b.end());
					if (b[1] == 1 &&
						(bytes.find("three") != std::string::npos || bytes.find(utf16_three) != std::string::npos)) {
						row_values++;
					}
				}
				Check(
					o->header.tde && compared > 300 && moved < 8 && equal + moved == compared && row_values > 0,
					std::string(name) + ": flags 0x" + (o->header.flags == 0xAB ? "AB" : "2B") + ", " +
						std::to_string(equal) + " of " + std::to_string(compared) +
						" pages equal byte for byte to the plain backup's decrypted (" + std::to_string(moved) +
						" written between the two backups); dbo.t's 'three' in a data page; the media label at 0x438");
			}
		}
		for (const char *name : {"tde256_full_xpress", "tde256_full_xpress_nock", "tde256_full_zstd",
								 "tde128_full_xpress", "tde256_enc_full"}) {
			auto o = open_compressed(name);
			AllocationMap allocation(*o->index, {1});
			PageStore store(*o->index, allocation);
			size_t data_pages = 0;
			size_t sound = 0;
			size_t searched_same = 0;
			for (uint32_t page = 0; page < 700; page++) {
				// Every page the header scan finds, the extension's searches find alike.
				std::vector<uint8_t> scanned;
				if (o->index->ReadPage(1, page, scanned)) {
					std::vector<uint8_t> through_blocks;
					std::vector<uint8_t> by_resync;
					searched_same += o->searched[0]->ReadPage(1, page, through_blocks) &&
									 o->searched[1]->ReadPage(1, page, by_resync) && through_blocks == scanned &&
									 by_resync == scanned;
				} else {
					searched_same++;
				}
				Page out;
				if (!store.Read(1, page, out) || out.header.type != 1) {
					continue;
				}
				data_pages++;
				uint16_t slots = 0;
				std::memcpy(&slots, out.bytes.data() + 22, 2);
				bool ok = slots > 0;
				for (uint16_t i = 0; ok && i < slots; i++) {
					uint16_t at = 0;
					std::memcpy(&at, out.bytes.data() + PAGE_SIZE - 2 * (i + 1), 2);
					ok = at >= 96 && at < PAGE_SIZE;
				}
				sound += ok;
			}
			Check(o->from_tail && searched_same == 700,
				  std::string(name) +
					  ": the chain from its tail (a seek index naming offsets inside its last block), "
					  "and the searched passes (through its blocks; by resync) read pages 0..699 as "
					  "the header scan does");
			Check(data_pages > 100 && sound == data_pages,
				  std::string(name) + ": " + KeyAlgorithmName(o->keys.dek->algorithm) + " DEK" +
					  (o->keys.backup ? std::string(", and the backup's ") + KeyAlgorithmName(o->keys.backup->algorithm)
									  : std::string()) +
					  "; " + std::to_string(data_pages) + " live data pages through the allocation map, sound");
		}
		{
			// The differential, over the rotated full: its own pages read (the overlay's pages are 085's).
			auto o = open_compressed("tde256_diff_xpress");
			Check(o->header.tde && o->header.tde_thumbprint == tde_cert2.thumbprint &&
					  o->layout.FindAll("SSET").size() == 1,
				  "tde256_diff_xpress: a differential, its DEK under tde_cert2 since the rotation, its data set read");
			const BackupSetHeader set = ReadBackupSet(*o->stream, o->layout);
			bool named = false;
			for (const auto &f : set.files) {
				named = named || (f.type == 'D' && f.tde && f.tde_thumbprint == tde_cert2.thumbprint);
			}
			Check(named, "tde256_diff_xpress: its data file's SFIN (in the decrypted stream) names tde_cert2 too");
		}

		std::cout << "[8] D10: an uncompressed TDE backup names its DEK's certificate in clear (SFIN, TDEThumbprint)"
				  << std::endl;
		struct Expected {
			const char *name;
			const Certificate *certificate;
		};
		for (const auto &e : {Expected{"tde256_full", &tde_cert}, Expected{"tde128_full", &tde_cert},
							  Expected{"tde_small_plain", &tde_cert}, Expected{"tde256_rotated_full", &tde_cert2},
							  Expected{"tde256_rotated_diff", &tde_cert2}, Expected{"enc_aes_256_none_ck", nullptr}}) {
			const auto bytes = Load(dir + "/" + e.name + ".bak");
			MemoryAccess file(e.name, bytes.data(), bytes.size());
			if (IsContainer(file)) {
				// An encrypted backup of a database without TDE: its SFIN is in its data set.
				const ContainerHeader header = ReadContainerHeader(file);
				ContainerKeys keys;
				keys.backup = std::make_shared<ContentKey>(
					UnwrapContentKey(enc_key, header.wrapped_key.data(), header.wrapped_key.size(), e.name));
				ContainerStream stream(file, WalkChain(file, ChainStart(header)), BlockDecoder(), keys);
				const BackupSetHeader set = ReadBackupSet(stream, WalkMtfDataSet(stream));
				bool any = false;
				for (const auto &f : set.files) {
					any = any || f.tde;
				}
				Check(!any && !header.tde, std::string(e.name) + ": not TDE, no file names a DEK");
				continue;
			}
			const BackupSetHeader set = ReadBackupSet(file, WalkMtf(file));
			size_t data_named = 0;
			size_t log_named = 0;
			for (const auto &f : set.files) {
				if (f.tde) {
					(f.type == 'D' ? data_named : log_named)++;
					if (f.tde_thumbprint != e.certificate->thumbprint || f.wrapped_dek.size() != 384) {
						data_named = 1000;
					}
				}
			}
			Check(data_named == 1 && log_named == 0, std::string(e.name) +
														 ": its data file's SFIN names the DEK's certificate (" +
														 ThumbprintHex(e.certificate->thumbprint).substr(0, 8) +
														 "…) with a 384-byte wrapped DEK; the log "
														 "file's names none");
		}
	}
	if (failures != 0) {
		std::cout << failures << " check(s) FAILED" << std::endl;
		return 1;
	}
	std::cout << "all checks passed" << std::endl;
	return 0;
}
