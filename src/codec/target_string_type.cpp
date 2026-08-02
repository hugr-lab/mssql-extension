#include "codec/target_string_type.hpp"

#include "duckdb/common/extension_type_info.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {
namespace mssql {
namespace codec {

//! The alias IS the type identity: one integer modifier cannot say whether it
//! means varchar or nvarchar. The alias is also not optional for a different
//! reason — an anonymous extension type crashes DuckDB's binder (spec 060 § 2a).
static constexpr const char *NVARCHAR_ALIAS = "MSSQL_NVARCHAR";
static constexpr const char *VARCHAR_ALIAS = "MSSQL_VARCHAR";

//! The collation travels as a PROPERTY rather than a second modifier so that it
//! stays out of the rendered type name: with the catalog reporting these types
//! for every string column, `DESCRIBE` would otherwise print the collation of
//! every varchar in the database.
static constexpr const char *COLLATION_PROPERTY = "collation";

LogicalType MakeTargetStringType(const TargetStringType &spec) {
	LogicalType type(LogicalTypeId::VARCHAR);
	type.SetAlias(spec.unicode ? NVARCHAR_ALIAS : VARCHAR_ALIAS);

	auto info = make_uniq<ExtensionTypeInfo>();
	info->modifiers.emplace_back(Value::INTEGER(spec.length));
	if (!spec.unicode && !spec.collation.empty()) {
		info->properties[COLLATION_PROPERTY] = Value(spec.collation);
	}
	type.SetExtensionInfo(std::move(info));
	return type;
}

bool TryGetTargetStringType(const LogicalType &type, TargetStringType &result) {
	if (type.id() != LogicalTypeId::VARCHAR || !type.HasAlias() || !type.HasExtensionInfo()) {
		return false;
	}

	const auto &alias = type.GetAlias();
	if (alias == NVARCHAR_ALIAS) {
		result.unicode = true;
	} else if (alias == VARCHAR_ALIAS) {
		result.unicode = false;
	} else {
		return false;
	}

	const auto &info = *type.GetExtensionInfo();
	if (info.modifiers.size() != 1 || info.modifiers[0].value.IsNull()) {
		return false;
	}
	result.length = info.modifiers[0].value.GetValue<int32_t>();

	const auto entry = info.properties.find(COLLATION_PROPERTY);
	result.collation = entry == info.properties.end() ? std::string() : entry->second.ToString();
	return true;
}

std::string FormatTargetStringDdl(const TargetStringType &spec, const std::string &fallback_collation) {
	if (spec.unicode) {
		return StringUtil::Format("nvarchar(%d)", spec.length);
	}

	std::string ddl = StringUtil::Format("varchar(%d)", spec.length);
	const std::string &collation = spec.collation.empty() ? fallback_collation : spec.collation;
	if (!collation.empty()) {
		ddl += " COLLATE " + collation;
	}
	return ddl;
}

LogicalType ApplyDefaultStringType(const LogicalType &type, bool unicode, int32_t length,
								   const std::string &collation) {
	if (length <= 0 || type.id() != LogicalTypeId::VARCHAR || type.HasAlias()) {
		return type;
	}
	// Past SQL Server's inline limit the column has to be MAX, which is what the
	// unannotated type already asks for.
	const int32_t limit = unicode ? MAX_NVARCHAR_LENGTH : MAX_VARCHAR_LENGTH;
	if (length > limit) {
		return type;
	}

	TargetStringType spec;
	spec.unicode = unicode;
	spec.length = length;
	if (!unicode) {
		spec.collation = collation;
	}
	return MakeTargetStringType(spec);
}

bool NeedsVarcharCollation(const LogicalType &type) {
	TargetStringType spec;
	return TryGetTargetStringType(type, spec) && !spec.unicode && spec.collation.empty();
}

bool IsValidCollationName(const std::string &name) {
	if (name.empty()) {
		return false;
	}
	for (const char c : name) {
		const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
		if (!ok) {
			return false;
		}
	}
	return true;
}

}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
