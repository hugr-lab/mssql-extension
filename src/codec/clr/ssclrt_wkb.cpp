// Spec 093 T5 (D6): MS-SSCLRT geometry / geography into ISO WKB. See the header for the format and the
// mapping (G1's prototype). DuckDB-free.

#include "codec/clr/ssclrt_wkb.hpp"

#include <cstring>

namespace duckdb {
namespace mssql {
namespace codec {
namespace ssclrt {

namespace {

constexpr uint8_t PROP_Z = 0x01;
constexpr uint8_t PROP_M = 0x02;
constexpr uint8_t PROP_P = 0x08;
constexpr uint8_t PROP_L = 0x10;
//! V (0x04, valid) and H (0x20, larger than a hemisphere) are read as stored and change nothing here.
constexpr uint8_t PROP_KNOWN = 0x3F;

constexpr uint64_t CANONICAL_NAN = 0x7FF8000000000000ULL;

//! OGC types as the shapes name them (MS-SSCLRT 2.1.4); 8 - 11 in version 2 only.
constexpr uint8_t T_POINT = 1;
constexpr uint8_t T_LINESTRING = 2;
constexpr uint8_t T_POLYGON = 3;
constexpr uint8_t T_MULTIPOINT = 4;
constexpr uint8_t T_MULTILINESTRING = 5;
constexpr uint8_t T_MULTIPOLYGON = 6;
constexpr uint8_t T_COLLECTION = 7;
constexpr uint8_t T_CIRCULARSTRING = 8;
constexpr uint8_t T_COMPOUNDCURVE = 9;
constexpr uint8_t T_CURVEPOLYGON = 10;
constexpr uint8_t T_FULLGLOBE = 11;
//! The WKB type the server writes for a FullGlobe (G1): no OGC type.
constexpr uint32_t WKB_FULLGLOBE = 126;

//! Figure attributes: version 1 0 interior ring, 1 stroke, 2 exterior ring; version 2 1 line, 2 arc,
//! 3 composite curve (a CurvePolygon's ring by its attribute).
constexpr uint8_t ATTR_ARC = 2;
constexpr uint8_t ATTR_COMPOSITE = 3;

//! The header: u32 SRID, u8 version, u8 properties.
constexpr size_t HEADER_BYTES = 6;

uint32_t LoadU32(const uint8_t *p) {
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
		   (static_cast<uint32_t>(p[3]) << 24);
}

//! Reads little-endian fields, each checked against what is left.
class Reader {
public:
	Reader(const uint8_t *data, size_t size) : p_(data), left_(size) {}
	bool U32(uint32_t &v) {
		if (left_ < 4) {
			return false;
		}
		v = LoadU32(p_);
		p_ += 4;
		left_ -= 4;
		return true;
	}
	//! `count` items of `width` bytes, in place: nullptr when they do not fit.
	const uint8_t *Take(uint64_t count, uint64_t width) {
		const uint64_t bytes = count * width;  // count < 2^32, width <= 16: no overflow
		if (bytes > left_) {
			return nullptr;
		}
		const uint8_t *at = p_;
		p_ += bytes;
		left_ -= static_cast<size_t>(bytes);
		return at;
	}
	size_t Left() const {
		return left_;
	}

private:
	const uint8_t *p_;
	size_t left_;
};

//! Any NaN as the canonical one: a missing Z / M is stored as 0xFFF8... (G1).
inline void PutCoordinate(char *dst, const uint8_t *src) {
	uint64_t bits;
	std::memcpy(&bits, src, sizeof(bits));
	if ((bits & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL && (bits & 0x000FFFFFFFFFFFFFULL) != 0) {
		bits = CANONICAL_NAN;
	}
	std::memcpy(dst, &bits, sizeof(bits));
}

//! Points [begin, end) as WKB. x and y are copied as stored -- the server keeps no NaN there (G7: 24126),
//! so a 2D geometry's run is one copy -- a geography's swapped (stored latitude first, G3); a Z / M, which
//! may be missing (NaN), in its canonical form (G1).
inline char *PutPoints(char *dst, const uint8_t *points, const uint8_t *zs, const uint8_t *ms, uint32_t begin,
					   uint32_t end, bool geography, bool z, bool m) {
	if (!geography && !z && !m) {
		const size_t n = static_cast<size_t>(end - begin) * 16;
		std::memcpy(dst, points + static_cast<size_t>(begin) * 16, n);
		return dst + n;
	}
	for (uint32_t i = begin; i < end; i++) {
		const uint8_t *xy = points + static_cast<size_t>(i) * 16;
		if (geography) {
			std::memcpy(dst, xy + 8, 8);
			std::memcpy(dst + 8, xy, 8);
		} else {
			std::memcpy(dst, xy, 16);
		}
		dst += 16;
		if (z) {
			PutCoordinate(dst, zs + static_cast<size_t>(i) * 8);
			dst += 8;
		}
		if (m) {
			PutCoordinate(dst, ms + static_cast<size_t>(i) * 8);
			dst += 8;
		}
	}
	return dst;
}

inline char *PutU32(char *dst, uint32_t v) {
	const char b[4] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF),
					   static_cast<char>((v >> 16) & 0xFF), static_cast<char>((v >> 24) & 0xFF)};
	std::memcpy(dst, b, 4);
	return dst + 4;
}

uint32_t IsoCode(uint32_t type, bool z, bool m) {
	return type + (z && m ? 3000 : z ? 1000 : m ? 2000 : 0);
}

//! Can a shape of type `child` be a member of one of type `parent`?
bool MemberFits(uint8_t parent, uint8_t child) {
	switch (parent) {
	case T_MULTIPOINT:
		return child == T_POINT;
	case T_MULTILINESTRING:
		return child == T_LINESTRING;
	case T_MULTIPOLYGON:
		return child == T_POLYGON;
	case T_COLLECTION:
		return child >= T_POINT && child <= T_CURVEPOLYGON;	 // FullGlobe never (the server refuses it, 24150)
	default:
		return false;  // a leaf has no members
	}
}

//! x and y, and a Z and an M where the value has them.
uint32_t Dimensions(bool z, bool m) {
	return 2 + (z ? 1 : 0) + (m ? 1 : 0);
}

//! A shape's type is one its version has: 8 - 11 in version 2 only.
bool TypeKnown(uint8_t type, uint8_t version) {
	return type >= T_POINT && type <= T_FULLGLOBE && (version != 1 || type <= T_COLLECTION);
}

//! A Z or an M per point, where the value has them. False when they do not fit.
bool TakeOrdinates(Reader &r, bool present, uint32_t point_count, const uint8_t *&ordinates) {
	if (!present) {
		return true;
	}
	ordinates = r.Take(point_count, 8);
	return ordinates != nullptr;
}

//! The figures: a count, then an attribute and a first point each. False when they do not fit.
bool TakeFigures(Reader &r, std::vector<WkbDecoder::Figure> &figures) {
	uint32_t figure_count;
	if (!r.U32(figure_count)) {
		return false;
	}
	const uint8_t *stored = r.Take(figure_count, 5);
	if (!stored) {
		return false;
	}
	figures.resize(figure_count);
	for (uint32_t f = 0; f < figure_count; f++) {
		const uint8_t *at = stored + static_cast<size_t>(f) * 5;
		figures[f].attribute = at[0];
		figures[f].first_point = LoadU32(at + 1);
	}
	return true;
}

//! The shapes: a count, then a parent, a first figure and a type each. False when they do not fit.
bool TakeShapes(Reader &r, std::vector<WkbDecoder::Shape> &shapes) {
	uint32_t shape_count;
	if (!r.U32(shape_count)) {
		return false;
	}
	const uint8_t *stored = r.Take(shape_count, 9);
	if (!stored) {
		return false;
	}
	shapes.resize(shape_count);
	for (uint32_t s = 0; s < shape_count; s++) {
		const uint8_t *at = stored + static_cast<size_t>(s) * 9;
		const uint32_t parent = LoadU32(at);
		const uint32_t figure = LoadU32(at + 4);
		std::memcpy(&shapes[s].parent, &parent, 4);
		std::memcpy(&shapes[s].first_figure, &figure, 4);
		shapes[s].type = at[8];
	}
	return true;
}

//! Version 2 carries its segments when it has any (G1: a CompoundCurve's); none means none. A count, then
//! a byte each. False when they do not fit.
bool TakeSegments(Reader &r, uint8_t version, const uint8_t *&segments, uint32_t &segment_count) {
	if (version != 2 || r.Left() == 0) {
		return true;
	}
	if (!r.U32(segment_count)) {
		return false;
	}
	segments = r.Take(segment_count, 1);
	return segments != nullptr;
}

}  // namespace

//! The WKB of a validated value (not P / L), walked once to size it (WRITE false: lengths only, no
//! coordinate read) and once to write it. Both walks consume the segments in the stored order, so the
//! sizing walk is also the segments' check.
template <bool WRITE>
class Emitter {
public:
	Emitter(const WkbDecoder &d, char *dst) : d_(d), p_(dst), dims_(Dimensions(d.z_, d.m_)) {}

