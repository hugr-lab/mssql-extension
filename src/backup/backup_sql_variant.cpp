#include "backup/backup_sql_variant.hpp"

#include "backup/backup_bytes.hpp"

#include <initializer_list>

namespace duckdb {
namespace mssql {
namespace backup {

namespace {

enum class Props { None, Scale, PrecisionScale, MaxLength, MaxLengthCollation, Unknown };

//! The properties each base type's value is preceded by, by xtype; any other is Unknown.
struct PropsTable {
	Props of[256];
	void Set(std::initializer_list<uint8_t> xtypes, Props props) {
		for (const uint8_t xtype : xtypes) {
			of[xtype] = props;
		}
	}
	PropsTable() {
		for (auto &p : of) {
			p = Props::Unknown;
		}
		// uniqueidentifier, date, tinyint, smallint, int, smalldatetime, real, money, datetime, float, bit,
		// smallmoney, bigint
		Set({36, 40, 48, 52, 56, 58, 59, 60, 61, 62, 104, 122, 127}, Props::None);
		Set({41, 42, 43}, Props::Scale);					   // time, datetime2, datetimeoffset
		Set({106, 108}, Props::PrecisionScale);				   // decimal, numeric
		Set({165, 173}, Props::MaxLength);					   // varbinary, binary
		Set({167, 175, 231, 239}, Props::MaxLengthCollation);  // varchar, char, nvarchar, nchar
	}
};

Props PropsOf(uint8_t xtype) {
	static const PropsTable table;
	return table.of[xtype];
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
