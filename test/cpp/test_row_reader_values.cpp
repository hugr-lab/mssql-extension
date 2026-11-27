// test/cpp/test_row_reader_values.cpp
//
// RowReader's per-value READERS, ReadValue (a ROW's value) and ReadValueNBC (a value an NBCROW's
// bitmap says is present). test_skip_form_equivalence holds the skip walks to each other and never
// reads a value; this holds the readers to the skip walks and pins what the two row forms do
// differently (review of #648, which replaced the readers' per-type switches by one framing table):
//
//   * a well-formed value is READ in exactly the bytes it is SKIPPED in, for every TDS type the reader
//     takes, in both row forms, and the bytes read are the value's;
//   * a 1-byte length of 0 is NULL in a ROW and an empty value in an NBCROW, where the bitmap alone
//     says NULL;
//   * a ROW's UNIQUEIDENTIFIER is its prefix and 16 bytes; an NBCROW's is read by its prefix;
//   * a legacy LOB (TEXT / NTEXT / IMAGE) can be skipped and not read, an unknown type neither, and
//     each refusal names the row form and the type;
//   * a value that has not all arrived answers 0 and -- a PLP value excepted, which is assembled in
//     place -- leaves `value` as it was.
//
// The framing of each type is stated HERE, by this file's own table, not asked of the reader.
//
// TDS-only by construction: no DuckDB headers, so it links without libduckdb and runs in the
// `cpp-unit-tests` CI job on every PR. Shape follows test_skip_form_equivalence.cpp.
//
// Build & run:
//   make test-row-reader-values

#include "tds/tds_column_metadata.hpp"
#include "tds/tds_row_reader.hpp"
#include "tds/tds_types.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using duckdb::tds::ColumnMetadata;
using duckdb::tds::RowReader;

namespace tds = duckdb::tds;

