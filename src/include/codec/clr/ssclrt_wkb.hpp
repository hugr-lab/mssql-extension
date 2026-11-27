//===----------------------------------------------------------------------===//
// codec/clr/ssclrt_wkb.hpp
//
// Spec 093 T5 (D6): a SQL Server `geometry` / `geography` value -- the MS-SSCLRT 2.1 form the server
// stores and sends (UDT 0xF0, PLP; G1, G2) -- as little-endian ISO WKB, the bytes DuckDB's GEOMETRY holds.
// DuckDB-free (D1), so the fuzz harness links it alone.
//
// The value is a header (u32 SRID, u8 version 1 or 2, u8 properties: Z 0x01, M 0x02, V 0x04, P 0x08 a
// single point, L 0x10 a single line of two points, H 0x20), then either the one point or two points (P,
// L) or: a point count and the points as (x, y) pairs; every Z, then every M; the figures (u8 attribute,
// u32 first point); the shapes (i32 parent, i32 first figure or -1 for EMPTY, u8 OGC type); and in
// version 2 the segments of the compound curves (u32 count, a byte each). The mapping is G1's prototype,
// which gave AsBinaryZM() byte for byte for 65 values on SQL Server 2019, 2022 and 2025:
//   - a geography point is stored latitude first and written longitude first (G3);
//   - a missing Z / M is stored as the NaN 0xFFF8..., written as the canonical 0x7FF8...;
//   - Z / M take the ISO codes, the type + 1000 (Z), 2000 (M), 3000 (ZM);
//   - shapes nest by their parents; a leaf takes the figures from its own first one to the next shape's;
//   - a CompoundCurve's points are cut into its parts by the segments (2 / 3 start a line / an arc, 0 / 1
//     continue one; each part starts at the previous one's last point), a CurvePolygon's rings are given
//     by their figure's attribute (1 line, 2 arc, 3 composite);
//   - FullGlobe is type 126, as the server writes it.
// One place differs from AsBinaryZM() (owner, 2026-11-20): a POINT EMPTY -- the value itself, or a member
// of a MULTIPOINT or a collection -- is DuckDB's POINT EMPTY, a point of NaN coordinates, where the server
// writes MULTIPOINT EMPTY for the value and leaves a MULTIPOINT's EMPTY member out. The value as stored.
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {
namespace mssql {
namespace codec {
namespace ssclrt {

//! The deepest collections nest in a value the kernel reads at all; past it the value is refused. DuckDB's
//! own GEOMETRY reads 15 (MAX_RECURSION_DEPTH 16, the root not counted); between the two a value is
//! NotGeometry.
constexpr uint32_t MAX_DEPTH = 64;
constexpr uint32_t GEOMETRY_MAX_DEPTH = 15;

enum class SpatialStatus : uint8_t {
	Ok,
	//! A curve (CircularString, CompoundCurve, CurvePolygon), a FullGlobe, or collections nested deeper than
	//! DuckDB's GEOMETRY reads: not written unless curves are asked for (geometry_as 'wkb').
	NotGeometry,
	//! The value ends before its header, points, figures, shapes or segments do.
	Truncated,
	//! Bytes after the value's last part.
	TrailingBytes,
	//! A version other than 1 or 2.
	BadVersion,
	//! A property bit the format does not define, or both P and L.
	BadProperties,
	//! A figure whose attribute is not 0 - 3, or whose first point is out of order or past the points.
	BadFigure,
	//! A shape whose parent, first figure or type is out of place: a parent that is not a collection
	//! before it, a member of a type its MULTI* cannot hold, a leaf with the wrong number of figures or
	//! points, a curve in version 1, a FullGlobe anywhere but alone.
	BadShape,
	//! A CompoundCurve's segments do not cut its points into parts, or are left over.
	BadSegment,
	//! Collections nested deeper than MAX_DEPTH.
	TooDeep,
	//! WKB longer than MAX_WKB_BYTES, which one DuckDB value cannot hold.
	TooLarge
};

//! The longest WKB one value may be: a DuckDB string's length is 32 bits (string_t::MAX_STRING_SIZE).
constexpr uint64_t MAX_WKB_BYTES = 0xFFFFFFFFULL;

//! A few words for an error message.
const char *DescribeStatus(SpatialStatus status);

//! One value at a time, for a column's kernel: Read checks a value and sizes its WKB, Write puts it where
//! the caller says. The decoder keeps its arrays from one value to the next (cleared, their capacity
//! kept), so a column of them allocates nothing per value once it has met its widest; the single point
//! and the single line of two points (P, L), most of a column of points, take a path of their own that
//! touches none of them.
class WkbDecoder {
public:
	//! Check the MS-SSCLRT bytes (borrowed: they must outlive Write). `geography`: the points are stored
	//! latitude first. `curves`: a curve or FullGlobe in its ISO form (8 - 10, 126) and collections however
	//! deep, rather than NotGeometry.
	SpatialStatus Read(const uint8_t *data, size_t size, bool geography, bool curves);
	//! The exact length of the WKB of the value Read last answered Ok for.
	size_t Size() const {
		return size_;
	}
	//! Its SRID, read (not checked: DQ-45) once Read got past the header.
	uint32_t Srid() const {
		return srid_;
	}
	//! Its WKB: Size() bytes at `dst`.
	void Write(char *dst) const;

	struct Figure {
		uint8_t attribute;
		uint32_t first_point;
	};
	struct Shape {
		int32_t parent;
		int32_t first_figure;
		uint8_t type;
	};

private:
	template <bool WRITE>
	friend class Emitter;

	SpatialStatus ReadSingle(const uint8_t *data, size_t size, bool point);
	SpatialStatus ReadShaped(const uint8_t *data, size_t size, uint8_t version, bool curves);
	SpatialStatus ReadArrays(const uint8_t *data, size_t size, uint8_t version);
	SpatialStatus Validate(uint8_t version, bool &geometry_holds);
	//! Validate's steps.
	bool FiguresInOrder() const;
	SpatialStatus PlaceShape(uint32_t s, uint8_t version, int32_t &previous_figure, bool &geometry_holds);
	bool FirstFigureInOrder(const Shape &shape, int32_t &previous_figure) const;
	bool ParentHolds(uint32_t s) const;
	SpatialStatus LinkMember(uint32_t s, bool &geometry_holds);
	void EndFigures();
	bool LeafHoldsItsFigures(uint32_t s) const;
	bool FullGlobeIsAlone(uint32_t s) const;
	bool PointHasOnePoint(uint32_t s) const;

	const uint8_t *data_ = nullptr;
	size_t size_ = 0;
	uint32_t srid_ = 0;
	bool z_ = false;
	bool m_ = false;
	bool geography_ = false;
	//! P or L: the points after the header, nothing else.
	bool single_ = false;
	uint8_t single_type_ = 0;
	uint32_t point_count_ = 0;
	const uint8_t *points_ = nullptr;
	const uint8_t *zs_ = nullptr;
	const uint8_t *ms_ = nullptr;
	const uint8_t *segments_ = nullptr;
	uint32_t segment_count_ = 0;
	std::vector<Figure> figures_;
	std::vector<Shape> shapes_;
	//! Per shape: the figure after its last; its first child, next sibling and last child (-1: none); depth.
	std::vector<uint32_t> figure_end_;
	std::vector<int32_t> first_child_;
	std::vector<int32_t> next_sibling_;
	std::vector<int32_t> last_child_;
	std::vector<uint32_t> depth_;
};

//! One value as ISO WKB, appended to `out` (left as it was unless Ok): WkbDecoder's Read then Write, for
//! the tests and the fuzz harness. `srid`, when given, is set whenever the header was read.
SpatialStatus ToWkb(const uint8_t *data, size_t size, bool geography, bool curves, std::string &out,
					uint32_t *srid = nullptr);

}  // namespace ssclrt
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