	bool Emit(int32_t s) {
		switch (d_.shapes_[s].type) {
		case T_POINT:
			EmitPoint(s);
			return true;
		case T_LINESTRING:
		case T_CIRCULARSTRING:
			EmitLine(s);
			return true;
		case T_POLYGON:
			EmitPolygon(s);
			return true;
		case T_COMPOUNDCURVE:
			return EmitCompoundCurve(s);
		case T_CURVEPOLYGON:
			return EmitCurvePolygon(s);
		case T_FULLGLOBE:
			Byte(1);
			U32(WKB_FULLGLOBE);
			return true;
		default:  // the collections; Validate let no other type through
			return EmitCollection(s);
		}
	}

	bool SegmentsUsed() const {
		return segment_at_ == d_.segment_count_;
	}
	uint64_t Bytes() const {
		return bytes_;
	}

private:
	//! A shape's figures: none for an EMPTY one, else from its first to where the next shape's start.
	bool Empty(int32_t s) const {
		return d_.shapes_[s].first_figure < 0;
	}
	uint32_t FirstFigure(int32_t s) const {
		return Empty(s) ? 0 : static_cast<uint32_t>(d_.shapes_[s].first_figure);
	}
	uint32_t FigureCount(int32_t s) const {
		return Empty(s) ? 0 : d_.figure_end_[s] - FirstFigure(s);
	}

