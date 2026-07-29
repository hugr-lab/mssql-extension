#include "tds/encoding/decimal_encoding.hpp"
#include <cstring>

namespace duckdb {
namespace tds {
namespace encoding {

// Every platform this extension builds for is little-endian (x86_64 / ARM64 on
// Linux, macOS and Windows), which makes the TDS magnitude a straight load. The
// guard is here so a big-endian port fails over to the portable loop instead of
// silently byte-swapping every DECIMAL on the wire.
#if defined(_MSC_VER) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#define MSSQL_DECIMAL_DIRECT_LOAD 1
#endif

namespace {

// Reference implementation, kept for the two cases the fast path declines:
// big-endian hosts, and a malformed length that would not fit an int128 anyway
// (where this overflows exactly as it always has, rather than silently
// truncating to the low 16 bytes).
hugeint_t ConvertDecimalPortable(const uint8_t *data, size_t length) {
	bool negative = data[0] == 0;
	hugeint_t magnitude(0);
	for (size_t i = length - 1; i >= 1; i--) {
		magnitude = magnitude * hugeint_t(256) + hugeint_t(data[i]);
	}
	return negative ? -magnitude : magnitude;
}

}  // namespace

hugeint_t DecimalEncoding::ConvertDecimal(const uint8_t *data, size_t length) {
	if (length == 0) {
		return hugeint_t(0);
	}

	// The old implementation ran a full 128-bit multiply-accumulate PER BYTE
	// (magnitude * 256 + data[i]) — up to sixteen 128-bit multiplies for one
	// value. But TDS already sends the magnitude in exactly the byte order an
	// int128 wants, so the whole thing is two loads. Measured 73.3 -> 3.2 ns
	// per value (23x); see spec 055 D1.
#ifdef MSSQL_DECIMAL_DIRECT_LOAD
	const size_t mag_bytes = length - 1;
	if (mag_bytes <= 16) {
		uint64_t lower = 0;
		uint64_t upper = 0;
		const size_t low_bytes = mag_bytes < 8 ? mag_bytes : 8;
		if (low_bytes > 0) {
			std::memcpy(&lower, data + 1, low_bytes);
		}
		if (mag_bytes > 8) {
			std::memcpy(&upper, data + 9, mag_bytes - 8);
		}
		hugeint_t magnitude;
		magnitude.lower = lower;
		magnitude.upper = static_cast<int64_t>(upper);
		return data[0] == 0 ? -magnitude : magnitude;
	}
#endif
	return ConvertDecimalPortable(data, length);
}

hugeint_t DecimalEncoding::ConvertMoney(const uint8_t *data) {
	// TDS MONEY is stored as two int32_t:
	// bytes 0-3: high-order 32 bits (little-endian)
	// bytes 4-7: low-order 32 bits (little-endian)
	// Value represents amount × 10000
	int32_t high = 0;
	int32_t low = 0;
	std::memcpy(&high, data, 4);
	std::memcpy(&low, data + 4, 4);

	int64_t value = (static_cast<int64_t>(high) << 32) | static_cast<uint32_t>(low);
	return hugeint_t(value);
}

hugeint_t DecimalEncoding::ConvertSmallMoney(const uint8_t *data) {
	// SMALLMONEY is stored as int32_t little-endian
	// Value represents amount × 10000
	int32_t value = 0;
	std::memcpy(&value, data, 4);

	return hugeint_t(static_cast<int64_t>(value));
}

}  // namespace encoding
}  // namespace tds
}  // namespace duckdb
