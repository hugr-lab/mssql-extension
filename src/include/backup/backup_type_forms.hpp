//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_type_forms.hpp
//
// Spec 093 T8 (D9): the backup reader's use of the TDS path's type kernels. A catalog column whose type
// a kernel of the codec reads -- the CLR system types hierarchyid, geometry and geography (T4, T5) -- is
// described to the codec as the TDS path's column of that type, so the record stager stages its
// stored bytes and the same kernel decodes them. TypeForms carries what TYPE mssql takes at ATTACH and
// a backup takes at ATTACH or per call: how a geometry / geography is read, and (T8c) the type a
// datetime2(7) is read as. [DuckDB]
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_catalog_model.hpp"
#include "catalog/mssql_column_info.hpp"
#include "duckdb/common/named_parameter_map.hpp"
#include "duckdb/common/types.hpp"
#include "tds/tds_column_metadata.hpp"

#include <string>

namespace duckdb {
namespace mssql {
namespace backup {

//! How a backup's columns of the kernels' types are read: the `geometry_as` of the call, else of the
//! attached catalog, else 'geometry' (TYPE mssql's default; no setting); and (spec 093 T8c) the type a
//! datetime2(7) is read as: the `datetime2_7_as` of the call, else of the attached catalog, else the
//! setting mssql_datetime2_7_as, else 'timestamp_ns' -- TYPE mssql's order.
struct TypeForms {
	tds::GeometryAs geometry_as = tds::GeometryAs::Geometry;
	MSSQLDatetime2Read datetime2_read = MSSQLDatetime2Read::Nanoseconds;

	bool operator==(const TypeForms &other) const {
		return geometry_as == other.geometry_as && datetime2_read == other.datetime2_read;
	}
};

//! TypeForms' options as a bind or a catalog keeps them, each as given: "" when not given (the default).
//! Kept as text so a plan serializes them and a refusal can quote them.
struct TypeFormOptions {
	std::string geometry_as;
	std::string datetime2_7_as;

	bool operator==(const TypeFormOptions &other) const {
		return geometry_as == other.geometry_as && datetime2_7_as == other.datetime2_7_as;
	}
};

//! Whether `c` is of a CLR system type (schema sys): hierarchyid, geometry or geography. A user CLR
//! type (its own schema, no system type underneath) is not: opaque by design, its stored bytes.
bool IsClrSystemType(const CatalogColumn &c);

//! The type a CLR system type's column is read as: hierarchyid VARCHAR (its ToString() text), geometry
//! GEOMETRY, geography GEOMETRY('OGC:CRS84') -- TYPE mssql's (MapSQLServerTypeToDuckDB) -- or, under
//! geometry_as 'wkb' / 'native', a BLOB of ISO WKB / of the stored bytes. IsClrSystemType(c) holds.
LogicalType ClrSystemTypeOf(const CatalogColumn &c, const TypeForms &forms);

//! `m` made the TDS path's column of `c`'s CLR system type: a UDT of schema sys and that name, PLP, with
//! `forms.geometry_as`, so ResolveColumnOps stages its bytes on the PLP binary arm and finalizes them
//! with the HierarchyId or Spatial kernel. IsClrSystemType(c) holds.
void DescribeClrSystemType(const CatalogColumn &c, const TypeForms &forms, tds::ColumnMetadata &m);

//! Whether `c` is a SQL Server 2025 `vector` (spec 093 T8d, D9): syscolpars has xtype 165 (varbinary)
//! and utype 255, sysscalartypes' row of that utype named `vector` (measured V4). Its declaration may
//! still be one no server writes: VectorDeclaration says.
bool IsVectorColumn(const CatalogColumn &c);
//! A vector column's dimensions, its element type its `scale` (0 float32, 1 float16) and its length
//! 8 + n x element size (V4); 0, with `why`, for a declaration no vector(n) has.
uint16_t VectorDeclaration(const CatalogColumn &c, std::string &why);
//! `m` made the TDS path's column of `c`'s vector: a VECTORTYPE (0xF5) of the stored length and element
//! type, UserType 255, so ResolveColumnOps finalizes its stored bytes -- the 0xF5 wire value's, V4 --
//! with the Vector kernel. IsVectorColumn(c) holds and VectorDeclaration(c) answered n.
void DescribeVector(const CatalogColumn &c, tds::ColumnMetadata &m);

//! `m` made the TDS path's column of a sql_variant read from a record (spec 093 T8b): the Variant kernel's,
//! its values in the form a record stores them (`variant_disk_form`: SplitDiskVariant), read as VARIANT
//! as TYPE mssql reads them (T1).
void DescribeSqlVariant(tds::ColumnMetadata &m);

//! The forms of `options` as kept by a bind or a catalog (TypeFormOptions: "" = the default); `where`
//! (a function, "ATTACH x (TYPE mssql_backup)") names the option in a refusal.
TypeForms TypeFormsOf(const TypeFormOptions &options, const std::string &where);
//! A `geometry_as` the call or the ATTACH gives (not NULL): checked as TYPE mssql checks its option, so ""
//! is refused too, and returned as given.
std::string GeometryAsGiven(const std::string &value, const char *source);
//! A `datetime2_7_as` the call or the ATTACH gives (not NULL): checked by ParseDatetime2Read ('timestamp_ns'
//! or 'timestamp', any case, trimmed), so "" is refused, as the backup's geometry_as refuses it (TYPE mssql's
//! ATTACH takes '' as unset), and returned as given.
std::string Datetime2AsGiven(const std::string &value, const char *source);
//! The setting mssql_datetime2_7_as as `context` has it, "" when unset: what a datetime2_7_as neither the
//! call nor an ATTACH gives falls back to, read once by the bind or the ATTACH (and so serialized).
std::string Datetime2AsSetting(ClientContext &context);
//! The `geometry_as` and `datetime2_7_as` ATTACH options of the backup attached as `name` (spec 093 T8 /
//! T8c; the latter with the setting folded in at its ATTACH), {} when `name` is none. [the catalog layer]
TypeFormOptions AttachedTypeFormOptions(ClientContext &context, const std::string &name);
//! A file function's options, each checked: the call's (`named`, a value not NULL), else the attached
//! backup's when `paths` is one attached name, else for datetime2_7_as the setting -- TYPE mssql's order.
//! `function` names the option in a refusal.
TypeFormOptions TypeFormOptionsOf(ClientContext &context, const named_argument_map_t &named,
								  const vector<std::string> &paths, const std::string &function);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