	void EmitPoint(int32_t s) {
		Head(T_POINT);
		if (!Empty(s)) {
			Point(d_.figures_[FirstFigure(s)].first_point);
			return;
		}
		// DuckDB's POINT EMPTY: a point of NaN coordinates (K1), where the server writes MULTIPOINT
		// EMPTY or leaves a MULTIPOINT's member out (owner, 2026-11-20).
		for (uint32_t k = 0; k < dims_; k++) {
			NaN();
		}
	}
	//! A LineString or a CircularString: its one figure's points.
	void EmitLine(int32_t s) {
		Head(d_.shapes_[s].type);
		if (Empty(s)) {
			U32(0);
		} else {
			Points(FirstFigure(s));
		}
	}
	//! A Polygon: a ring per figure.
	void EmitPolygon(int32_t s) {
		const uint32_t first = FirstFigure(s);
		const uint32_t figure_count = FigureCount(s);
		Head(T_POLYGON);
		U32(figure_count);
		for (uint32_t f = first; f < first + figure_count; f++) {
			Points(f);
		}
	}
	bool EmitCompoundCurve(int32_t s) {
		Head(T_COMPOUNDCURVE);
		if (Empty(s)) {
			U32(0);
			return true;
		}
		return Compound(FirstFigure(s));
	}
	//! A CurvePolygon: a ring per figure, each a LineString, a CircularString or a CompoundCurve by its
	//! figure's attribute.
	bool EmitCurvePolygon(int32_t s) {
		const uint32_t first = FirstFigure(s);
		const uint32_t figure_count = FigureCount(s);
		Head(T_CURVEPOLYGON);
		U32(figure_count);
		for (uint32_t f = first; f < first + figure_count; f++) {
			const uint8_t attribute = d_.figures_[f].attribute;
			if (attribute == ATTR_COMPOSITE) {
				Head(T_COMPOUNDCURVE);
				if (!Compound(f)) {
					return false;
				}
			} else {
				Head(attribute == ATTR_ARC ? T_CIRCULARSTRING : T_LINESTRING);
				Points(f);
			}
		}
		return true;
	}
	//! A collection: the count of its members, then each of them.
	bool EmitCollection(int32_t s) {
		Head(d_.shapes_[s].type);
		uint32_t members = 0;
		for (int32_t c = d_.first_child_[s]; c >= 0; c = d_.next_sibling_[c]) {
			members++;
		}
		U32(members);
		for (int32_t c = d_.first_child_[s]; c >= 0; c = d_.next_sibling_[c]) {
			if (!Emit(c)) {
				return false;
			}
		}
		return true;
	}