namespace {

int failures = 0;
int checks = 0;

void Check(bool ok, const std::string &what) {
	checks++;
	if (!ok) {
		std::cerr << "FAIL: " << what << std::endl;
		failures++;
	}
}

std::string HexByte(uint8_t v) {
	static const char *d = "0123456789ABCDEF";
	return std::string("0x") + d[(v >> 4) & 0xF] + d[v & 0xF];
}

typedef std::vector<uint8_t> Bytes;

//! How a type's value is framed on the wire -- this file's statement of it.
enum class Framing { UNKNOWN, FIXED, BYTE_PREFIX, SHORT_PREFIX_OR_PLP, PLP, LEGACY_LOB, VARIANT };

struct TypeFraming {
	uint8_t type_id;
	Framing framing;
	size_t width;  // FIXED: the value's bytes
};

const std::vector<TypeFraming> &KnownTypes() {
	static const std::vector<TypeFraming> types = {
		{tds::TDS_TYPE_TINYINT, Framing::FIXED, 1},
		{tds::TDS_TYPE_BIT, Framing::FIXED, 1},
		{tds::TDS_TYPE_SMALLINT, Framing::FIXED, 2},
		{tds::TDS_TYPE_INT, Framing::FIXED, 4},
		{tds::TDS_TYPE_REAL, Framing::FIXED, 4},
		{tds::TDS_TYPE_SMALLMONEY, Framing::FIXED, 4},
		{tds::TDS_TYPE_SMALLDATETIME, Framing::FIXED, 4},
		{tds::TDS_TYPE_BIGINT, Framing::FIXED, 8},
		{tds::TDS_TYPE_FLOAT, Framing::FIXED, 8},
		{tds::TDS_TYPE_MONEY, Framing::FIXED, 8},
		{tds::TDS_TYPE_DATETIME, Framing::FIXED, 8},
		{tds::TDS_TYPE_INTN, Framing::BYTE_PREFIX, 0},
		{tds::TDS_TYPE_BITN, Framing::BYTE_PREFIX, 0},
		{tds::TDS_TYPE_FLOATN, Framing::BYTE_PREFIX, 0},
		{tds::TDS_TYPE_MONEYN, Framing::BYTE_PREFIX, 0},
		{tds::TDS_TYPE_DATETIMEN, Framing::BYTE_PREFIX, 0},
		{tds::TDS_TYPE_DECIMAL, Framing::BYTE_PREFIX, 0},
		{tds::TDS_TYPE_NUMERIC, Framing::BYTE_PREFIX, 0},
		{tds::TDS_TYPE_DATE, Framing::BYTE_PREFIX, 0},
		{tds::TDS_TYPE_TIME, Framing::BYTE_PREFIX, 0},
		{tds::TDS_TYPE_DATETIME2, Framing::BYTE_PREFIX, 0},
		{tds::TDS_TYPE_DATETIMEOFFSET, Framing::BYTE_PREFIX, 0},
		{tds::TDS_TYPE_UNIQUEIDENTIFIER, Framing::BYTE_PREFIX, 0},
		{tds::TDS_TYPE_BIGCHAR, Framing::SHORT_PREFIX_OR_PLP, 0},
		{tds::TDS_TYPE_BIGVARCHAR, Framing::SHORT_PREFIX_OR_PLP, 0},
		{tds::TDS_TYPE_NCHAR, Framing::SHORT_PREFIX_OR_PLP, 0},
		{tds::TDS_TYPE_NVARCHAR, Framing::SHORT_PREFIX_OR_PLP, 0},
		{tds::TDS_TYPE_BIGBINARY, Framing::SHORT_PREFIX_OR_PLP, 0},
		{tds::TDS_TYPE_BIGVARBINARY, Framing::SHORT_PREFIX_OR_PLP, 0},
		{tds::TDS_TYPE_VECTOR, Framing::SHORT_PREFIX_OR_PLP, 0},
		{tds::TDS_TYPE_XML, Framing::PLP, 0},
		{tds::TDS_TYPE_UDT, Framing::PLP, 0},
		{tds::TDS_TYPE_SQL_VARIANT, Framing::VARIANT, 0},
		{tds::TDS_TYPE_TEXT, Framing::LEGACY_LOB, 0},
		{tds::TDS_TYPE_NTEXT, Framing::LEGACY_LOB, 0},
		{tds::TDS_TYPE_IMAGE, Framing::LEGACY_LOB, 0},
	};
	return types;
}

TypeFraming FramingOf(uint8_t type_id) {
	for (const auto &t : KnownTypes()) {
		if (t.type_id == type_id) {
			return t;
		}
	}
	return {type_id, Framing::UNKNOWN, 0};
}

ColumnMetadata MakeCol(uint8_t type_id, uint16_t max_length) {
	ColumnMetadata c;
	c.name = "c";
	c.type_id = type_id;
	c.max_length = max_length;
	c.collation = 0;
	c.flags = tds::COL_FLAG_NULLABLE;
	return c;
}

void Le(Bytes &b, uint64_t v, int n) {
	for (int i = 0; i < n; i++) {
		b.push_back(static_cast<uint8_t>(v >> (8 * i)));
	}
}

Bytes Payload(size_t n) {
	Bytes p;
	for (size_t i = 0; i < n; i++) {
		p.push_back(static_cast<uint8_t>(0x30 + i));
	}
	return p;
}

//! A well-formed value of `col` on the wire, and (`payload`) the bytes a reader hands back for it.
Bytes WireValue(const ColumnMetadata &col, Bytes &payload) {
	const TypeFraming t = FramingOf(col.type_id);
	Bytes wire;
	switch (t.framing) {
	case Framing::FIXED:
		payload = Payload(t.width);
		wire = payload;
		break;
	case Framing::BYTE_PREFIX:
		payload = Payload(col.type_id == tds::TDS_TYPE_UNIQUEIDENTIFIER ? 16 : 5);
		wire.push_back(static_cast<uint8_t>(payload.size()));
		wire.insert(wire.end(), payload.begin(), payload.end());
		break;
	case Framing::SHORT_PREFIX_OR_PLP:
	case Framing::PLP:
		payload = Payload(12);
		if (t.framing == Framing::PLP || col.IsPLPType()) {
			// 8-byte total, one chunk (4-byte length + bytes), the 4-byte terminator
			Le(wire, payload.size(), 8);
			Le(wire, payload.size(), 4);
			wire.insert(wire.end(), payload.begin(), payload.end());
			Le(wire, 0, 4);
		} else {
			Le(wire, payload.size(), 2);
			wire.insert(wire.end(), payload.begin(), payload.end());
		}
		break;
	case Framing::VARIANT:
		payload = Payload(6);
		Le(wire, payload.size(), 4);
		wire.insert(wire.end(), payload.begin(), payload.end());
		break;
	case Framing::LEGACY_LOB:
		// text-pointer length, the pointer, an 8-byte timestamp, a 4-byte length, the bytes
		payload = Payload(9);
		wire.push_back(16);
		wire.insert(wire.end(), 16 + 8, 0x7E);
		Le(wire, payload.size(), 4);
		wire.insert(wire.end(), payload.begin(), payload.end());
		break;
	case Framing::UNKNOWN:
		break;
	}
	return wire;
}

struct Read {
	size_t consumed = 0;
	Bytes value;
	bool is_null = false;
	bool threw = false;
	std::string what;
};

//! One read of `wire` (its first `length` bytes) as column `col`; `value` starts as `before`.
Read ReadOne(const ColumnMetadata &col, const Bytes &wire, size_t length, bool nbc, const Bytes &before = Bytes()) {
	std::vector<ColumnMetadata> cols(1, col);
	RowReader reader(cols);
	Read r;
	r.value = before;
	r.is_null = true;  // dirty: the reader says false itself, on every way out
	// One byte past the value, so a reader that looks beyond `length` reads the canary, not the heap.
	Bytes buffer(wire.begin(), wire.begin() + static_cast<std::ptrdiff_t>(length));
	buffer.push_back(0xEE);
	try {
		r.consumed = nbc ? reader.ReadValueNBC(buffer.data(), length, 0, r.value, r.is_null)
						 : reader.ReadValue(buffer.data(), length, 0, r.value, r.is_null);
	} catch (const std::runtime_error &e) {
		r.threw = true;
		r.what = e.what();
	}
	return r;
}

size_t SkipOne(const ColumnMetadata &col, const Bytes &wire, bool nbc) {
	std::vector<ColumnMetadata> cols(1, col);
	RowReader reader(cols);
	return nbc ? reader.SkipValueNBC(wire.data(), wire.size(), 0) : reader.SkipValue(wire.data(), wire.size(), 0);
}

std::string Label(const ColumnMetadata &col, bool nbc) {
	return std::string(nbc ? "NBCROW " : "ROW ") + HexByte(col.type_id) + (col.IsPLPType() ? " (MAX)" : "");
}

//! The columns of a type: bounded, and MAX where the type has that form.
std::vector<ColumnMetadata> ColumnsOf(uint8_t type_id) {
	std::vector<ColumnMetadata> cols = {MakeCol(type_id, 40)};
	const ColumnMetadata max = MakeCol(type_id, 0xFFFF);
	if (FramingOf(type_id).framing == Framing::SHORT_PREFIX_OR_PLP && max.IsPLPType()) {
		cols.push_back(max);
	}
	return cols;
}

// [1] Every type id, both row forms: read in the bytes it is skipped in, or refused by name.
void TestEveryType() {
	std::cout << "[1] every type id: a value is read in the bytes it is skipped in, or refused by name..." << std::endl;
	for (int id = 0; id < 256; id++) {
		const TypeFraming t = FramingOf(static_cast<uint8_t>(id));
		for (const auto &col : ColumnsOf(static_cast<uint8_t>(id))) {
			for (int nbc = 0; nbc < 2; nbc++) {
				const std::string label = Label(col, nbc != 0);
				Bytes payload;
				const Bytes wire = WireValue(col, payload);
				const std::string refusal =
					std::string(nbc ? "Unsupported type in NBC RowReader: " : "Unsupported type in RowReader: ") +
					col.GetTypeName();
				if (t.framing == Framing::UNKNOWN) {
					// Neither skipped nor read. Any bytes will do: the type is refused before they are looked at.
					const Bytes any = Payload(8);
					Check(SkipOne(col, any, nbc != 0) == 0, label + ": an unknown type cannot be skipped");
					const Read r = ReadOne(col, any, any.size(), nbc != 0);
					Check(r.threw && r.what == refusal,
						  label + ": an unknown type is refused as '" + refusal + "', got '" + r.what + "'");
					continue;
				}
				const size_t skipped = SkipOne(col, wire, nbc != 0);
				Check(skipped == wire.size(),
					  label + ": skipped " + std::to_string(skipped) + " of " + std::to_string(wire.size()) + " bytes");
				const Read r = ReadOne(col, wire, wire.size(), nbc != 0);
				if (t.framing == Framing::LEGACY_LOB) {
					Check(r.threw && r.what == refusal,
						  label + ": a legacy LOB is refused as '" + refusal + "', got '" + r.what + "'");
					continue;
				}
				Check(!r.threw, label + ": read threw '" + r.what + "'");
				Check(r.consumed == skipped,
					  label + ": read " + std::to_string(r.consumed) + " bytes, skipped " + std::to_string(skipped));
				Check(r.value == payload, label + ": the bytes read are not the value's");
				Check(!r.is_null, label + ": a value read as NULL");
			}
		}
	}
}

// [2] A 1-byte length of 0: NULL in a ROW, an empty value in an NBCROW.
void TestZeroLengthPrefix() {
	std::cout << "[2] a 1-byte length of 0 is NULL in a ROW and an empty value in an NBCROW..." << std::endl;
	const Bytes zero = {0x00};
	const Bytes before = {0xAA, 0xBB};
	for (const auto &t : KnownTypes()) {
		if (t.framing != Framing::BYTE_PREFIX) {
			continue;
		}
		const ColumnMetadata col = MakeCol(t.type_id, 40);
		const Read row = ReadOne(col, zero, 1, false, before);
		Check(!row.threw && row.consumed == 1 && row.is_null && row.value.empty(),
			  Label(col, false) + ": a length of 0 is NULL, one byte, no value");
		const Read nbc = ReadOne(col, zero, 1, true, before);
		Check(!nbc.threw && nbc.consumed == 1 && !nbc.is_null && nbc.value.empty(),
			  Label(col, true) + ": a length of 0 is an empty value, one byte, not NULL");
		Check(SkipOne(col, zero, false) == 1 && SkipOne(col, zero, true) == 1,
			  HexByte(t.type_id) + ": a length of 0 is skipped as one byte");
	}
}

// [3] UNIQUEIDENTIFIER: a ROW's is its prefix and 16 bytes; an NBCROW's is read by its prefix.
void TestGuid() {
	std::cout << "[3] UNIQUEIDENTIFIER: 17 bytes in a ROW; by its prefix in an NBCROW..." << std::endl;
	const ColumnMetadata col = MakeCol(tds::TDS_TYPE_UNIQUEIDENTIFIER, 16);
	Bytes payload;
	const Bytes wire = WireValue(col, payload);
	Check(wire.size() == 17 && wire[0] == 16, "the fixture is a prefix of 16 and 16 bytes");
	for (int nbc = 0; nbc < 2; nbc++) {
		const Read r = ReadOne(col, wire, wire.size(), nbc != 0);
		Check(!r.threw && r.consumed == 17 && r.value == payload && !r.is_null,
			  Label(col, nbc != 0) + ": a GUID with prefix 16 is read as 17 bytes");
		Check(SkipOne(col, wire, nbc != 0) == 17,
			  Label(col, nbc != 0) + ": a GUID with prefix 16 is skipped as 17 bytes");
	}
	// The ROW reader takes 16 bytes whatever the prefix says, so 16 of the 17 bytes are not a value yet.
	const Read cut = ReadOne(col, wire, 16, false, {0xAA});
	Check(!cut.threw && cut.consumed == 0 && cut.value == Bytes({0xAA}), "ROW: a GUID one byte short needs more data");
	// ...and a prefix that is not 16 still reads 16: the ROW reader's rule as it stands, and where it
	// parts from the skip walk, which goes by the prefix (5 bytes here). No server sends such a GUID;
	// this records the reader's behaviour so a change to it is a decision, not an accident.
	Bytes odd = {4};
	const Bytes sixteen = Payload(16);
	odd.insert(odd.end(), sixteen.begin(), sixteen.end());
	const Read row_odd = ReadOne(col, odd, odd.size(), false);
	Check(!row_odd.threw && row_odd.consumed == 17 && row_odd.value == sixteen,
		  "ROW: a GUID is 16 bytes whatever its prefix says");
	Check(SkipOne(col, odd, false) == 5, "ROW: the skip walk goes by the GUID's prefix");
	// An NBCROW's is read by its prefix: 4 bytes are a whole value of 4.
	const Bytes four = {4, 1, 2, 3, 4};
	const Read nbc = ReadOne(col, four, four.size(), true);
	Check(!nbc.threw && nbc.consumed == 5 && nbc.value == Bytes({1, 2, 3, 4}), "NBCROW: a GUID is read by its prefix");
}

// [4] A value that has not all arrived: 0, and `value` as it was (a PLP value excepted).
void TestNeedsMoreData() {
	std::cout << "[4] a value cut short answers 0 and leaves `value` as it was..." << std::endl;
	const Bytes before = {0xAA, 0xBB, 0xCC};
	for (const auto &t : KnownTypes()) {
		if (t.framing == Framing::LEGACY_LOB) {
			continue;  // not read at all: [1]
		}
		for (const auto &col : ColumnsOf(t.type_id)) {
			for (int nbc = 0; nbc < 2; nbc++) {
				Bytes payload;
				const Bytes wire = WireValue(col, payload);
				const bool plp = t.framing == Framing::PLP || col.IsPLPType();
				for (size_t length = 0; length < wire.size(); length++) {
					const Read r = ReadOne(col, wire, length, nbc != 0, before);
					const std::string label = Label(col, nbc != 0) + ", " + std::to_string(length) + " of " +
											  std::to_string(wire.size()) + " bytes";
					Check(!r.threw, label + ": threw '" + r.what + "'");
					Check(r.consumed == 0, label + ": consumed " + std::to_string(r.consumed));
					// A PLP value is assembled chunk by chunk into `value` (ReadPLPType, as it always was),
					// so one cut short leaves the chunks that had arrived there. ReadRow clears the value
					// before every read and drops the row on a 0, so nothing reads them; not pinned.
					if (!plp) {
						Check(r.value == before, label + ": `value` was touched");
					}
					Check(!r.is_null, label + ": read as NULL");
				}
			}
		}
	}
}

}  // namespace

int main() {
	std::cout << "============================================================" << std::endl;
	std::cout << "RowReader: ReadValue / ReadValueNBC (review of #648)" << std::endl;
	std::cout << "============================================================" << std::endl;

	try {
		TestEveryType();
		TestZeroLengthPrefix();
		TestGuid();
		TestNeedsMoreData();
	} catch (const std::exception &e) {
		std::cerr << "UNCAUGHT EXCEPTION: " << e.what() << std::endl;
		return 2;
	}

	std::cout << "============================================================" << std::endl;
	std::cout << checks << " checks" << std::endl;
	if (failures > 0) {
		std::cerr << failures << " assertion(s) failed." << std::endl;
		return 1;
	}
	std::cout << "ALL TESTS PASSED" << std::endl;
	return 0;
}
