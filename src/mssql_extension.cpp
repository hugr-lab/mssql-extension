#include "mssql_extension.hpp"
#include "azure/azure_test_function.hpp"
#include "catalog/mssql_preload_catalog.hpp"
#include "catalog/mssql_refresh_function.hpp"
#include "codec/target_string_type.hpp"
#include "connection/mssql_diagnostic.hpp"
#include "connection/mssql_settings.hpp"
#include "copy/copy_function.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/extension_type_info.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/vector_operations/generic_executor.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "mssql_functions.hpp"
#include "mssql_secret.hpp"
#include "mssql_storage.hpp"
#include "table_scan/mssql_optimizer.hpp"
#include "tds/auth/krb5_test_function.hpp"
#include "tds/auth/winsspi_test_function.hpp"

namespace duckdb {

// Extension version string
static const char *GetMssqlExtensionVersion() {
#ifdef MSSQL_VERSION
	return MSSQL_VERSION;
#else
	return "unknown";
#endif
}

// Placeholder scalar function to verify extension loads correctly
static void MssqlVersionFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto version = GetMssqlExtensionVersion();
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<string_t>(result)[0] = StringVector::AddString(result, version);
}

// Internal function to register extension functionality
//===----------------------------------------------------------------------===//
// Spec 060 — parameterised target types
//
// MSSQL_NVARCHAR(n) and MSSQL_VARCHAR(n [, collation]) state the type a
// CREATE/CTAS/COPY target column should get. DuckDB drops the VARCHAR length
// modifier at CREATE TABLE and Parquet carries no string length, so the bound
// cannot be plumbed from a source — it has to be stated, and a cast is the only
// mechanism available on the CTAS path, which has no options syntax.
//
// Bound to a VARCHAR carrying the length in ExtensionTypeInfo, WITH an alias —
// and the alias is not optional. Without it the type looks like a plain VARCHAR
// to function binding (which is nicer: upper(), ||, = and LIKE keep working),
// but it then crashes the binder outright: `CREATE TABLE t (a MSSQL_NVARCHAR(50));
// INSERT INTO t VALUES ('x')` segfaults on a stack overflow, in plain DuckDB with
// no catalog involved. DuckDB supports extension types only in their aliased
// form. The cost is paid back by the implicit no-op casts registered below.
// Measured both ways; see spec 060 § 2a.
//
// Only these two. CHAR/NCHAR pad, and BINARY/VARBINARY already round-trip
// through varbinary(max) without loss — none of them has a problem to solve.
//===----------------------------------------------------------------------===//

static LogicalType BindMssqlStringType(BindLogicalTypeInput &input, bool unicode) {
	const char *name = unicode ? "MSSQL_NVARCHAR" : "MSSQL_VARCHAR";
	const int32_t limit = unicode ? mssql::codec::MAX_NVARCHAR_LENGTH : mssql::codec::MAX_VARCHAR_LENGTH;
	const idx_t max_modifiers = unicode ? 1 : 2;

	auto &modifiers = input.modifiers;
	if (modifiers.empty() || modifiers.size() > max_modifiers) {
		throw BinderException(unicode ? "MSSQL_NVARCHAR takes one modifier: MSSQL_NVARCHAR(n)"
									  : "MSSQL_VARCHAR takes one or two modifiers: "
										"MSSQL_VARCHAR(n) or MSSQL_VARCHAR(n, 'collation')");
	}

	mssql::codec::TargetStringType spec;
	spec.unicode = unicode;

	auto &length_val = modifiers[0].GetValue();
	if (length_val.IsNull() || !length_val.type().IsIntegral()) {
		throw BinderException("%s(n): n must be a non-NULL integer", name);
	}
	spec.length = length_val.DefaultCastAs(LogicalType::INTEGER).GetValue<int32_t>();
	if (spec.length < 1 || spec.length > limit) {
		// n is SQL Server's own unit for the type named: UTF-16 code units for
		// nvarchar, BYTES for varchar. Past the limit SQL Server requires MAX,
		// which is what a plain VARCHAR already asks for.
		throw BinderException("%s(n): n must be between 1 and %d (use VARCHAR for MAX)", name, limit);
	}

	if (modifiers.size() == 2) {
		auto &collation_val = modifiers[1].GetValue();
		if (collation_val.IsNull() || collation_val.type().id() != LogicalTypeId::VARCHAR) {
			throw BinderException("MSSQL_VARCHAR(n, collation): collation must be a non-NULL string");
		}
		spec.collation = collation_val.ToString();
		if (!mssql::codec::IsValidCollationName(spec.collation)) {
			throw BinderException("Invalid collation name '%s': expected letters, digits and underscores",
								  spec.collation);
		}
		// A code-page collation is allowed here on purpose. Issue #225's trap was
		// the ABSENCE of a choice — mssql_ctas_text_type='VARCHAR' produced a
		// column in whatever code page the database happened to use, which the
		// user never picked and could not see. A collation spelled out by name is
		// the opposite of that, it is what SQL Server itself accepts, and
		// mssql_utf8_collation already takes any name globally. Refusing it here
		// would only mean the extension cannot reproduce a schema that exists.
		// The consequence — characters outside the code page become '?' on insert
		// — is the user's to accept, and is documented where the argument is.
	}

	return mssql::codec::MakeTargetStringType(spec);
}

