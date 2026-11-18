// Spec 085 PR 4c-3: the digests mssql_backup_table_changes compares two versions of a row by where it
// keeps them, not their bytes (a row moved between pages, a value stored off row). DuckDB-free (D15).
//
// Two kinds (mssql_backup_change_hash): XXH64 by default -- written here from its published
// specification (Yann Collet's xxHash, XXH64 r39), checked against its test vectors; an accidental
// collision is out of reach (2^-64 a pair), bytes chosen to collide are not (owner's call, 2026-11-18:
// the source's writers are trusted) -- or SHA-256 through the OpenSSL the extension links (its first 16
// bytes). Both are fed in pieces and give one digest however the bytes are cut (a value's fragments).

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

struct evp_md_ctx_st;

namespace duckdb {
namespace mssql {
namespace backup {

//! A record's or a value's digest: XXH64 and the length, or SHA-256's first 16 bytes.
struct Digest {
	uint8_t bytes[16] = {};
	bool operator==(const Digest &other) const {
		return std::memcmp(bytes, other.bytes, sizeof(bytes)) == 0;
	}
	bool operator!=(const Digest &other) const {
		return !(*this == other);
	}
};

//! XXH64, streamed (seed 0 unless given).
class Xxh64 {
public:
	explicit Xxh64(uint64_t seed = 0) {
		Reset(seed);
	}
	void Reset(uint64_t seed = 0);
	void Update(const void *data, size_t size);
	uint64_t Digest() const;

private:
	uint64_t seed_ = 0;
	uint64_t v_[4] = {};
	uint8_t buffer_[32] = {};
	size_t buffered_ = 0;
	uint64_t total_ = 0;
};

//! The digest of bytes fed in pieces, XXH64 or SHA-256.
class BytesHash {
public:
	BytesHash() = default;
	~BytesHash();
	BytesHash(const BytesHash &) = delete;
	BytesHash &operator=(const BytesHash &) = delete;

	//! SHA-256 from now on (before anything is added).
	void UseSha256();
	void Update(const void *data, size_t size);
	//! The digest of what was added since the last one.
	Digest Finish();
	Digest Of(const std::string &bytes) {
		Update(bytes.data(), bytes.size());
		return Finish();
	}

private:
	evp_md_ctx_st *sha_ = nullptr;
	Xxh64 xxh_;
	uint64_t length_ = 0;
};

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
