//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension — spec 045
//
// codec/literal_format.cpp
//
// Canonical 9-arm dispatcher. As each family migrates, its arm is
// replaced with a direct call into codec::<family>::FormatSqlLiteral.
//
// Phase 6 (US3 sub-phases 1-2) — Boolean and Float arms wired. Integer
// and String arms landed in Phase 4/Phase 5. The remaining 5 arms still
// throw NotImplementedException; they are unreachable in production
// until the corresponding family migration phase lands the dispatch-site
// rewrites that route through this dispatcher.
//===----------------------------------------------------------------------===//

#include "codec/literal_format.hpp"

#include "codec/boolean_codec.hpp"
#include "codec/decimal_codec.hpp"
#include "codec/float_codec.hpp"
#include "codec/integer_codec.hpp"
#include "codec/string_codec.hpp"
#include "codec/type_family.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {
namespace mssql {
namespace codec {

namespace {

[[noreturn]] void ThrowFamilyNotMigrated(const char *family_name, const LogicalType &type) {
	throw NotImplementedException("codec::FormatSqlLiteral: %s family not yet migrated to codec layer (type '%s')",
								  family_name, type.ToString());
}

}  // namespace

std::string FormatSqlLiteral(const Value &v, const LogicalType &type, LiteralContext ctx) {
	if (v.IsNull()) {
		return "NULL";
	}
	switch (FamilyFromLogicalType(type)) {
	case TypeFamily::Boolean:
		return boolean::FormatSqlLiteral(v, type, ctx);
	case TypeFamily::Integer:
		return integer::FormatSqlLiteral(v, type, ctx);
	case TypeFamily::Float:
		return float_family::FormatSqlLiteral(v, type, ctx);
	case TypeFamily::Decimal:
		return decimal::FormatSqlLiteral(v, type, ctx);
	case TypeFamily::Money:
		ThrowFamilyNotMigrated("Money", type);
	case TypeFamily::String:
		return string::FormatSqlLiteral(v, type, ctx);
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
		return boolean::EstimateLiteralSize(type);
	case TypeFamily::Integer:
		return integer::EstimateLiteralSize(type);
	case TypeFamily::Float:
		return float_family::EstimateLiteralSize(type);
	case TypeFamily::Decimal:
		return decimal::EstimateLiteralSize(type);
	case TypeFamily::Money:
		ThrowFamilyNotMigrated("Money", type);
	case TypeFamily::String:
		return string::EstimateLiteralSize(type);
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
}  // namespace mssql
}  // namespace duckdb