	void Byte(uint8_t b) {
		if (WRITE) {
			*p_++ = static_cast<char>(b);
		}
		bytes_ += 1;
	}
	void U32(uint32_t v) {
		if (WRITE) {
			p_ = PutU32(p_, v);
		}
		bytes_ += 4;
	}
	void NaN() {
		if (WRITE) {
			std::memcpy(p_, &CANONICAL_NAN, 8);
			p_ += 8;
		}
		bytes_ += 8;
	}
	//! Little-endian marker, then the ISO type code.
	void Head(uint32_t type) {
		Byte(1);
		U32(IsoCode(type, d_.z_, d_.m_));
	}
	void Point(uint32_t i) {
		if (WRITE) {
			p_ = PutPoints(p_, d_.points_, d_.zs_, d_.ms_, i, i + 1, d_.geography_, d_.z_, d_.m_);
		}
		bytes_ += 8 * dims_;
	}
	uint32_t FigureEndPoint(uint32_t f) const {
		return f + 1 < d_.figures_.size() ? d_.figures_[f + 1].first_point : d_.point_count_;
	}
	//! A figure's points as a count and the points (a LineString's or a ring's body).
	void Points(uint32_t f) {
		const uint32_t begin = d_.figures_[f].first_point;
		const uint32_t end = FigureEndPoint(f);
		U32(end - begin);
		if (!WRITE) {
			bytes_ += static_cast<uint64_t>(end - begin) * 8 * dims_;
			return;
		}
		p_ = PutPoints(p_, d_.points_, d_.zs_, d_.ms_, begin, end, d_.geography_, d_.z_, d_.m_);
		bytes_ += static_cast<uint64_t>(end - begin) * 8 * dims_;
	}
	//! A CompoundCurve's body: its figure's points cut into parts by the segments, from where the last
	//! compound figure left them. A segment 2 / 3 starts a line / an arc, 0 / 1 continues one; a line
	//! segment takes one point more, an arc two, and a part starts at the previous one's last point. A part
	//! starts only at a 2 / 3: a figure opening with a 0 / 1, or a 0 / 1 of the other kind than the part it
	//! would continue, is refused (review of #594). The part count precedes the parts, so the segments are
	//! walked twice: to count and check, then to write.
	bool Compound(uint32_t f) {
		const uint32_t begin = d_.figures_[f].first_point;
		const uint32_t end = FigureEndPoint(f);
		if (end < begin + 2) {
			return false;  // no segment has room
		}
		uint32_t at = segment_at_;
		uint32_t parts = 0;
		if (!CutParts(begin, end, at, parts)) {
			return false;
		}
		U32(parts);
		Parts(begin, at);
		segment_at_ = at;
		return true;
	}
	//! The first walk: the segments from `at` that cut the points [begin, end) counted into parts, `at`
	//! left after the last of them. False when they do not cut the points.
	bool CutParts(uint32_t begin, uint32_t end, uint32_t &at, uint32_t &parts) const {
		const uint8_t *segments = d_.segments_;
		uint32_t i = begin;
		bool previous_arc = false;
		while (i < end - 1) {
			if (at >= d_.segment_count_) {
				return false;  // the points outrun the segments
			}
			const uint8_t segment = segments[at++];
			if (segment > 3) {
				return false;
			}
			const bool arc = segment == 1 || segment == 3;
			const uint32_t step = arc ? 2 : 1;
			if (i + step > end - 1) {
				return false;  // the segment runs past the figure
			}
			if (segment >= 2) {
				parts++;
				previous_arc = arc;
			} else if (parts == 0 || previous_arc != arc) {
				return false;  // a continuation with no part of its kind to continue
			}
			i += step;
		}
		return true;
	}
	//! The second walk: the parts the segments [segment_at_, at) cut the points from `begin` into, each a
	//! LineString or a CircularString.
	void Parts(uint32_t begin, uint32_t at) {
		const uint8_t *segments = d_.segments_;
		uint32_t k = segment_at_;
		uint32_t i = begin;
		while (k < at) {
			const bool arc = segments[k] == 1 || segments[k] == 3;
			const uint32_t first = i;
			do {
				i += arc ? 2 : 1;
				k++;
			} while (k < at && segments[k] < 2 && (segments[k] == 1) == arc);
			Head(arc ? T_CIRCULARSTRING : T_LINESTRING);
			U32(i - first + 1);
			for (uint32_t q = first; q <= i; q++) {
				Point(q);
			}
		}
	}

