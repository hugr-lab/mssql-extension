//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// codec/variant/variant_envelope.hpp
//
// Spec 093 D3 / T1: a SQL Server `sql_variant` value read as a DuckDB `VARIANT`. A value in a TDS row
// (SSVARIANT, 0x62) is a u32 length (0 = NULL), then BASETYPE, PROPBYTES, the base type's properties
// and the value in its TDS form (measured: specs/093-type-kernels/measured/S1.md). This module splits
// that envelope and writes the value as a DuckDB VARIANT primitive -- a tag and the payload bytes the
// VARIANT's `data` blob holds for it (duckdb/common/types/variant/variant_builder.hpp) -- by D3's
// table:
//
//   bit                         BOOL_TRUE / BOOL_FALSE
//   tinyint smallint int bigint UINT8 INT16 INT32 INT64
//   real float                  FLOAT DOUBLE
//   decimal(p,s) numeric(p,s)   DECIMAL(p,s)            (a 16-byte mantissa on the wire, any p)
//   money smallmoney            DECIMAL(19,4) DECIMAL(10,4)
//   date                        DATE
//   time(0-6) / time(7)         TIME_MICROS / TIME_NANOS
//   datetime2(0) (1-3) (4-6)    TIMESTAMP_SEC / _MILIS / _MICROS
//   datetime2(7)                TIMESTAMP_NANOS inside 1677-2262, else TIMESTAMP_MICROS (DQ-44)
//   datetime smalldatetime      TIMESTAMP_MICROS
//   datetimeoffset(0-6) / (7)   TIMESTAMP_MICROS_TZ / TIMESTAMP_NANOS_TZ inside 1677-2262 (the UTC
//                               instant; the offset is lost, as the column map loses it)
//   uniqueidentifier            UUID
//   binary varbinary            BLOB (as stored, padding included)
//   char varchar                VARCHAR, transcoded by the value's own collation (code page or
//                               UTF-8); a collation with no table, or UTF-8 bytes that are not
//                               UTF-8, is the value's bytes as a BLOB
//   nchar nvarchar              VARCHAR (UTF-16, an unpaired surrogate as U+FFFD)
//
// char(n) and nchar(n) arrive padded to n and are trimmed of trailing spaces, as the column codec
// trims them (#405). Every length is checked against the envelope before a byte is read; a malformed
// value is a `false` with the reason. DuckDB-free (spec 093 D1): the fuzz harness links it alone.
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace duckdb {
namespace mssql {
namespace codec {
namespace variant {

//! DuckDB's VariantLogicalType values (duckdb/common/types/variant.hpp), checked against it where the
//! vector is written (static_assert): the tag a payload is written under.
enum class VariantTag : uint8_t {
	VARIANT_NULL = 0,
	BOOL_TRUE = 1,
	BOOL_FALSE = 2,
	INT16 = 4,
	INT32 = 5,
	INT64 = 6,
	UINT8 = 8,
	FLOAT = 13,
	DOUBLE = 14,
	DECIMAL = 15,
	VARCHAR = 16,
	BLOB = 17,
	UUID = 18,
	DATE = 19,
	TIME_MICROS = 20,
	TIME_NANOS = 21,
	TIMESTAMP_SEC = 22,
	TIMESTAMP_MILIS = 23,
	TIMESTAMP_MICROS = 24,
	TIMESTAMP_NANOS = 25,
	TIMESTAMP_MICROS_TZ = 27,
	TIMESTAMP_NANOS_TZ = 34,
};

//! One sql_variant value, split: its base type (a TDS type code), properties and value bytes. The
//! pointers are into the bytes it was split from.
struct WireVariant {
	uint8_t base_type = 0;
	const uint8_t *properties = nullptr;
	uint8_t property_bytes = 0;
	const uint8_t *value = nullptr;
	size_t value_bytes = 0;
};

//! Splits a TDS SSVARIANT value's `size` bytes (after its u32 length; `size` > 0): the base type, its
//! properties and its value, every length checked against the base type and the bytes. False with
//! `why` for a base type sql_variant cannot hold or a shape it does not have.
bool SplitTdsVariant(const uint8_t *data, size_t size, WireVariant &out, std::string &why);

//! Where SplitDiskVariant puts the parts of a disk value whose TDS form differs (spec 093 T8b,
//! measured/S1d.md): a string's properties and the value of a money, datetime or smalldatetime. One per
//! chunk of a column (the kernel's), reused value after value: the last CollationId's wire collation is kept.
struct DiskVariant {
	uint8_t properties[7] = {};
	uint8_t value[8] = {};
	int64_t collation_id = -1;	// the CollationId `wire_collation` is of; -1 none yet
	uint8_t wire_collation[5] = {};
};

//! Splits a sql_variant value as a record stores it (a backup's; `size` > 0): u8 base type (an xtype), u8
//! version (1), the properties, the value. `out` is what SplitTdsVariant gives for the same value -- the
//! TDS base type (bit 0x32, not its xtype 0x68), the TDS properties (a string's 5-byte collation from its
//! CollationId, then its maximum length) and the TDS value bytes (money's halves, datetime's and
//! smalldatetime's parts in the TDS order) -- its pointers into `data` or `scratch`, every length checked
//! as SplitTdsVariant checks it, so EncodeVariant reads either alike. A CollationId the generated tables
//! do not have gives a collation of zeros: a char / varchar value is then its bytes (EncodeNote's
//! unknown_collation). False with `why` for another version, a base type sql_variant cannot hold or a
//! shape it does not have.
bool SplitDiskVariant(const uint8_t *data, size_t size, WireVariant &out, DiskVariant &scratch, std::string &why);

//! What EncodeVariant reports beside the payload.
struct EncodeNote {
	//! A char / varchar value whose collation names no code page this build has: written as its
	//! bytes, a BLOB (spec 093 D3), for the caller's WARNING.
	bool unknown_collation = false;
	//! A char / varchar value under a UTF-8 collation whose bytes are not UTF-8: written as its
	//! bytes, a BLOB, for the caller's WARNING (a VARIANT's VARCHAR is UTF-8 by contract).
	bool invalid_utf8 = false;
};

//! Appends `v`'s VARIANT payload to `blob` and sets `tag`. False with `why` for a value its base type
//! cannot be (a decimal past its precision, a fixed type of the wrong width, an odd-length UTF-16).
bool EncodeVariant(const WireVariant &v, std::string &blob, VariantTag &tag, EncodeNote &note, std::string &why);

}  // namespace variant
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
