#include "codec/target_string_type.hpp"

#include "duckdb/common/extension_type_info.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {
namespace mssql {
namespace codec {

//! The alias IS the type identity: one integer modifier cannot say whether it
//! means varchar or nvarchar. The alias is also not optional for a different
//! reason — an anonymous extension type crashes DuckDB's binder (spec 060 § 2a).
static constexpr const char *NVARCHAR_ALIAS = "MSSQL_NVARCHAR";
static constexpr const char *VARCHAR_ALIAS = "MSSQL_VARCHAR";

//! A collation travels inside the length modifier, as "<length>,<collation>", with a
//! label that prints the length alone. In the modifier, because a type is respelled from
//! its modifiers when DuckDB's remote-pushdown rewriter folds a constant
//! (TypeExpression::FromLogicalType), and a property would be lost there: `'x'::
//! MSSQL_VARCHAR(4, 'Cyrillic_General_CI_AS')` came back without its collation. Printing
//! the length alone, because with the catalog reporting these types for every string
//! column `DESCRIBE` would otherwise print the collation of every varchar in the database.
static constexpr char COLLATION_SEPARATOR = ',';

//! Where v0.3.0 kept the collation. Only read: a DuckDB file it wrote holds such types, a
//! catalog varchar copied into a local table among them.
static constexpr const char *V030_COLLATION_PROPERTY = "collation";

void SplitLengthModifier(const std::string &text, std::string &length, std::string &collation) {
	const auto separator = text.find(COLLATION_SEPARATOR);
	length = text.substr(0, separator);
	collation = separator == std::string::npos ? std::string() : text.substr(separator + 1);
}

LogicalType MakeTargetStringType(const TargetStringType &spec) {
	LogicalType type = LogicalType(LogicalTypeId::VARCHAR).WithAlias(spec.unicode ? NVARCHAR_ALIAS : VARCHAR_ALIAS);

	auto info = make_uniq<ExtensionTypeInfo>();
	// The modifier is what DESCRIBE and duckdb_columns() PRINT, so the MAX form
	// carries the word rather than the sentinel. DuckDB renders it unquoted, so
	// the column reads as `MSSQL_VARCHAR(MAX)` -- T-SQL's own spelling -- where
	// the sentinel would have printed `MSSQL_VARCHAR(0)`, which says zero-length,
	// or `(-1)`, which says nothing at all. The internal `length` stays numeric;
	// this is the one boundary where it becomes something a person reads.
	//
	// The printed form is deliberately NOT round-trippable: unquoted MAX in a
	// cast is refused by DuckDB's parser, before this extension is consulted.
	// Documented in website/docs/writing/table-options.md, since the obvious
	// thing to do with a type name is paste it back.
	const std::string length = IsMaxLength(spec.length) ? std::string(MAX_MODIFIER) : std::to_string(spec.length);
	if (!spec.unicode && !spec.collation.empty()) {
		info->modifiers.emplace_back(Value(length + COLLATION_SEPARATOR + spec.collation));
		info->modifiers.back().label = length;
	} else if (IsMaxLength(spec.length)) {
		info->modifiers.emplace_back(Value(MAX_MODIFIER));
	} else {
		info->modifiers.emplace_back(Value::INTEGER(spec.length));
	}
	return type.WithExtensionInfo(std::move(info));
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
	const auto &length_modifier = info.modifiers[0].value;
	if (length_modifier.type().id() == LogicalTypeId::VARCHAR) {
		// The MAX form, a named collation, or both. Nothing else is stored as a string,
		// and a stray one would be a type this codec did not build.
		std::string length;
		SplitLengthModifier(length_modifier.ToString(), length, result.collation);
		if (IsMaxKeyword(length)) {
			result.length = MAX_LENGTH;
		} else {
			int32_t parsed;
			if (!TryCast::Operation<string_t, int32_t>(string_t(length), parsed)) {
				return false;
			}
			result.length = parsed;
		}
	} else {
		result.collation.clear();
		result.length = length_modifier.GetValue<int32_t>();
	}
	if (result.collation.empty()) {
		const auto entry = info.properties.find(V030_COLLATION_PROPERTY);
		if (entry != info.properties.end()) {
			result.collation = entry->second.ToString();
		}
	}
	return true;
}

std::string FormatTargetStringDdl(const TargetStringType &spec, const std::string &fallback_collation) {
	// `max` is a keyword in T-SQL's length position, not a number (issue #321).
	const std::string length = IsMaxLength(spec.length) ? "max" : StringUtil::Format("%d", spec.length);

	if (spec.unicode) {
		return "nvarchar(" + length + ")";
	}

	std::string ddl = "varchar(" + length + ")";
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

LogicalType FabricStringType(const LogicalType &type) {
	TargetStringType spec;
	if (!TryGetTargetStringType(type, spec)) {
		return type;
	}
	const std::string upper = StringUtil::Upper(spec.collation);
	const bool utf8 = StringUtil::EndsWith(upper, "_UTF8");
	if (!spec.unicode && (spec.collation.empty() || upper == FABRIC_BIN2_COLLATION || upper == FABRIC_CI_COLLATION)) {
		return type;
	}
	// A UTF-8 varchar's n is bytes already; n of anything else can be 3n of them.
	if ((spec.unicode || !utf8) && !IsMaxLength(spec.length)) {
		const int64_t utf8_bytes = static_cast<int64_t>(spec.length) * 3;
		spec.length = utf8_bytes > MAX_VARCHAR_LENGTH ? MAX_LENGTH : static_cast<int32_t>(utf8_bytes);
	}
	spec.unicode = false;
	spec.collation.clear();
	return MakeTargetStringType(spec);
}

bool IsMaxKeyword(const std::string &text) {
	return StringUtil::CIEquals(text, MAX_MODIFIER);
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
