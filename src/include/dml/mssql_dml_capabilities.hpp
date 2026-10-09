#pragma once

// Spec 080 D0: which DML forms the server behind a catalog can take.
//
// Self-contained (std only), like catalog/mssql_rowid_key_choice.hpp, so the
// table below is unit-tested with no server and no linking
// (test/cpp/test_dml_capabilities.cpp, `make test-dml-capabilities`).
//
// The platform comes from the host test (IsFabricEndpoint / IsSynapseEndpoint),
// not from EngineEdition: Fabric Warehouse is commonly reported with
// EngineEdition 11, the value Synapse serverless reports too. The one edition
// that does decide is 6, Synapse dedicated only: a dedicated pool reached under
// `*.database.windows.net` (a former SQL DW) or a private-link alias misses the
// host test, and its keys are NOT ENFORCED. Otherwise EngineEdition and
// ProductMajorVersion decide whether the server has IS NOT DISTINCT FROM.

#include <cstdint>

namespace duckdb {
namespace mssql {

enum class DmlPlatform : uint8_t {
	SqlServer,	//!< SQL Server 2019+, Azure SQL Database, Managed Instance
	Fabric,		//!< Fabric Warehouse
	Synapse		//!< Synapse dedicated or serverless (not told apart)
};

struct DmlCapabilities {
	DmlPlatform platform = DmlPlatform::SqlServer;
	//! `UPDATE … OUTPUT … INTO @t` (RETURNING on the staged / VALUES path).
	bool output_into_tvar = false;
	//! `UPDATE … FROM … JOIN`, `DELETE … FROM … JOIN`. Fabric documents neither.
	bool update_from_join = false;
	//! T-SQL MERGE.
	bool merge = false;
	//! `INSERT BULK` into a session `#stage`.
	bool stage_bulk = false;
	//! `a IS NOT DISTINCT FROM b`; otherwise rung 3 compares through
	//! `EXISTS (SELECT t.c… INTERSECT SELECT s.c…)`, which is correct everywhere.
	bool null_safe_operator = false;
	//! Whether keyless (rung 3) UPDATE / DELETE may run. Fabric: until
	//! fabric-probe/p18_merge_on_intersect has passed on a live warehouse.
	bool keyless_dml = false;

	//! Synapse keeps today's path: no stage, no pushdown, and (since its keys are
	//! NOT ENFORCED) no keyed UPDATE / DELETE either.
	bool IsSynapse() const {
		return platform == DmlPlatform::Synapse;
	}

	//! engine_edition / product_major_version: SERVERPROPERTY values, or -1 when
	//! unreadable. An unknown value takes the INTERSECT form: correct everywhere,
	//! only slower on 2022+.
	static DmlCapabilities Resolve(DmlPlatform platform, int32_t engine_edition, int32_t product_major_version) {
		if (platform == DmlPlatform::SqlServer && engine_edition == 6) {
			platform = DmlPlatform::Synapse;
		}
		DmlCapabilities caps;
		caps.platform = platform;
		switch (platform) {
		case DmlPlatform::SqlServer:
			caps.output_into_tvar = true;
			caps.update_from_join = true;
			caps.merge = true;
			caps.stage_bulk = true;
			caps.keyless_dml = true;
			// Azure SQL Database (5) and Managed Instance (8) report version 12
			// while having the operator.
			caps.null_safe_operator = product_major_version >= 16 || engine_edition == 5 || engine_edition == 8;
			break;
		case DmlPlatform::Fabric:
			// The "until the probe" rows of D0 stay off until fabric-probe/
			// settles them; each flips with one line and a [fabric] test.
			caps.merge = true;
			break;
		case DmlPlatform::Synapse:
			break;
		}
		return caps;
	}
};

}  // namespace mssql
}  // namespace duckdb
