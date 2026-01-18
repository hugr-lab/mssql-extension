#include "tds/encoding/decimal_encoding.hpp"
#include <cstring>

namespace duckdb {
namespace tds {
namespace encoding {

hugeint_t DecimalEncoding::ConvertDecimal(const uint8_t* data, size_t length) {
	if (length == 0) {
		return hugeint_t(0);
	}

	// First byte is sign: 0 = negative, 1 = positive
	bool negative = data[0] == 0;

	// Remaining bytes are magnitude (little-endian)
	hugeint_t magnitude(0);

	for (size_t i = length - 1; i >= 1; i--) {
		magnitude = magnitude * hugeint_t(256) + hugeint_t(data[i]);
	}

	return negative ? -magnitude : magnitude;
}

hugeint_t DecimalEncoding::ConvertMoney(const uint8_t* data) {
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

hugeint_t DecimalEncoding::ConvertSmallMoney(const uint8_t* data) {
	// SMALLMONEY is stored as int32_t little-endian
	// Value represents amount × 10000
	int32_t value = 0;
	std::memcpy(&value, data, 4);

	return hugeint_t(static_cast<int64_t>(value));
}

}  // namespace encoding
}  // namespace tds
}  // namespace duckdb
