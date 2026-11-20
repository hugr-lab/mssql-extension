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

}  // namespace

//! The WKB of a validated value (not P / L), walked once to size it (WRITE false: lengths only, no
//! coordinate read) and once to write it. Both walks consume the segments in the stored order, so the
//! sizing walk is also the segments' check.
template <bool WRITE>
class Emitter {
public:
	Emitter(const WkbDecoder &d, char *dst) : d_(d), p_(dst), dims_(2 + (d.z_ ? 1 : 0) + (d.m_ ? 1 : 0)) {}

	bool Emit(int32_t s) {
		const WkbDecoder::Shape &shape = d_.shapes_[s];
		const bool empty = shape.first_figure < 0;
		const uint32_t first = empty ? 0 : static_cast<uint32_t>(shape.first_figure);
		const uint32_t figure_count = empty ? 0 : d_.figure_end_[s] - first;
		switch (shape.type) {
		case T_POINT:
			Head(T_POINT);
			if (empty) {
				// DuckDB's POINT EMPTY: a point of NaN coordinates (K1), where the server writes MULTIPOINT
				// EMPTY or leaves a MULTIPOINT's member out (owner, 2026-11-20).
				for (uint32_t k = 0; k < dims_; k++) {
					NaN();
				}
			} else {
				Point(d_.figures_[first].first_point);
			}
			return true;
		case T_LINESTRING:
		case T_CIRCULARSTRING:
			Head(shape.type);
			if (empty) {
				U32(0);
			} else {
				Points(first);
			}
			return true;
		case T_POLYGON:
			Head(T_POLYGON);
			U32(figure_count);
			for (uint32_t f = first; f < first + figure_count; f++) {
				Points(f);
			}
			return true;
		case T_COMPOUNDCURVE:
			Head(T_COMPOUNDCURVE);
			if (empty) {
				U32(0);
				return true;
			}
			return Compound(first);
		case T_CURVEPOLYGON:
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
		case T_FULLGLOBE:
			Byte(1);
			U32(WKB_FULLGLOBE);
			return true;
		default: {	// the collections; Validate let no other type through
			Head(shape.type);
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
		}
	}

	bool SegmentsUsed() const {
		return segment_at_ == d_.segment_count_;
	}
	uint64_t Bytes() const {
		return bytes_;
	}

private:
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
	//! segment takes one point more, an arc two, and a part starts at the previous one's last point. The
	//! part count precedes the parts, so the segments are walked twice: to count and check, then to write.
	bool Compound(uint32_t f) {
		const uint32_t begin = d_.figures_[f].first_point;
		const uint32_t end = FigureEndPoint(f);
		if (end < begin + 2) {
			return false;  // no segment has room
		}
		const uint8_t *segments = d_.segments_;
		uint32_t at = segment_at_;
		uint32_t i = begin;
		uint32_t parts = 0;
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
			if (segment >= 2 || parts == 0 || previous_arc != arc) {
				parts++;
				previous_arc = arc;
			}
			i += step;
		}
		U32(parts);
		uint32_t k = segment_at_;
		i = begin;
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
		segment_at_ = at;
		return true;
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
	if (z_ && !(zs_ = r.Take(point_count_, 8))) {
		return SpatialStatus::Truncated;
	}
	if (m_ && !(ms_ = r.Take(point_count_, 8))) {
		return SpatialStatus::Truncated;
	}
	uint32_t figure_count;
	if (!r.U32(figure_count)) {
		return SpatialStatus::Truncated;
	}
	const uint8_t *figures = r.Take(figure_count, 5);
	if (!figures) {
		return SpatialStatus::Truncated;
	}
	figures_.resize(figure_count);
	for (uint32_t f = 0; f < figure_count; f++) {
		const uint8_t *at = figures + static_cast<size_t>(f) * 5;
		figures_[f].attribute = at[0];
		figures_[f].first_point = LoadU32(at + 1);
	}
	uint32_t shape_count;
	if (!r.U32(shape_count)) {
		return SpatialStatus::Truncated;
	}
	const uint8_t *shapes = r.Take(shape_count, 9);
	if (!shapes) {
		return SpatialStatus::Truncated;
	}
	shapes_.resize(shape_count);
	for (uint32_t s = 0; s < shape_count; s++) {
		const uint8_t *at = shapes + static_cast<size_t>(s) * 9;
		const uint32_t parent = LoadU32(at);
		const uint32_t figure = LoadU32(at + 4);
		std::memcpy(&shapes_[s].parent, &parent, 4);
		std::memcpy(&shapes_[s].first_figure, &figure, 4);
		shapes_[s].type = at[8];
	}
	// Version 2 carries its segments when it has any (G1: a CompoundCurve's); none means none.
	if (version == 2 && r.Left() > 0) {
		if (!r.U32(segment_count_)) {
			return SpatialStatus::Truncated;
		}
		segments_ = r.Take(segment_count_, 1);
		if (!segments_) {
			return SpatialStatus::Truncated;
		}
	}
	return r.Left() != 0 ? SpatialStatus::TrailingBytes : SpatialStatus::Ok;
}

//! Check what the writer relies on: figures and shapes in order and in range, every shape where its type
//! may be; and say whether DuckDB's GEOMETRY can hold the value.
SpatialStatus WkbDecoder::Validate(uint8_t version, bool &geometry_holds) {
	geometry_holds = true;
	uint32_t previous_point = 0;
	for (const auto &figure : figures_) {
		if (figure.attribute > 3 || figure.first_point < previous_point || figure.first_point > point_count_) {
			return SpatialStatus::BadFigure;
		}
		previous_point = figure.first_point;
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
		const Shape &shape = shapes_[s];
		if (shape.type < T_POINT || shape.type > T_FULLGLOBE || (version == 1 && shape.type > T_COLLECTION)) {
			return SpatialStatus::BadShape;
		}
		if (shape.type >= T_CIRCULARSTRING) {
			geometry_holds = false;
		}
		if (shape.first_figure >= 0) {
			if (static_cast<uint32_t>(shape.first_figure) >= figure_count || shape.first_figure < previous_figure) {
				return SpatialStatus::BadShape;
			}
			previous_figure = shape.first_figure;
		} else if (shape.first_figure != -1) {
			return SpatialStatus::BadShape;
		}
		if (s == 0) {
			if (shape.parent != -1) {
				return SpatialStatus::BadShape;
			}
			continue;
		}
		// A member names a collection before it, and the shapes are in depth-first order: the parent is the
		// shape before this one or one of its ancestors, so the emission order is the stored order (the
		// figures and the segments are consumed in it).
		if (shape.parent < 0 || static_cast<uint32_t>(shape.parent) >= s ||
			!MemberFits(shapes_[shape.parent].type, shape.type)) {
			return SpatialStatus::BadShape;
		}
		int32_t ancestor = static_cast<int32_t>(s) - 1;
		while (ancestor >= 0 && ancestor != shape.parent) {
			ancestor = shapes_[ancestor].parent;
		}
		if (ancestor != shape.parent) {
			return SpatialStatus::BadShape;
		}
		depth_[s] = depth_[shape.parent] + 1;
		if (depth_[s] > MAX_DEPTH) {
			return SpatialStatus::TooDeep;
		}
		if (depth_[s] > GEOMETRY_MAX_DEPTH) {
			geometry_holds = false;
		}
		if (last_child_[shape.parent] < 0) {
			first_child_[shape.parent] = static_cast<int32_t>(s);
		} else {
			next_sibling_[last_child_[shape.parent]] = static_cast<int32_t>(s);
		}
		last_child_[shape.parent] = static_cast<int32_t>(s);
	}
	// A shape's figures end where the next shape that has any starts (G1's prototype).
	uint32_t next_start = figure_count;
	for (uint32_t s = shape_count; s-- > 0;) {
		figure_end_[s] = next_start;
		if (shapes_[s].first_figure >= 0) {
			next_start = static_cast<uint32_t>(shapes_[s].first_figure);
		}
	}
	// Each leaf holds the figures and points its type has room for.
	for (uint32_t s = 0; s < shape_count; s++) {
		const Shape &shape = shapes_[s];
		const bool empty = shape.first_figure < 0;
		const uint32_t first = empty ? 0 : static_cast<uint32_t>(shape.first_figure);
		const uint32_t figures = empty ? 0 : figure_end_[s] - first;
		switch (shape.type) {
		case T_POINT:
			if (!empty) {
				const uint32_t end = first + 1 < figure_count ? figures_[first + 1].first_point : point_count_;
				if (figures != 1 || end - figures_[first].first_point != 1) {
					return SpatialStatus::BadShape;
				}
			}
			break;
		case T_LINESTRING:
		case T_CIRCULARSTRING:
		case T_COMPOUNDCURVE:
			if (!empty && figures != 1) {
				return SpatialStatus::BadShape;
			}
			break;
		case T_POLYGON:
		case T_CURVEPOLYGON:
			if (!empty && figures == 0) {
				return SpatialStatus::BadShape;
			}
			break;
		case T_FULLGLOBE:
			// G1: the value alone, with no figure (it is stored as an EMPTY shape of type 11).
			if (!empty || s != 0 || shape_count != 1) {
				return SpatialStatus::BadShape;
			}
			break;
		default:  // a collection's figures are its members'
			break;
		}
	}
	return SpatialStatus::Ok;
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
	const uint32_t dims = 2 + (z_ ? 1 : 0) + (m_ ? 1 : 0);
	single_ = (properties & (PROP_P | PROP_L)) != 0;
	if (single_) {
		// The single point, or the single line of two points: the points, then their Z, then their M, and
		// nothing else -- no arrays, no shapes, so nothing to check but the length.
		point_count_ = (properties & PROP_P) ? 1 : 2;
		const size_t stored = HEADER_BYTES + static_cast<size_t>(point_count_) * 8 * dims;
		if (size < stored) {
			return SpatialStatus::Truncated;
		}
		if (size > stored) {
			return SpatialStatus::TrailingBytes;
		}
		single_type_ = (properties & PROP_P) ? T_POINT : T_LINESTRING;
		points_ = data + HEADER_BYTES;
		zs_ = points_ + static_cast<size_t>(point_count_) * 16;
		ms_ = zs_ + (z_ ? static_cast<size_t>(point_count_) * 8 : 0);
		size_ = 5 + (single_type_ == T_LINESTRING ? 4 : 0) + static_cast<size_t>(point_count_) * 8 * dims;
		return SpatialStatus::Ok;
	}
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
	if (!geometry_holds && !curves) {
		return SpatialStatus::NotGeometry;
	}
	Emitter<false> sizing(*this, nullptr);
	if (!sizing.Emit(0) || !sizing.SegmentsUsed()) {
		return SpatialStatus::BadSegment;
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