	const WkbDecoder &d_;
	char *p_;
	const uint32_t dims_;
	uint64_t bytes_ = 0;
	uint32_t segment_at_ = 0;
};

SpatialStatus WkbDecoder::ReadArrays(const uint8_t *data, size_t size, uint8_t version) {
	Reader r(data + HEADER_BYTES, size - HEADER_BYTES);
	if (!r.U32(point_count_)) {
		return SpatialStatus::Truncated;
	}
	points_ = r.Take(point_count_, 16);
	if (!points_) {
		return SpatialStatus::Truncated;
	}
	if (!TakeOrdinates(r, z_, point_count_, zs_) || !TakeOrdinates(r, m_, point_count_, ms_)) {
		return SpatialStatus::Truncated;
	}
	if (!TakeFigures(r, figures_) || !TakeShapes(r, shapes_)) {
		return SpatialStatus::Truncated;
	}
	if (!TakeSegments(r, version, segments_, segment_count_)) {
		return SpatialStatus::Truncated;
	}
	return r.Left() != 0 ? SpatialStatus::TrailingBytes : SpatialStatus::Ok;
}

//! Check what the writer relies on: figures and shapes in order and in range, every shape where its type
//! may be; and say whether DuckDB's GEOMETRY can hold the value.
SpatialStatus WkbDecoder::Validate(uint8_t version, bool &geometry_holds) {
	geometry_holds = true;
	if (!FiguresInOrder()) {
		return SpatialStatus::BadFigure;
	}
	const uint32_t shape_count = static_cast<uint32_t>(shapes_.size());
	if (shape_count == 0) {
		return SpatialStatus::BadShape;	 // even an EMPTY value has its one shape
	}
	const uint32_t figure_count = static_cast<uint32_t>(figures_.size());
	figure_end_.assign(shape_count, figure_count);
	first_child_.assign(shape_count, -1);
	next_sibling_.assign(shape_count, -1);
	last_child_.assign(shape_count, -1);
	depth_.assign(shape_count, 0);
	int32_t previous_figure = 0;
	for (uint32_t s = 0; s < shape_count; s++) {
		const auto placed = PlaceShape(s, version, previous_figure, geometry_holds);
		if (placed != SpatialStatus::Ok) {
			return placed;
		}
	}
	EndFigures();
	// Each leaf holds the figures and points its type has room for.
	for (uint32_t s = 0; s < shape_count; s++) {
		if (!LeafHoldsItsFigures(s)) {
			return SpatialStatus::BadShape;
		}
	}
	return SpatialStatus::Ok;
}

//! Every figure has an attribute the format defines and starts at or after the one before, within the points.
bool WkbDecoder::FiguresInOrder() const {
	uint32_t previous_point = 0;
	for (const auto &figure : figures_) {
		if (figure.attribute > 3 || figure.first_point < previous_point || figure.first_point > point_count_) {
			return false;
		}
		previous_point = figure.first_point;
	}
	return true;
}

//! Shape `s` in the stored order: its type, its first figure (`previous_figure`: the last one a shape before
//! it named), its parent; a member is linked under its parent. A curve or a FullGlobe clears `geometry_holds`,
//! and so does a member nested past what a GEOMETRY reads.
SpatialStatus WkbDecoder::PlaceShape(uint32_t s, uint8_t version, int32_t &previous_figure, bool &geometry_holds) {
	const Shape &shape = shapes_[s];
	if (!TypeKnown(shape.type, version)) {
		return SpatialStatus::BadShape;
	}
	if (shape.type >= T_CIRCULARSTRING) {
		geometry_holds = false;
	}
	if (!FirstFigureInOrder(shape, previous_figure)) {
		return SpatialStatus::BadShape;
	}
	if (s == 0) {
		return shape.parent == -1 ? SpatialStatus::Ok : SpatialStatus::BadShape;
	}
	if (!ParentHolds(s)) {
		return SpatialStatus::BadShape;
	}
	return LinkMember(s, geometry_holds);
}

//! A shape's first figure is none (-1), or a figure at or after the one the shapes before it reached.
bool WkbDecoder::FirstFigureInOrder(const Shape &shape, int32_t &previous_figure) const {
	if (shape.first_figure < 0) {
		return shape.first_figure == -1;
	}
	const uint32_t figure_count = static_cast<uint32_t>(figures_.size());
	if (static_cast<uint32_t>(shape.first_figure) >= figure_count || shape.first_figure < previous_figure) {
		return false;
	}
	previous_figure = shape.first_figure;
	return true;
}

//! A member names a collection before it, and the shapes are in depth-first order: the parent is the
//! shape before this one or one of its ancestors, so the emission order is the stored order (the
//! figures and the segments are consumed in it).
bool WkbDecoder::ParentHolds(uint32_t s) const {
	const Shape &shape = shapes_[s];
	if (shape.parent < 0 || static_cast<uint32_t>(shape.parent) >= s ||
		!MemberFits(shapes_[shape.parent].type, shape.type)) {
		return false;
	}
	int32_t ancestor = static_cast<int32_t>(s) - 1;
	while (ancestor >= 0 && ancestor != shape.parent) {
		ancestor = shapes_[ancestor].parent;
	}
	return ancestor == shape.parent;
}

//! A member's depth, and its place as its parent's last child so far.
SpatialStatus WkbDecoder::LinkMember(uint32_t s, bool &geometry_holds) {
	const int32_t parent = shapes_[s].parent;
	depth_[s] = depth_[parent] + 1;
	if (depth_[s] > MAX_DEPTH) {
		return SpatialStatus::TooDeep;
	}
	if (depth_[s] > GEOMETRY_MAX_DEPTH) {
		geometry_holds = false;
	}
	if (last_child_[parent] < 0) {
		first_child_[parent] = static_cast<int32_t>(s);
	} else {
		next_sibling_[last_child_[parent]] = static_cast<int32_t>(s);
	}
	last_child_[parent] = static_cast<int32_t>(s);
	return SpatialStatus::Ok;
}

//! A shape's figures end where the next shape that has any starts (G1's prototype).
void WkbDecoder::EndFigures() {
	uint32_t next_start = static_cast<uint32_t>(figures_.size());
	for (uint32_t s = static_cast<uint32_t>(shapes_.size()); s-- > 0;) {
		figure_end_[s] = next_start;
		if (shapes_[s].first_figure >= 0) {
			next_start = static_cast<uint32_t>(shapes_[s].first_figure);
		}
	}
}

//! Shape `s` holds the figures and points its type has room for.
bool WkbDecoder::LeafHoldsItsFigures(uint32_t s) const {
	const Shape &shape = shapes_[s];
	if (shape.type == T_FULLGLOBE) {
		return FullGlobeIsAlone(s);
	}
	if (shape.first_figure < 0) {
		return true;  // an EMPTY shape of any other type
	}
	const uint32_t figures = figure_end_[s] - static_cast<uint32_t>(shape.first_figure);
	switch (shape.type) {
	case T_POINT:
		return PointHasOnePoint(s);
	case T_LINESTRING:
	case T_CIRCULARSTRING:
	case T_COMPOUNDCURVE:
		return figures == 1;
	case T_POLYGON:
	case T_CURVEPOLYGON:
		return figures != 0;
	default:  // a collection's figures are its members'
		return true;
	}
}

//! G1: the value alone, with no figure (it is stored as an EMPTY shape of type 11).
bool WkbDecoder::FullGlobeIsAlone(uint32_t s) const {
	return shapes_[s].first_figure < 0 && s == 0 && shapes_.size() == 1;
}

//! A Point that is not EMPTY: one figure of one point.
bool WkbDecoder::PointHasOnePoint(uint32_t s) const {
	const uint32_t first = static_cast<uint32_t>(shapes_[s].first_figure);
	const uint32_t figure_count = static_cast<uint32_t>(figures_.size());
	const uint32_t end = first + 1 < figure_count ? figures_[first + 1].first_point : point_count_;
	return figure_end_[s] - first == 1 && end - figures_[first].first_point == 1;
}

SpatialStatus WkbDecoder::Read(const uint8_t *data, size_t size, bool geography, bool curves) {
	data_ = data;
	size_ = 0;
	if (size < HEADER_BYTES) {
		return SpatialStatus::Truncated;
	}
	srid_ = LoadU32(data);
	const uint8_t version = data[4];
	const uint8_t properties = data[5];
	if (version != 1 && version != 2) {
		return SpatialStatus::BadVersion;
	}
	if ((properties & ~PROP_KNOWN) != 0 || ((properties & PROP_P) && (properties & PROP_L))) {
		return SpatialStatus::BadProperties;
	}
	z_ = (properties & PROP_Z) != 0;
	m_ = (properties & PROP_M) != 0;
	geography_ = geography;
	single_ = (properties & (PROP_P | PROP_L)) != 0;
	return single_ ? ReadSingle(data, size, (properties & PROP_P) != 0) : ReadShaped(data, size, version, curves);
}

//! The single point, or the single line of two points: the points, then their Z, then their M, and
//! nothing else -- no arrays, no shapes, so nothing to check but the length.
SpatialStatus WkbDecoder::ReadSingle(const uint8_t *data, size_t size, bool point) {
	const uint32_t dims = Dimensions(z_, m_);
	point_count_ = point ? 1 : 2;
	const size_t stored = HEADER_BYTES + static_cast<size_t>(point_count_) * 8 * dims;
	if (size < stored) {
		return SpatialStatus::Truncated;
	}
	if (size > stored) {
		return SpatialStatus::TrailingBytes;
	}
	single_type_ = point ? T_POINT : T_LINESTRING;
	points_ = data + HEADER_BYTES;
	zs_ = points_ + static_cast<size_t>(point_count_) * 16;
	ms_ = zs_ + (z_ ? static_cast<size_t>(point_count_) * 8 : 0);
	size_ = 5 + (single_type_ == T_LINESTRING ? 4 : 0) + static_cast<size_t>(point_count_) * 8 * dims;
	return SpatialStatus::Ok;
}

//! Any other value: its arrays read and checked, its WKB sized.
SpatialStatus WkbDecoder::ReadShaped(const uint8_t *data, size_t size, uint8_t version, bool curves) {
	segments_ = nullptr;
	segment_count_ = 0;
	zs_ = nullptr;
	ms_ = nullptr;
	const auto parsed = ReadArrays(data, size, version);
	if (parsed != SpatialStatus::Ok) {
		return parsed;
	}
	bool geometry_holds;
	const auto valid = Validate(version, geometry_holds);
	if (valid != SpatialStatus::Ok) {
		return valid;
	}
	// The segments are checked before a curve is answered NotGeometry, so a value whose segments do not cut
	// its points is BadSegment whatever geometry_as says (review of #594).
	Emitter<false> sizing(*this, nullptr);
	if (!sizing.Emit(0) || !sizing.SegmentsUsed()) {
		return SpatialStatus::BadSegment;
	}
	if (!geometry_holds && !curves) {
		return SpatialStatus::NotGeometry;
	}
	if (sizing.Bytes() > MAX_WKB_BYTES) {
		return SpatialStatus::TooLarge;
	}
	size_ = static_cast<size_t>(sizing.Bytes());
	return SpatialStatus::Ok;
}

void WkbDecoder::Write(char *dst) const {
	if (!single_) {
		Emitter<true> writer(*this, dst);
		writer.Emit(0);	 // Read has walked it: it succeeds
		return;
	}
	*dst++ = 1;
	dst = PutU32(dst, IsoCode(single_type_, z_, m_));
	if (single_type_ == T_LINESTRING) {
		dst = PutU32(dst, point_count_);
	}
	PutPoints(dst, points_, zs_, ms_, 0, point_count_, geography_, z_, m_);
}

const char *DescribeStatus(SpatialStatus status) {
	switch (status) {
	case SpatialStatus::Ok:
		return "ok";
	case SpatialStatus::NotGeometry:
		return "a curve, a FullGlobe or collections nested past 15, which a GEOMETRY cannot hold";
	case SpatialStatus::Truncated:
		return "the value ends before its points, figures, shapes or segments do";
	case SpatialStatus::TrailingBytes:
		return "bytes follow the value's last part";
	case SpatialStatus::BadVersion:
		return "a serialization version other than 1 or 2";
	case SpatialStatus::BadProperties:
		return "a property bit the format does not define";
	case SpatialStatus::BadFigure:
		return "a figure out of order or past the points";
	case SpatialStatus::BadShape:
		return "a shape out of place (its parent, its figures or its type)";
	case SpatialStatus::BadSegment:
		return "a compound curve's segments do not cut its points";
	case SpatialStatus::TooDeep:
		return "collections nested deeper than 64";
	case SpatialStatus::TooLarge:
		return "its WKB would pass 4 GiB, more than one DuckDB value holds";
	}
	return "?";
}

SpatialStatus ToWkb(const uint8_t *data, size_t size, bool geography, bool curves, std::string &out, uint32_t *srid) {
	WkbDecoder decoder;
	const auto status = decoder.Read(data, size, geography, curves);
	if (srid && size >= HEADER_BYTES) {
		*srid = decoder.Srid();
	}
	if (status != SpatialStatus::Ok) {
		return status;
	}
	const size_t start = out.size();
	out.resize(start + decoder.Size());
	decoder.Write(&out[start]);
	return SpatialStatus::Ok;
}

}  // namespace ssclrt
}  // namespace codec
}  // namespace mssql
}  // namespace duckdb
