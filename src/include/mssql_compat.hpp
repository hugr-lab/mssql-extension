//===----------------------------------------------------------------------===//
// mssql_compat.hpp — DuckDB API shape adapters (spec 051)
//
// Lets a single source tree compile against multiple DuckDB SHAs by hiding
// header relocations and signature shape changes behind preprocessor guards.
// Detection uses __has_include on a target-SHA-only header
// (duckdb/common/vector/flat_vector.hpp); features that landed together in
// DuckDB main share that sentinel.
//
// Migrations covered (spec 051):
//   M2 — FlatVector header moved to <duckdb/common/vector/flat_vector.hpp>.
//   M4 — bind_scalar_function_t signature changed to single-arg
//        (BindScalarFunctionInput &). The MSSQL_BIND_SCALAR_SIG macro hides
//        the per-SHA signature; MSSQL_BIND_SCALAR_PROLOGUE unpacks the new
//        struct on the new SHA and is a no-op on the legacy SHA.
//
// M1 (ClientContext::IsInterrupted) and M3 (ScalarFunction::SetNullHandling)
// exist on both currently-supported SHAs and are renamed in-place at call
// sites, not via this header.
//
// Spec 051 follow-up (M5–M7), added after a downstream DuckLake build against
// SHA 997c2427 surfaced three more shape changes:
//   M5 — FlatVector::GetData<T>(Vector&) now returns const T*; write sites
//        must call FlatVector::GetDataMutable<T>(Vector&). Use the
//        mssql_compat::GetDataMutable<T> helper, which routes to
//        GetDataMutable on the new SHA and GetData on the legacy SHA (where
//        GetData<T>(Vector&) still returns T*).
//   M6 — StringVector moved out of <duckdb/common/types/vector.hpp> into a
//        dedicated <duckdb/common/vector/string_vector.hpp>. Including this
//        header conditionally below picks it up on the new SHA; the legacy
//        SHA continues to expose StringVector transitively via vector.hpp.
//   M7 — Vector::ToUnifiedFormat(idx_t, UnifiedVectorFormat&) is deprecated
//        in favour of the single-arg form. Use mssql_compat::ToUnifiedFormat.
//   M8 — ExpressionExecutor only forward-declared from <function.hpp> on the
//        new SHA. Callers of ExpressionExecutor::EvaluateScalar must include
//        <duckdb/execution/expression_executor.hpp> directly. Header exists
//        on both SHAs so the include is unconditional at the call site.
//   M9 — ScalarFunction::varargs moved from a public field on ScalarFunction
//        to a private FunctionSignature member with a SetVarArgs setter.
//        Guarded inline at the single call site with
//        MSSQL_DUCKDB_HAS_NEW_BIND_INPUT.
//   M10 — TableFilter API rewrite. Old class names (ConstantFilter, InFilter,
//        ConjunctionAndFilter, ConjunctionOrFilter) are now `Legacy*`-prefixed
//        on the new SHA; TableFilterType enum values are `LEGACY_*`-prefixed.
//        TableFilterSet moved to <duckdb/planner/table_filter_set.hpp>.
//        BoundBetweenExpression became a static-helper struct over a
//        BoundFunctionExpression (function name "__between"). Use
//        mssql_compat::{ConstantFilter,InFilter,ConjunctionAndFilter,
//        ConjunctionOrFilter} typedefs and mssql_compat::TFT::* enum aliases.
//        BETWEEN dispatch is gated with MSSQL_DUCKDB_HAS_NEW_BIND_INPUT.
//===----------------------------------------------------------------------===//

#pragma once

#if __has_include(<duckdb/common/vector/flat_vector.hpp>)
#include <duckdb/common/vector/flat_vector.hpp>
#define MSSQL_DUCKDB_HAS_NEW_BIND_INPUT 1
#else
#include <duckdb/common/types/vector.hpp>
#endif

#if __has_include(<duckdb/common/vector/string_vector.hpp>)
#include <duckdb/common/vector/string_vector.hpp>
#endif
#if __has_include(<duckdb/common/vector/struct_vector.hpp>)
#include <duckdb/common/vector/struct_vector.hpp>
#endif

// M10 — TableFilter API rewrite. The Legacy*-prefixed classes (new SHA) and
// their un-prefixed peers (legacy SHA) live at the same header paths, so
// these includes are unconditional. table_filter_set.hpp is new-SHA-only
// (the type was inline in table_filter.hpp on the legacy SHA, brought in
// transitively via the constant/in/conjunction headers).
#include <duckdb/planner/filter/conjunction_filter.hpp>
#include <duckdb/planner/filter/constant_filter.hpp>
#include <duckdb/planner/filter/in_filter.hpp>
#include <duckdb/planner/filter/null_filter.hpp>
#if __has_include(<duckdb/planner/filter/optional_filter.hpp>)
#include <duckdb/planner/filter/optional_filter.hpp>
#endif
#if __has_include(<duckdb/planner/table_filter_set.hpp>)
#include <duckdb/planner/table_filter_set.hpp>
#endif
// M11 — BoundComparisonExpression and BoundFunctionExpression headers.
#include <duckdb/planner/expression/bound_comparison_expression.hpp>
#include <duckdb/planner/expression/bound_function_expression.hpp>

