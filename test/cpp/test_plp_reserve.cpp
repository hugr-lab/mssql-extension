// test/cpp/test_plp_reserve.cpp
//
// RowReader::ReadPLPType must not allocate on the strength of a server's
// declaration alone.
//
// A PLP value (a MAX type: varchar(max), nvarchar(max), varbinary(max), xml)
// arrives as an 8-byte total length, then chunks, then a terminator. The reader
// pre-reserved that total length, bounded only by `< 0x7FFFFFFF` -- so a
// response declaring ~2 GB and then ending bought a 2 GB allocation before a
// single payload byte arrived. Nothing was written out of bounds and the memory
// was released, which made it remote memory amplification rather than
// corruption; the LOB path next door already capped (MAX_LOB_VALUE_BYTES), and
// that inconsistency was the bug.
//
// The cap is the buffer in hand: a parse only succeeds with the whole value
// buffered (every chunk needs `offset + chunk_length <= length`), so reserving
// past `length` can never be the allocation this call needs.
//
// Found by the q4 query in joern/ ("allocation sized by a declared length").
//
// Driven through the public single-value entry point, RowReader::ReadValue,
// which routes a column with max_length == 0xFFFF (ColumnMetadata::IsPLPType)
// into the PLP reader -- the same seam the staged read path uses. ReadPLPType
// itself is private and stays that way.
//
// No SQL Server, no DuckDB runtime. Part of STANDALONE_TEST_SOURCES
// (`make test-cpp`), which CI runs.

#include <cstdint>
#include <iostream>
#include <vector>

#include "tds/tds_column_metadata.hpp"
#include "tds/tds_row_reader.hpp"
#include "tds/tds_types.hpp"

using duckdb::tds::ColumnMetadata;
using duckdb::tds::RowReader;

namespace {

int failures = 0;

#define CHECK_TRUE(expr)                                             \
	do {                                                             \
		if (!(expr)) {                                               \
			++failures;                                              \
			std::cerr << "FAIL [" << __LINE__ << "] " #expr << "\n"; \
		}                                                            \
	} while (0)

#define CHECK_LE(actual, bound)                                                                     \
	do {                                                                                            \
		const auto _a = (actual);                                                                   \
		const auto _b = (bound);                                                                    \
		if (!(_a <= _b)) {                                                                          \
			++failures;                                                                             \
			std::cerr << "FAIL [" << __LINE__ << "] " #actual " <= " #bound << "\n  actual: " << _a \
					  << "\n  bound:  " << _b << "\n";                                              \
		}                                                                                           \
	} while (0)

std::vector<ColumnMetadata> OnePlpColumn() {
	ColumnMetadata col{};  // value-initialised: precision, scale, collation all 0
	col.name = "v";
	col.type_id = duckdb::tds::TDS_TYPE_BIGVARCHAR;
	col.max_length = 0xFFFF;  // the MAX sentinel: this column arrives as PLP
	col.flags = duckdb::tds::COL_FLAG_NULLABLE;
	return std::vector<ColumnMetadata>{col};
}

// An 8-byte little-endian PLP total length, and nothing after it.
std::vector<uint8_t> PlpHeader(uint64_t total_length) {
	std::vector<uint8_t> wire(8);
	for (size_t i = 0; i < 8; i++) {
		wire[i] = static_cast<uint8_t>((total_length >> (i * 8)) & 0xFF);
	}
	return wire;
}

void TestHugeDeclarationDoesNotAllocate() {
	std::cout << "Test: a 2 GB PLP declaration with no payload allocates nothing\n";

	auto columns = OnePlpColumn();
	RowReader reader(columns);

	// Just under the 0x7FFFFFFF bound the reader checks, which is what made the
	// old reserve legal: 2147483646 bytes declared, 8 bytes in hand.
	auto wire = PlpHeader(0x7FFFFFFEULL);
	std::vector<uint8_t> value;
	bool is_null = true;

	size_t consumed = reader.ReadValue(wire.data(), wire.size(), 0, value, is_null);

	// The chunk header is not there, so the reader asks for more data.
	CHECK_TRUE(consumed == 0);
	// And it did not reserve the declaration while asking. The bound is far
	// above the 8 bytes the cap permits and far below the 2 GB it used to take,
	// so no allocator growth policy can make this pass by accident.
	CHECK_LE(value.capacity(), static_cast<size_t>(4096));
}

void TestDeclaredLengthStillReservedWhenItFits() {
	std::cout << "Test: a declaration that fits the buffer is still pre-reserved\n";

	auto columns = OnePlpColumn();
	RowReader reader(columns);

	// 8-byte header, one 4-byte chunk header, 4 payload bytes, 4-byte
	// terminator: a complete 4-byte value.
	std::vector<uint8_t> wire = PlpHeader(4);
	const uint8_t rest[] = {4, 0, 0, 0, 'a', 'b', 'c', 'd', 0, 0, 0, 0};
	wire.insert(wire.end(), rest, rest + sizeof(rest));

	std::vector<uint8_t> value;
	bool is_null = true;
	size_t consumed = reader.ReadValue(wire.data(), wire.size(), 0, value, is_null);

	CHECK_TRUE(consumed == wire.size());
	CHECK_TRUE(!is_null);
	CHECK_TRUE(value.size() == 4);
	CHECK_TRUE(value[0] == 'a' && value[3] == 'd');
	// The optimisation the cap had to keep: the value's own length was reserved
	// up front, so the four bytes did not grow the vector.
	CHECK_TRUE(value.capacity() >= 4);
}

void TestNullAndUnknownMarkers() {
	std::cout << "Test: the NULL and UNKNOWN-length markers are unaffected\n";

	auto columns = OnePlpColumn();
	RowReader reader(columns);

	{
		auto wire = PlpHeader(0xFFFFFFFFFFFFFFFFULL);  // PLP_NULL_MARKER
		std::vector<uint8_t> value;
		bool is_null = false;
		size_t consumed = reader.ReadValue(wire.data(), wire.size(), 0, value, is_null);
		CHECK_TRUE(consumed == 8);
		CHECK_TRUE(is_null);
		CHECK_TRUE(value.empty());
	}
	{
		// PLP_UNKNOWN_MARKER: length not declared, so nothing to reserve. One
		// chunk of two bytes, then the terminator.
		std::vector<uint8_t> wire = PlpHeader(0xFFFFFFFFFFFFFFFEULL);
		const uint8_t rest[] = {2, 0, 0, 0, 'h', 'i', 0, 0, 0, 0};
		wire.insert(wire.end(), rest, rest + sizeof(rest));
		std::vector<uint8_t> value;
		bool is_null = true;
		size_t consumed = reader.ReadValue(wire.data(), wire.size(), 0, value, is_null);
		CHECK_TRUE(consumed == wire.size());
		CHECK_TRUE(!is_null);
		CHECK_TRUE(value.size() == 2);
		CHECK_TRUE(value[0] == 'h' && value[1] == 'i');
	}
}

}  // namespace

int main() {
	TestHugeDeclarationDoesNotAllocate();
	TestDeclaredLengthStillReservedWhenItFits();
	TestNullAndUnknownMarkers();

	if (failures == 0) {
		std::cout << "\nAll PLP reserve tests passed.\n";
		return 0;
	}
	std::cerr << "\n" << failures << " PLP reserve test(s) FAILED.\n";
	return 1;
}
