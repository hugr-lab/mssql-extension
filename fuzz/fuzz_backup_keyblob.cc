// Fuzz harness: the PLAINTEXTKEYBLOB an unwrapped backup key or DEK is (spec 087 task 8, E4 / E4b) --
// ReadPlaintextKeyBlob, what UnwrapContentKey hands the RSA decryption's output to: bType, version, aiKeyAlg,
// the key's length against its algorithm's; then the key is used as DecryptCbc uses it, over a block of the
// input's own bytes. Every malformed input must end in a FormatError; any sanitizer report, signal, hang or
// other exception is a bug.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "backup/backup_bytes.hpp"
#include "backup/backup_crypto.hpp"

using namespace duckdb::mssql::backup;

static volatile size_t g_sink = 0;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	try {
		ContentKey key = ReadPlaintextKeyBlob(data, size, "fuzz keyblob");
		// A block of the input (zeros past its end), decrypted with the key and an IV of the input's first bytes.
		uint8_t iv[16] = {};
		for (size_t i = 0; i < 16 && i < size; i++) {
			iv[i] = data[i];
		}
		std::vector<uint8_t> block(64, 0);
		for (size_t i = 0; i < block.size() && i < size; i++) {
			block[i] = data[size - 1 - i];
		}
		DecryptCbc(key, iv, block.data(), block.size());
		g_sink += block[0] + key.BlockSize();
	} catch (FormatError &) {
	}
	return 0;
}
