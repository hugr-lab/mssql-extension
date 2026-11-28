// Fuzz harness: a Microsoft PVK private-key container (spec 087 task 8), as the mssql_backup_key secret reads
// one at CREATE SECRET -- ReadPrivateKey's PVK path: the header's lengths, the BLOBHEADER, RC4 under
// SHA1(salt || password)[:16] when it is encrypted, and the PRIVATEKEYBLOB's RSA components into an OpenSSL key.
//
// The input is the PVK's bytes after its magic (the harness writes 0xB0B5F11E first, so that PEM and PFX --
// OpenSSL's own parsers, fuzzed upstream, whose PBKDF2 iteration count an input could set to minutes -- are
// never reached). Each input is read with no password and with "password" (the seeds' encrypted PVK's). Every
// malformed input must end in a FormatError; any sanitizer report, signal, hang or other exception is a bug.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "backup/backup_bytes.hpp"
#include "backup/backup_crypto.hpp"

using namespace duckdb::mssql::backup;

static volatile size_t g_sink = 0;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	if (size > (1u << 16)) {
		return 0;  // a PVK of a 16384-bit key is under 10 KB; larger inputs only slow the run
	}
	std::vector<uint8_t> pvk = {0x1E, 0xF1, 0xB5, 0xB0};
	pvk.insert(pvk.end(), data, data + size);
	for (const char *password : {"", "password"}) {
		try {
			PrivateKey key = ReadPrivateKey(pvk, password, "fuzz.pvk");
			g_sink += key.ModulusBytes();
		} catch (FormatError &) {
		}
	}
	return 0;
}
