#include "backup/backup_sql_variant.hpp"

#include "backup/backup_bytes.hpp"

namespace duckdb {
namespace mssql {
namespace backup {

namespace {

enum class Props { None, Scale, PrecisionScale, MaxLength, MaxLengthCollation, Unknown };

Props PropsOf(uint8_t xtype) {
	switch (xtype) {
	case 36:   // uniqueidentifier
	case 40:   // date
	case 48:   // tinyint
	case 52:   // smallint
	case 56:   // int
	case 58:   // smalldatetime
	case 59:   // real
	case 60:   // money
	case 61:   // datetime
	case 62:   // float
	case 104:  // bit
	case 122:  // smallmoney
	case 127:  // bigint
		return Props::None;
	case 41:  // time
	case 42:  // datetime2
	case 43:  // datetimeoffset
		return Props::Scale;
	case 106:  // decimal
	case 108:  // numeric
		return Props::PrecisionScale;
	case 165:  // varbinary
	case 173:  // binary
		return Props::MaxLength;
	case 167:  // varchar
	case 175:  // char
	case 231:  // nvarchar
	case 239:  // nchar
		return Props::MaxLengthCollation;
	default:
		return Props::Unknown;
	}
}

}  // namespace

SqlVariant ParseSqlVariant(const std::string &bytes, const std::string &what) {
	ByteReader r(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(), what.c_str());
	SqlVariant v;
	v.xtype = r.U8(0);
	const uint8_t version = r.U8(1);
	if (version != 1) {
		throw FormatError("backup: " + what + " is a sql_variant of version " + std::to_string(version) +
						  ", which the reader does not know");
	}
	size_t at = 2;
	switch (PropsOf(v.xtype)) {
	case Props::None:
		break;
	case Props::Scale:
		v.scale = r.U8(at);
		at += 1;
		break;
	case Props::PrecisionScale:
		v.precision = r.U8(at);
		v.scale = r.U8(at + 1);
		at += 2;
		break;
	case Props::MaxLength:
		v.max_length = r.U16(at);
		at += 2;
		break;
	case Props::MaxLengthCollation:
		v.max_length = r.U16(at);
		v.collation = r.U32(at + 2);
		at += 6;
		break;
	case Props::Unknown:
		throw FormatError("backup: " + what + " is a sql_variant of xtype " + std::to_string(v.xtype) +
						  ", whose properties the reader does not know");
	}
	v.value.assign(reinterpret_cast<const char *>(r.Bytes(at, bytes.size() - at)), bytes.size() - at);
	return v;
}

int64_t SqlVariantInt(const SqlVariant &v, const std::string &what) {
	ByteReader r(reinterpret_cast<const uint8_t *>(v.value.data()), v.value.size(), what.c_str());
	size_t want = 0;
	switch (v.xtype) {
	case 48:
	case 104:
		want = 1;
		break;
	case 52:
		want = 2;
		break;
	case 56:
		want = 4;
		break;
	case 127:
		want = 8;
		break;
	default:
		throw FormatError("backup: " + what + " is a sql_variant of xtype " + std::to_string(v.xtype) +
						  ", not an integer");
	}
	if (v.value.size() != want) {
		throw FormatError("backup: " + what + " holds " + std::to_string(v.value.size()) + " bytes of xtype " +
						  std::to_string(v.xtype) + ", which takes " + std::to_string(want));
	}
	switch (want) {
	case 1:
		return r.U8(0);
	case 2:
		return r.I16(0);
	case 4:
		return r.I32(0);
	default:
		return r.I64(0);
	}
}

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
