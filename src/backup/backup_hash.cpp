// Spec 085 PR 4c-3: XXH64 and SHA-256 digests for the row changes (backup_hash.hpp). DuckDB-free (D15).

#include "backup/backup_hash.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <new>
#include <stdexcept>

namespace duckdb {
namespace mssql {
namespace backup {

namespace {

constexpr uint64_t P1 = 0x9E3779B185EBCA87ULL;
constexpr uint64_t P2 = 0xC2B2AE3D27D4EB4FULL;
constexpr uint64_t P3 = 0x165667B19E3779F9ULL;
constexpr uint64_t P4 = 0x85EBCA77C2B2AE63ULL;
constexpr uint64_t P5 = 0x27D4EB2F165667C5ULL;

uint64_t Rotl(uint64_t x, int r) {
	return (x << r) | (x >> (64 - r));
}

uint64_t Read64(const uint8_t *p) {
	// Little-endian, as XXH64 reads its input.
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--) {
		v = (v << 8) | p[i];
	}
	return v;
}

uint32_t Read32(const uint8_t *p) {
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
		   (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t Round(uint64_t acc, uint64_t input) {
	acc += input * P2;
	acc = Rotl(acc, 31);
	return acc * P1;
}

uint64_t MergeRound(uint64_t acc, uint64_t val) {
	acc ^= Round(0, val);
	return acc * P1 + P4;
}

EVP_MD *Sha256Md() {
	static EVP_MD *const MD = EVP_MD_fetch(nullptr, "SHA256", nullptr);
	return MD;
}

void InitSha(EVP_MD_CTX *ctx) {
	EVP_MD *md = Sha256Md();
	if (!md || EVP_DigestInit_ex(ctx, md, nullptr) != 1) {
		throw std::runtime_error("backup: SHA-256 is not available");
	}
}

}  // namespace

void Xxh64::Reset(uint64_t seed) {
	seed_ = seed;
	v_[0] = seed + P1 + P2;
	v_[1] = seed + P2;
	v_[2] = seed;
	v_[3] = seed - P1;
	buffered_ = 0;
	total_ = 0;
}

void Xxh64::Update(const void *data, size_t size) {
	const auto *in = static_cast<const uint8_t *>(data);
	total_ += size;
	if (buffered_ > 0) {
		const size_t take = std::min(size, sizeof(buffer_) - buffered_);
		std::memcpy(buffer_ + buffered_, in, take);
		buffered_ += take;
		in += take;
		size -= take;
		if (buffered_ < sizeof(buffer_)) {
			return;
		}
		for (int i = 0; i < 4; i++) {
			v_[i] = Round(v_[i], Read64(buffer_ + 8 * i));
		}
		buffered_ = 0;
	}
	while (size >= 32) {
		for (int i = 0; i < 4; i++) {
			v_[i] = Round(v_[i], Read64(in + 8 * i));
		}
		in += 32;
		size -= 32;
	}
	if (size > 0) {
		std::memcpy(buffer_, in, size);
		buffered_ = size;
	}
}

uint64_t Xxh64::Digest() const {
	uint64_t h;
	if (total_ >= 32) {
		h = Rotl(v_[0], 1) + Rotl(v_[1], 7) + Rotl(v_[2], 12) + Rotl(v_[3], 18);
		for (int i = 0; i < 4; i++) {
			h = MergeRound(h, v_[i]);
		}
	} else {
		h = seed_ + P5;
	}
	h += total_;
	const uint8_t *p = buffer_;
	size_t left = buffered_;
	while (left >= 8) {
		h ^= Round(0, Read64(p));
		h = Rotl(h, 27) * P1 + P4;
		p += 8;
		left -= 8;
	}
	if (left >= 4) {
		h ^= static_cast<uint64_t>(Read32(p)) * P1;
		h = Rotl(h, 23) * P2 + P3;
		p += 4;
		left -= 4;
	}
	while (left > 0) {
		h ^= static_cast<uint64_t>(*p) * P5;
		h = Rotl(h, 11) * P1;
		p++;
		left--;
	}
	h ^= h >> 33;
	h *= P2;
	h ^= h >> 29;
	h *= P3;
	h ^= h >> 32;
	return h;
}

BytesHash::~BytesHash() {
	if (sha_) {
		EVP_MD_CTX_free(sha_);
	}
}

void BytesHash::UseSha256() {
	if (sha_) {
		return;
	}
	sha_ = EVP_MD_CTX_new();
	if (!sha_) {
		throw std::bad_alloc();
	}
	InitSha(sha_);
}

void BytesHash::Update(const void *data, size_t size) {
	length_ += size;
	if (sha_) {
		if (size > 0 && EVP_DigestUpdate(sha_, data, size) != 1) {
			throw std::runtime_error("backup: SHA-256 failed");
		}
		return;
	}
	xxh_.Update(data, size);
}

Digest BytesHash::Finish() {
	Digest d;
	if (sha_) {
		unsigned char out[EVP_MAX_MD_SIZE];
		unsigned int size = 0;
		if (EVP_DigestFinal_ex(sha_, out, &size) != 1 || size < sizeof(d.bytes)) {
			throw std::runtime_error("backup: SHA-256 failed");
		}
		std::memcpy(d.bytes, out, sizeof(d.bytes));
		InitSha(sha_);
	} else {
		const uint64_t h = xxh_.Digest();
		std::memcpy(d.bytes, &h, sizeof(h));
		std::memcpy(d.bytes + 8, &length_, sizeof(length_));
		xxh_.Reset();
	}
	length_ = 0;
	return d;
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
