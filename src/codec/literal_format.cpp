//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension — spec 045
//
// codec/literal_format.cpp
//
// Phase-2 shell. The body is the canonical 9-arm dispatcher; each arm
// currently throws NotImplementedException because no family has been
// migrated yet. As each family lands its codec module (Phase 3 -> US1
// Integer, Phase 4 -> US2 wires call sites, Phase 5 -> US5 String,
// Phase 6 -> US3 remaining families), its arm in this switch is
// replaced with a direct call into codec::<family>::FormatSqlLiteral.
//
// Nothing in production calls codec::FormatSqlLiteral yet — Phase 4
// (filter_encoder.cpp:ValueToSQLLiteral and
// mssql_value_serializer.cpp:Serialize one-liner rewrite) is the first
// caller, and it lands only after the Integer arm is real.
//===----------------------------------------------------------------------===//

#include "codec/literal_format.hpp"

#include "codec/type_family.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {
namespace codec {

namespace {

[[noreturn]] void ThrowFamilyNotMigrated(const char *family_name, const LogicalType &type) {
	throw NotImplementedException("codec::FormatSqlLiteral: %s family not yet migrated to codec layer (type '%s')",
								  family_name, type.ToString());
}

}  // namespace

std::string FormatSqlLiteral(const Value &v, const LogicalType &type, LiteralContext ctx) {
	(void)ctx;
	if (v.IsNull()) {
		return "NULL";
	}
	switch (FamilyFromLogicalType(type)) {
	case TypeFamily::Boolean:
		ThrowFamilyNotMigrated("Boolean", type);
	case TypeFamily::Integer:
		ThrowFamilyNotMigrated("Integer", type);
	case TypeFamily::Float:
		ThrowFamilyNotMigrated("Float", type);
	case TypeFamily::Decimal:
		ThrowFamilyNotMigrated("Decimal", type);
	case TypeFamily::Money:
		ThrowFamilyNotMigrated("Money", type);
	case TypeFamily::String:
		ThrowFamilyNotMigrated("String", type);
	case TypeFamily::Binary:
		ThrowFamilyNotMigrated("Binary", type);
	case TypeFamily::DateTime:
		ThrowFamilyNotMigrated("DateTime", type);
	case TypeFamily::Uuid:
		ThrowFamilyNotMigrated("Uuid", type);
	}
	throw InternalException("codec::FormatSqlLiteral: unreachable (TypeFamily enum exhausted)");
}

size_t EstimateLiteralSize(const LogicalType &type) {
	switch (FamilyFromLogicalType(type)) {
	case TypeFamily::Boolean:
		ThrowFamilyNotMigrated("Boolean", type);
	case TypeFamily::Integer:
		ThrowFamilyNotMigrated("Integer", type);
	case TypeFamily::Float:
		ThrowFamilyNotMigrated("Float", type);
	case TypeFamily::Decimal:
		ThrowFamilyNotMigrated("Decimal", type);
	case TypeFamily::Money:
		ThrowFamilyNotMigrated("Money", type);
	case TypeFamily::String:
		ThrowFamilyNotMigrated("String", type);
	case TypeFamily::Binary:
		ThrowFamilyNotMigrated("Binary", type);
	case TypeFamily::DateTime:
		ThrowFamilyNotMigrated("DateTime", type);
	case TypeFamily::Uuid:
		ThrowFamilyNotMigrated("Uuid", type);
	}
	throw InternalException("codec::EstimateLiteralSize: unreachable (TypeFamily enum exhausted)");
}

}  // namespace codec
}  // namespace duckdb
