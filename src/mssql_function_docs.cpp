#include "mssql_function_docs.hpp"

#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

namespace duckdb {
namespace mssql {

static FunctionDescription Describe(const FunctionDoc &doc) {
	FunctionDescription description;
	description.description = doc.description;
	description.examples = doc.examples;
	description.categories = doc.categories;
	return description;
}

void RegisterDocumentedFunction(ExtensionLoader &loader, vector<ScalarFunction> overloads, const FunctionDoc &doc) {
	D_ASSERT(!overloads.empty());
	ScalarFunctionSet set(overloads[0].name);
	for (auto &overload : overloads) {
		// Named before the overload joins the set: a set's overloads are
		// immutable once added (shared with every function bound from them).
		auto &signature = overload.GetSignature();
		for (idx_t i = 0; i < signature.GetParameterCount() && i < doc.parameter_names.size(); i++) {
			// A parameter declared by its type alone is positional-only ("col<N>")
			// since DuckDB's function-argument unification: re-declared as a
			// standard one, so the name is callable (`mssql_exec(context := …)`).
			auto &parameter = signature.GetParameter(i);
			if (parameter.IsVariadic()) {
				continue;
			}
			optional<Value> default_value;
			if (parameter.HasDefaultValue()) {
				default_value = *parameter.GetDefaultValue();
			}
			parameter = FunctionParameter(Identifier(doc.parameter_names[i]), parameter.GetType(),
										  std::move(default_value), FunctionParameterKind::STANDARD);
		}
		set.AddFunction(std::move(overload));
	}
	CreateScalarFunctionInfo info(std::move(set));
	info.descriptions.push_back(Describe(doc));
	// What the bare RegisterFunction(ScalarFunction) overload sets; a CreateInfo
	// defaults to ERROR_ON_CONFLICT.
	info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	loader.RegisterFunction(std::move(info));
}

void RegisterDocumentedFunction(ExtensionLoader &loader, TableFunctionSet set, const FunctionDoc &doc) {
	CreateTableFunctionInfo info(std::move(set));
	info.descriptions.push_back(Describe(doc));
	info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;
	loader.RegisterFunction(std::move(info));
}

}  // namespace mssql
}  // namespace duckdb
