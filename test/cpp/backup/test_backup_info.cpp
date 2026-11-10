// Spec 085 task 9: mssql_backup_info through SQL, in an in-process DuckDB with the extension loaded,
// over synthetic backups (mtf_builder) written to files.
//
// The builder's media set has two families: one file alone is refused naming the missing
// FamilySequenceNumber; both, given in either order, are one row whose header columns are the
// builder's (type, LSNs in HEADERONLY's DECIMAL form, dates, database and server names, the backup
// set GUID, the media set, the recovery model, the files as a list), with the later stages' columns
// NULL. position := 2 and a NULL path are refused by name. A prepared statement keeps the row its
// PREPARE read after the files are rewritten; with reread := true it reads them again.

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include "duckdb.hpp"
#include "mssql_extension.hpp"
#include "mtf_builder.hpp"

using namespace duckdb;

namespace {

int failures = 0;

void Check(bool ok, const std::string &what) {
	std::cout << (ok ? "  ok   " : "  FAIL ") << what << std::endl;
	if (!ok) {
		failures++;
	}
}

void WriteFile(const std::string &path, const mtf_builder::Bytes &bytes) {
	std::ofstream out(path, std::ios::binary);
	out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::string ErrorOf(Connection &con, const std::string &sql) {
	auto result = con.Query(sql);
	return result->HasError() ? result->GetError() : std::string();
}

}  // namespace

int main() {
	std::cout << "mssql_backup_info (spec 085 task 9)" << std::endl;
	const char *tmp = std::getenv("TMPDIR");
	std::string dir = tmp ? tmp : "/tmp";
	while (dir.size() > 1 && dir.back() == '/') {
		dir.pop_back();
	}
	dir += "/mssql_backup_info_test";
	if (std::system(("mkdir -p '" + dir + "'").c_str()) != 0) {
		std::cout << "cannot create " << dir << std::endl;
		return 1;
	}
	mtf_builder::Options one;
	mtf_builder::Options two;
	two.family_sequence = 2;
	WriteFile(dir + "/set_1.bak", mtf_builder::BuildImage(one));
	WriteFile(dir + "/set_2.bak", mtf_builder::BuildImage(two));

	DuckDB db(nullptr);
	db.LoadStaticExtension<MssqlExtension>();
	Connection con(db);

	const auto alone = ErrorOf(con, "SELECT * FROM mssql_backup_info('" + dir + "/set_1.bak')");
	Check(alone.find("FamilySequenceNumber 2 are missing") != std::string::npos,
		  "one family of two alone is refused, naming the missing one");

	// The list as a parameter: an in-process DuckDB without core_functions has no list literal.
	auto prepared = con.Prepare(
		"SELECT backup_type, first_lsn::VARCHAR, database_name, server_name, backup_set_guid::VARCHAR, "
		"family_count, family_sequence_number, recovery_model, backup_start_date::VARCHAR, "
		"len(files), files[1].logical_name, files[1].type, database_version, page_count, consistent "
		"FROM mssql_backup_info($1)");
	vector<Value> paths{Value(dir + "/set_2.bak"), Value(dir + "/set_1.bak")};
	auto row = prepared->Execute(Value::LIST(LogicalType::VARCHAR, paths));
	unique_ptr<DataChunk> chunk;
	if (!row->HasError()) {
		chunk = row->Fetch();
	}
	const bool one_row = !row->HasError() && chunk && chunk->size() == 1 && !row->Fetch();
	Check(one_row,
		  "both families, in either order: one row" + (row->HasError() ? ": " + row->GetError() : std::string()));
	if (one_row) {
		Check(chunk->GetValue(0, 0).ToString() == "full" && chunk->GetValue(1, 0).ToString() == "47000000040000001",
			  "the backup type and an LSN in HEADERONLY's form");
		Check(chunk->GetValue(2, 0).ToString() == "salesdb" && chunk->GetValue(3, 0).ToString() == "srv1",
			  "the database and server names");
		Check(chunk->GetValue(4, 0).ToString() == "23222120-2524-2726-2829-2a2b2c2d2e2f", "the backup set GUID");
		Check(chunk->GetValue(5, 0).GetValue<int32_t>() == 2 && chunk->GetValue(6, 0).IsNull(),
			  "a whole set: two families, no one sequence number");
		Check(chunk->GetValue(7, 0).ToString() == "SIMPLE" && chunk->GetValue(8, 0).ToString() == "2026-11-09 08:40:38",
			  "the recovery model and the start date");
		Check(chunk->GetValue(9, 0).GetValue<int64_t>() == 1 && chunk->GetValue(10, 0).ToString() == "db_data" &&
				  chunk->GetValue(11, 0).ToString() == "data",
			  "the files as a list of structs");
		Check(chunk->GetValue(12, 0).IsNull() && chunk->GetValue(13, 0).IsNull() && chunk->GetValue(14, 0).IsNull(),
			  "the later stages' columns are NULL");
	}
	{
		// A prepared statement keeps what its PREPARE read; reread := true reads the files again.
		auto kept = con.Prepare("SELECT backup_type FROM mssql_backup_info('" + dir + "/set_*.bak')");
		auto fresh = con.Prepare("SELECT backup_type FROM mssql_backup_info('" + dir + "/set_*.bak', reread := true)");
		auto type_of = [](unique_ptr<QueryResult> r) {
			if (r->HasError()) {
				return r->GetError();
			}
			auto c = r->Fetch();
			return c && c->size() == 1 ? c->GetValue(0, 0).ToString() : std::string("?");
		};
		const auto before_kept = type_of(kept->Execute());
		const auto before_fresh = type_of(fresh->Execute());
		mtf_builder::Options log1;
		log1.with_msda = false;
		mtf_builder::Options log2 = log1;
		log2.family_sequence = 2;
		WriteFile(dir + "/set_1.bak", mtf_builder::BuildImage(log1));
		WriteFile(dir + "/set_2.bak", mtf_builder::BuildImage(log2));
		const auto after_kept = type_of(kept->Execute());
		const auto after_fresh = type_of(fresh->Execute());
		Check(before_kept == "full" && before_fresh == "full" && after_kept == "full" && after_fresh == "log",
			  "a prepared statement keeps its row (" + after_kept + "); with reread := true it reads again (" +
				  after_fresh + ")");
	}
	Check(ErrorOf(con, "SELECT * FROM mssql_backup_info('" + dir + "/set_*.bak', position := 2)")
				  .find("not supported yet") != std::string::npos,
		  "position := 2 is refused by name");
	Check(ErrorOf(con, "SELECT * FROM mssql_backup_info(NULL::VARCHAR)").find("the path is NULL") != std::string::npos,
		  "a NULL path is refused by name");
	Check(ErrorOf(con, "SELECT * FROM mssql_backup_info('" + dir + "/none_*.bak')").find("none_") != std::string::npos,
		  "a glob that matches nothing names it");

	std::cout << (failures ? "FAILED: " + std::to_string(failures) + " check(s)" : "all checks passed") << std::endl;
	return failures ? 1 : 0;
}
