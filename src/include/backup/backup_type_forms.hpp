//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_type_forms.hpp
//
// Spec 093 T8 (D9): the backup reader's use of the TDS path's type kernels. A catalog column whose type
// a kernel of the codec reads -- the CLR system types hierarchyid, geometry and geography (T4, T5) -- is
// described to the codec as the TDS path's column of that type, so the record stager stages its
// stored bytes and the same kernel decodes them. TypeForms carries what TYPE mssql takes at ATTACH and
// a backup takes at ATTACH or per call: how a geometry / geography is read. [DuckDB]
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_catalog_model.hpp"
#include "duckdb/common/types.hpp"
#include "tds/tds_column_metadata.hpp"

#include <string>

namespace duckdb {
namespace mssql {
namespace backup {

//! How a backup's columns of the kernels' types are read: the `geometry_as` of the call, else of the
//! attached catalog, else 'geometry' (TYPE mssql's default; neither has a setting).
struct TypeForms {
	tds::GeometryAs geometry_as = tds::GeometryAs::Geometry;

	bool operator==(const TypeForms &other) const {
		return geometry_as == other.geometry_as;
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

//! `geometry_as` as kept by a bind or a catalog: "" when neither the call nor the ATTACH gave one (the
//! default, 'geometry'); `source` names where in a refusal.
TypeForms TypeFormsOf(const std::string &geometry_as, const char *source);
//! A `geometry_as` the call or the ATTACH gives (not NULL): checked as TYPE mssql checks its option, so ""
//! is refused too, and returned as given.
std::string GeometryAsGiven(const std::string &value, const char *source);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