namespace duckdb {
namespace mssql_compat {

// M10 — typedefs that resolve to the Legacy*-prefixed classes on the new SHA
// and to the un-prefixed originals on the legacy SHA.
#ifdef MSSQL_DUCKDB_HAS_NEW_BIND_INPUT
using ConstantFilter = duckdb::LegacyConstantFilter;
using InFilter = duckdb::LegacyInFilter;
using ConjunctionAndFilter = duckdb::LegacyConjunctionAndFilter;
using ConjunctionOrFilter = duckdb::LegacyConjunctionOrFilter;
#else
using ConstantFilter = duckdb::ConstantFilter;
using InFilter = duckdb::InFilter;
using ConjunctionAndFilter = duckdb::ConjunctionAndFilter;
using ConjunctionOrFilter = duckdb::ConjunctionOrFilter;
#endif

// M10 — TableFilterType enum-value aliases (named after the legacy values).
struct TFT {
#ifdef MSSQL_DUCKDB_HAS_NEW_BIND_INPUT
	static constexpr auto CONSTANT_COMPARISON = duckdb::TableFilterType::LEGACY_CONSTANT_COMPARISON;
	static constexpr auto IS_NULL = duckdb::TableFilterType::LEGACY_IS_NULL;
	static constexpr auto IS_NOT_NULL = duckdb::TableFilterType::LEGACY_IS_NOT_NULL;
	static constexpr auto IN_FILTER = duckdb::TableFilterType::LEGACY_IN_FILTER;
	static constexpr auto CONJUNCTION_OR = duckdb::TableFilterType::LEGACY_CONJUNCTION_OR;
	static constexpr auto CONJUNCTION_AND = duckdb::TableFilterType::LEGACY_CONJUNCTION_AND;
	static constexpr auto OPTIONAL_FILTER = duckdb::TableFilterType::LEGACY_OPTIONAL_FILTER;
	static constexpr auto STRUCT_EXTRACT = duckdb::TableFilterType::LEGACY_STRUCT_EXTRACT;
	static constexpr auto DYNAMIC_FILTER = duckdb::TableFilterType::LEGACY_DYNAMIC_FILTER;
#else
	static constexpr auto CONSTANT_COMPARISON = duckdb::TableFilterType::CONSTANT_COMPARISON;
	static constexpr auto IS_NULL = duckdb::TableFilterType::IS_NULL;
	static constexpr auto IS_NOT_NULL = duckdb::TableFilterType::IS_NOT_NULL;
	static constexpr auto IN_FILTER = duckdb::TableFilterType::IN_FILTER;
	static constexpr auto CONJUNCTION_OR = duckdb::TableFilterType::CONJUNCTION_OR;
	static constexpr auto CONJUNCTION_AND = duckdb::TableFilterType::CONJUNCTION_AND;
	static constexpr auto OPTIONAL_FILTER = duckdb::TableFilterType::OPTIONAL_FILTER;
	static constexpr auto STRUCT_EXTRACT = duckdb::TableFilterType::STRUCT_EXTRACT;
	static constexpr auto DYNAMIC_FILTER = duckdb::TableFilterType::DYNAMIC_FILTER;
#endif
};


// M5 — write-site adapter for FlatVector data pointers. On the new SHA this
// routes to GetDataMutable<T>; on the legacy SHA the single-overload
// GetData<T>(Vector&) already returns T*, so we forward to it.
template <class T>
inline T *GetDataMutable(duckdb::Vector &vec) {
#ifdef MSSQL_DUCKDB_HAS_NEW_BIND_INPUT
	return duckdb::FlatVector::GetDataMutable<T>(vec);
#else
	return duckdb::FlatVector::GetData<T>(vec);
#endif
}

// M5b — same const-overload story for FlatVector::Validity. The new SHA has
// a `ValidityMutable` peer that returns the non-const reference suitable for
// SetAllValid / write-through; the legacy SHA's `Validity` already returns
// the non-const reference.
inline duckdb::ValidityMask &ValidityMutable(duckdb::Vector &vec) {
#ifdef MSSQL_DUCKDB_HAS_NEW_BIND_INPUT
	return duckdb::FlatVector::ValidityMutable(vec);
#else
	return duckdb::FlatVector::Validity(vec);
#endif
}

// M7 — count-less ToUnifiedFormat on the new SHA; legacy SHA keeps the count.
// We only need format info, not count, at every current call site, so we
// always pass `count` and let the new-SHA branch discard it.
inline void ToUnifiedFormat(duckdb::Vector &vec, duckdb::idx_t count, duckdb::UnifiedVectorFormat &fmt) {
#ifdef MSSQL_DUCKDB_HAS_NEW_BIND_INPUT
	(void)count;
	vec.ToUnifiedFormat(fmt);
#else
	vec.ToUnifiedFormat(count, fmt);
#endif
}

// M11 — BoundFunctionExpression.function.name moved from a public Function
// field to a protected BoundSimpleFunction field with a GetName() getter on
// the new SHA. Single forwarding helper covers both shapes.
inline const std::string &GetFunctionName(const duckdb::BoundFunctionExpression &expr) {
#ifdef MSSQL_DUCKDB_HAS_NEW_BIND_INPUT
	return expr.function.GetName();
#else
	return expr.function.name;
#endif
}

}  // namespace mssql_compat
}  // namespace duckdb

#ifdef MSSQL_DUCKDB_HAS_NEW_BIND_INPUT

#define MSSQL_BIND_SCALAR_SIG(name) \
	static duckdb::unique_ptr<duckdb::FunctionData> name(duckdb::BindScalarFunctionInput &input)

#define MSSQL_BIND_SCALAR_PROLOGUE            \
	auto &context = input.GetClientContext(); \
	auto &arguments = input.GetArguments();   \
	(void)context;                            \
	(void)arguments;

#else

#define MSSQL_BIND_SCALAR_SIG(name)                                                  \
	static duckdb::unique_ptr<duckdb::FunctionData> name(                            \
		duckdb::ClientContext &context, duckdb::ScalarFunction & /*bound_function*/, \
		duckdb::vector<duckdb::unique_ptr<duckdb::Expression>> &arguments)

#define MSSQL_BIND_SCALAR_PROLOGUE /* no-op on legacy SHA */

#endif
