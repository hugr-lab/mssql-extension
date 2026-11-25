// test/cpp/test_write_column_ops.cpp
//
// ResolveWriteColumnOps (codec/write_column_ops.hpp): which columnar kernel writes a source type into a
// target column, or that the column keeps the row path. A pure function of the two, so it is asked
// directly; it had no test of its own, only the encodings that follow from its answers.
//
// One regression is pinned (found reading the resolver): a datetime2 / datetimeoffset target took the
// datetime kernel for ANY source stored in eight bytes. The kernel asks the source's type for its tick
// unit and answered a BIGINT, a TIME or a DECIMAL(18) with an InternalException -- and so did the row
// path's encoder (review of #629). The resolver keeps such a pair on the row path, and the row path
// refuses it by name. The rest pins what the resolver already answered.
//
// Part of STANDALONE_TEST_SOURCES (`make test-cpp`), which CI runs.

#include <iostream>
#include <string>
#include <vector>

#include "codec/write_column_ops.hpp"
#include "copy/target_resolver.hpp"
#include "duckdb.hpp"
#include "tds/encoding/bcp_row_encoder.hpp"

using namespace duckdb;
using namespace duckdb::mssql;
using codec::ScatterArm;
using codec::WireKind;

namespace {

int failures = 0;

void Check(bool ok, const std::string &what) {
	std::cout << (ok ? "  ok   " : "  FAIL ") << what << std::endl;
	if (!ok) {
		failures++;
	}
}

BCPColumnMetadata Column(const std::string &type, int16_t max_length, uint8_t precision = 0, uint8_t scale = 0) {
	return BCPColumnMetadata::FromServerColumn("c", type, max_length, precision, scale, true, "");
}

ScatterArm Arm(const LogicalType &source, const BCPColumnMetadata &target) {
	return codec::ResolveWriteColumnOps(source, target).arm;
}

void TestDatetime() {
	std::cout << "datetime2 / datetimeoffset / date / time" << std::endl;
	const auto datetime2 = Column("datetime2", 8, 27, 7);
	const auto datetime2_3 = Column("datetime2", 7, 23, 3);
	const auto offset = Column("datetimeoffset", 10, 34, 7);
	for (const auto &source :
		 std::vector<LogicalType>{LogicalType::TIMESTAMP, LogicalType::TIMESTAMP_S, LogicalType::TIMESTAMP_MS,
								  LogicalType::TIMESTAMP_NS, LogicalType::TIMESTAMP_TZ}) {
		Check(Arm(source, datetime2) == ScatterArm::Datetime && Arm(source, datetime2_3) == ScatterArm::Datetime,
			  source.ToString() + " into datetime2 takes the datetime kernel");
		Check(Arm(source, offset) == ScatterArm::Datetime, source.ToString() + " into datetimeoffset too");
	}
	Check(Arm(LogicalType::DATE, datetime2) == ScatterArm::Datetime, "a DATE into datetime2 has a kernel of its own");
	// Regression: each of these is eight bytes, and took the datetime kernel.
	for (const auto &source : std::vector<LogicalType>{LogicalType::BIGINT, LogicalType::TIME, LogicalType::TIME_TZ,
													   LogicalType::DECIMAL(18, 4)}) {
		Check(Arm(source, datetime2) == ScatterArm::RowFallback && Arm(source, offset) == ScatterArm::RowFallback,
			  source.ToString() + " into datetime2 / datetimeoffset keeps the row path");
	}
	Check(Arm(LogicalType::DATE, offset) == ScatterArm::RowFallback, "a DATE into datetimeoffset keeps the row path");

	const auto date = Column("date", 3, 10, 0);
	Check(Arm(LogicalType::DATE, date) == ScatterArm::Datetime &&
			  Arm(LogicalType::INTEGER, date) == ScatterArm::RowFallback &&
			  Arm(LogicalType::TIMESTAMP, date) == ScatterArm::RowFallback,
		  "date takes a DATE and nothing else stored alike");
	const auto time = Column("time", 5, 16, 7);
	Check(Arm(LogicalType::TIME, time) == ScatterArm::Datetime &&
			  Arm(LogicalType::BIGINT, time) == ScatterArm::RowFallback,
		  "time takes a TIME and nothing else stored alike");
	// The width the kernel writes must be the width the column declares.
	auto short_datetime2 = datetime2;
	short_datetime2.max_length = 7;
	Check(Arm(LogicalType::TIMESTAMP, short_datetime2) == ScatterArm::RowFallback,
		  "a datetime2(7) declared one byte short keeps the row path");
}

//! `rows` values of `source` encoded for one column `target`; what the encoder threw, empty when it did not.
std::string EncodeError(const LogicalType &source, const BCPColumnMetadata &target) {
	DataChunk chunk;
	chunk.Initialize(Allocator::DefaultAllocator(), {source});
	const idx_t rows = 3;
	for (idx_t row = 0; row < rows; row++) {
		const int64_t micros = 1700000000000000LL + static_cast<int64_t>(row);
		chunk.SetValue(0, row,
					   source.id() == LogicalTypeId::BIGINT
						   ? Value::BIGINT(micros)
						   : Value::TIMESTAMP(timestamp_t(micros)).DefaultCastAs(source));
	}
	chunk.SetChildCardinality(rows);
	vector<BCPColumnMetadata> columns = {target};
	vector<uint8_t> bytes;
	try {
		tds::encoding::BCPRowEncoder::EncodeChunk(bytes, chunk, columns, nullptr);
	} catch (std::exception &e) {
		return ErrorData(e).RawMessage().empty() ? std::string("?") : ErrorData(e).Message();
	}
	return bytes.empty() ? std::string("no bytes") : std::string();
}

//! Regression (review of #629): the row path met the same InternalException as the kernel.
void TestRowPathRefusesByName() {
	std::cout << "the row path's refusal" << std::endl;
	auto datetime2 = Column("datetime2", 8, 27, 7);
	datetime2.name = "created";
	auto offset = Column("datetimeoffset", 10, 34, 7);
	offset.name = "seen";
	Check(EncodeError(LogicalType::TIMESTAMP, datetime2).empty() &&
			  EncodeError(LogicalType::TIMESTAMP_TZ, offset).empty(),
		  "a TIMESTAMP into datetime2 and a TIMESTAMPTZ into datetimeoffset are written");
	const auto bigint = EncodeError(LogicalType::BIGINT, datetime2);
	Check(bigint.find("Invalid Input") != std::string::npos && bigint.find("created") != std::string::npos &&
			  bigint.find("BIGINT") != std::string::npos && bigint.find("datetime2") != std::string::npos &&
			  bigint.find("INTERNAL") == std::string::npos,
		  "a BIGINT into datetime2 is refused, naming the column and both types (" + bigint + ")");
	const auto into_offset = EncodeError(LogicalType::BIGINT, offset);
	Check(into_offset.find("Invalid Input") != std::string::npos && into_offset.find("seen") != std::string::npos &&
			  into_offset.find("datetimeoffset") != std::string::npos,
		  "and into datetimeoffset (" + into_offset + ")");
}

void TestNumbers() {
	std::cout << "bit, the integers, real and float" << std::endl;
	const auto int_column = Column("int", 4, 10, 0);
	const auto ops = codec::ResolveWriteColumnOps(LogicalType::INTEGER, int_column);
	Check(ops.arm == ScatterArm::DirectCopy4 && ops.kind == WireKind::Fixed && ops.wire_width == 4 &&
			  ops.src_width == 0 && ops.CanScatter() && !ops.IsVariable(),
		  "an INTEGER into int is copied, four bytes");
	const auto widened = codec::ResolveWriteColumnOps(LogicalType::SMALLINT, Column("bigint", 8, 19, 0));
	Check(widened.arm == ScatterArm::IntConvert && widened.wire_width == 8 && widened.src_width == 2,
		  "a SMALLINT into bigint is converted: eight bytes written, two read");
	const auto narrowed = codec::ResolveWriteColumnOps(LogicalType::DOUBLE, Column("real", 4, 24, 0));
	Check(narrowed.arm == ScatterArm::FloatConvert && narrowed.wire_width == 4 && narrowed.src_width == 8,
		  "a DOUBLE into real is converted");
	Check(Arm(LogicalType::DOUBLE, int_column) == ScatterArm::RowFallback, "a DOUBLE into int keeps the row path");
	// Same width, another sign: copying the bytes would put another number there.
	Check(Arm(LogicalType::UINTEGER, int_column) == ScatterArm::RowFallback &&
			  Arm(LogicalType::UBIGINT, Column("bigint", 8, 19, 0)) == ScatterArm::RowFallback &&
			  Arm(LogicalType::USMALLINT, Column("smallint", 2, 5, 0)) == ScatterArm::RowFallback,
		  "an unsigned source into a signed target of its width keeps the row path");
	const auto tinyint = Column("tinyint", 1, 3, 0);
	Check(Arm(LogicalType::UTINYINT, tinyint) == ScatterArm::DirectCopy1 &&
			  Arm(LogicalType::TINYINT, tinyint) == ScatterArm::RowFallback,
		  "tinyint is unsigned: a UTINYINT is copied, a signed TINYINT keeps the row path");
	Check(Arm(LogicalType::BOOLEAN, Column("bit", 1, 1, 0)) == ScatterArm::DirectCopy1, "a BOOLEAN into bit is copied");
	auto wide_int = int_column;
	wide_int.max_length = 8;
	Check(Arm(LogicalType::INTEGER, wide_int) == ScatterArm::RowFallback,
		  "an int declared with another width than its type's keeps the row path");
}

void TestTheRest() {
	std::cout << "decimal, uniqueidentifier, strings and binaries" << std::endl;
	const auto decimal = Column("decimal", 9, 18, 4);
	for (const auto &source : std::vector<LogicalType>{
			 LogicalType::DECIMAL(18, 4), LogicalType::DECIMAL(4, 2), LogicalType::DECIMAL(38, 10),
			 LogicalType::INTEGER, LogicalType::BIGINT, LogicalType::UBIGINT, LogicalType::HUGEINT}) {
		Check(Arm(source, decimal) == ScatterArm::Decimal,
			  source.ToString() + " into decimal takes the decimal kernel");
	}
	Check(Arm(LogicalType::DOUBLE, decimal) == ScatterArm::RowFallback &&
			  Arm(LogicalType::TINYINT, decimal) == ScatterArm::RowFallback,
		  "a DOUBLE or a TINYINT into decimal keeps the row path");
	const auto guid = Column("uniqueidentifier", 16);
	Check(
		Arm(LogicalType::UUID, guid) == ScatterArm::Guid && Arm(LogicalType::HUGEINT, guid) == ScatterArm::RowFallback,
		"uniqueidentifier takes a UUID, not a HUGEINT");
	for (const auto &target :
		 std::vector<BCPColumnMetadata>{Column("nvarchar", 100), Column("varchar", 50), Column("varbinary", 64),
										Column("nvarchar", -1), Column("varchar", -1), Column("varbinary", -1)}) {
		const auto text = codec::ResolveWriteColumnOps(LogicalType::VARCHAR, target);
		Check(text.arm == ScatterArm::VarString && text.IsVariable() && text.kind != WireKind::Fixed &&
				  Arm(LogicalType::BLOB, target) == ScatterArm::VarString,
			  "a string or a blob into " + target.server_type_name + " takes the variable-width kernel");
		Check(Arm(LogicalType::INTERVAL, target) == ScatterArm::RowFallback &&
				  Arm(LogicalType::INTEGER, target) == ScatterArm::RowFallback,
			  "a source rendered as text keeps the row path");
	}
}

}  // namespace

int main() {
	TestDatetime();
	TestRowPathRefusesByName();
	TestNumbers();
	TestTheRest();
	std::cout << (failures ? "FAILED" : "all passed") << std::endl;
	return failures ? 1 : 0;
}