static LogicalType BindMssqlNVarchar(BindLogicalTypeInput &input) {
	return BindMssqlStringType(input, true);
}

static LogicalType BindMssqlVarchar(BindLogicalTypeInput &input) {
	return BindMssqlStringType(input, false);
}

static void LoadInternal(ExtensionLoader &loader) {
	// 1. Register secrets
	RegisterMSSQLSecretType(loader);

	// 2. Register storage extension (ATTACH TYPE mssql)
	RegisterMSSQLStorageExtension(loader);

	// 3. Register table functions
	RegisterMSSQLFunctions(loader);

	// 4. Register mssql_exec scalar function
	RegisterMSSQLExecFunction(loader);

	// 5. Register connection pool settings
	RegisterMSSQLSettings(loader);

	// 6. Register diagnostic functions (mssql_open, mssql_close, mssql_ping, mssql_pool_stats)
	RegisterMSSQLDiagnosticFunctions(loader);

	// 7. Register mssql_refresh_cache function
	RegisterMSSQLRefreshCacheFunction(loader);

	// 8. Register mssql_preload_catalog function
	RegisterMSSQLPreloadCatalogFunction(loader);

	// 9. Register COPY functions (bcp format)
	RegisterMSSQLCopyFunctions(loader);

	// Spec 060: parameterised target types (see the note above).
	//
	// The alias is required (it is what keeps the binder from recursing), but on
	// its own it makes the type opaque to every VARCHAR function. Both directions
	// are registered as implicit no-op casts at cost 0: the values ARE varchars,
	// so overload resolution reaches upper(), ||, LIKE and the rest without the
	// user writing a cast back. With the catalog reporting these types, that path
	// carries every query against every attached table, not just cast columns.
	//
	// ReinterpretCast, NOT NopCast: NopCast is `result.Reference(source)`, and
	// Vector::Reference requires the two types to be FULLY equal — it compares
	// the alias and the extension info, not just the LogicalTypeId. VARCHAR and
	// MSSQL_NVARCHAR(64) share an id, so the release build silently reinterprets
	// while the debug build trips `D_ASSERT(other.GetType() == GetType())`. That
	// is what the concurrency stress caught: constant folding in the expression
	// rewriter folds the literal in `INSERT ... VALUES ('x')` through this cast,
	// so every writer aborted on iteration 0 in the assert-enabled build.
	// Reinterpret is the operation actually wanted here — same physical layout,
	// different logical type — and it permits exactly that.
	{
		auto nvarchar_type = LogicalType(LogicalTypeId::VARCHAR).WithAlias("MSSQL_NVARCHAR");
		loader.RegisterType("MSSQL_NVARCHAR", nvarchar_type, BindMssqlNVarchar);
		loader.RegisterCastFunction(nvarchar_type, LogicalType::VARCHAR, BoundCastInfo(DefaultCasts::ReinterpretCast),
									0);
		loader.RegisterCastFunction(LogicalType::VARCHAR, nvarchar_type, BoundCastInfo(DefaultCasts::ReinterpretCast),
									0);

		auto varchar_type = LogicalType(LogicalTypeId::VARCHAR).WithAlias("MSSQL_VARCHAR");
		loader.RegisterType("MSSQL_VARCHAR", varchar_type, BindMssqlVarchar);
		loader.RegisterCastFunction(varchar_type, LogicalType::VARCHAR, BoundCastInfo(DefaultCasts::ReinterpretCast),
									0);
		loader.RegisterCastFunction(LogicalType::VARCHAR, varchar_type, BoundCastInfo(DefaultCasts::ReinterpretCast),
									0);
	}

	// 10. Register utility functions (mssql_version)
	auto mssql_version_func = ScalarFunction("mssql_version", {},  // No arguments
											 LogicalType::VARCHAR, MssqlVersionFunction);
	loader.RegisterFunction(mssql_version_func);

	// 11. Register Azure authentication test function
	mssql::azure::RegisterAzureTestFunction(loader);

	// 12. Register Kerberos authentication test function (spec 042).
	//     Always registered; on builds without MSSQL_ENABLE_KRB5 it returns a
	//     clear "compiled without Kerberos support" message instead of being absent.
	mssql::krb5::RegisterKrb5TestFunction(loader);
	mssql::winsspi::RegisterWinSspiTestFunction(loader);

	// 13. Register optimizer extension for ORDER BY pushdown (Spec 039)
	auto &db = loader.GetDatabaseInstance();
	auto &config = DBConfig::GetConfig(db);
	OptimizerExtension mssql_optimizer;
	mssql_optimizer.optimize_function = MSSQLOptimizer::Optimize;
	OptimizerExtension::Register(config, std::move(mssql_optimizer));
}

// Extension class methods
void MssqlExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string MssqlExtension::Name() {
	return "mssql";
}

std::string MssqlExtension::Version() const {
	return GetMssqlExtensionVersion();
}

}  // namespace duckdb

extern "C" {

// Use the new DUCKDB_CPP_EXTENSION_ENTRY macro for loadable extension entry point
DUCKDB_CPP_EXTENSION_ENTRY(mssql, loader) {
	duckdb::LoadInternal(loader);
}
}
