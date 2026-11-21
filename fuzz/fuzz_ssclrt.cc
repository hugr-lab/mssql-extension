// Fuzz harness: a geometry / geography value's MS-SSCLRT bytes (spec 093 T5, D6) as ISO WKB, DuckDB-free.
// The input is one value's bytes, as the PLP framing assembles them. Checked beyond "no crash": every call
// ends in a refusal with nothing written, or in WKB that a walker of ISO WKB reads to its last byte --
// little-endian, one Z / M flag for the whole value, the OGC types 1 - 7 (and, when curves are asked for,
// 8 - 10 and FullGlobe's 126), each MULTI* holding its own member type, collections no deeper than DuckDB's
// GEOMETRY reads unless curves are asked for. The geometry form and the WKB form of a value agree: where
// the first is written the second is the same bytes, and NotGeometry is said only of a value the second
// reads or refuses for its segments. The output stays within a bound of the input (spec 093 § Security).
// A WkbDecoder kept across inputs, as the column kernel keeps one across a chunk, answers as a fresh one.
// Seeds: G1's stored values (fuzz/corpus/ssclrt).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "codec/clr/ssclrt_wkb.hpp"

using namespace duckdb::mssql::codec::ssclrt;

static void Require(bool ok) {
	if (!ok) {
		__builtin_trap();
	}
}

namespace {

struct Walker {
	const std::string &w;
	size_t at = 0;
	bool curves;
	int dims = 0;  // set by the root
	uint32_t z_m = 0;

	Walker(const std::string &wkb, bool curves_p) : w(wkb), curves(curves_p) {}

	uint32_t U32() {
		Require(at + 4 <= w.size());
		uint32_t v;
		std::memcpy(&v, w.data() + at, 4);
		at += 4;
		return v;
	}
	void Skip(size_t n) {
		Require(n <= w.size() - at);
		at += n;
	}
	void Points(uint32_t n) {
		Require(n <= (w.size() - at) / (8u * dims));
		Skip(static_cast<size_t>(n) * 8 * dims);
	}
	//! One geometry; `required` is the member type a MULTI* holds (0: any), `depth` its collection ancestors.
	void Geometry(uint32_t required, uint32_t depth, bool root) {
		Require(at < w.size() && static_cast<uint8_t>(w[at]) == 1);
		at++;
		const uint32_t code = U32();
		if (code == 126) {
			Require(curves && root && at == w.size());	// FullGlobe: the value alone, as the server writes it
			return;
		}
		const uint32_t type = code % 1000;
		const uint32_t flags = code / 1000;
		Require(flags <= 3);
		if (root) {
			z_m = flags;
			dims = 2 + ((flags & 1) ? 1 : 0) + ((flags & 2) ? 1 : 0);
		} else {
			Require(flags == z_m);
		}
		Require(required == 0 || type == required);
		Require(type >= 1 && type <= (curves ? 10u : 7u));
		Require(curves || depth <= GEOMETRY_MAX_DEPTH);
		Require(depth <= MAX_DEPTH);
		switch (type) {
		case 1:
			Points(1);
			return;
		case 2:
		case 8:
			Points(U32());
			return;
		case 3: {
			const uint32_t rings = U32();
			for (uint32_t r = 0; r < rings; r++) {
				Points(U32());
			}
			return;
		}
		case 9: {  // a CompoundCurve: LineString / CircularString parts
			const uint32_t parts = U32();
			for (uint32_t p = 0; p < parts; p++) {
				const uint32_t part = PeekType();
				Require(part == 2 || part == 8);
				Geometry(part, depth, false);
			}
			return;
		}
		case 10: {	// a CurvePolygon: rings as LineString / CircularString / CompoundCurve
			const uint32_t rings = U32();
			for (uint32_t r = 0; r < rings; r++) {
				const uint32_t ring = PeekType();
				Require(ring == 2 || ring == 8 || ring == 9);
				Geometry(ring, depth, false);
			}
			return;
		}
		default: {	// 4 - 7
			const uint32_t members = U32();
			Require(members <= w.size() - at);	// every member takes bytes
			const uint32_t member = type == 7 ? 0 : type - 3;
			for (uint32_t m = 0; m < members; m++) {
				Geometry(member, depth + 1, false);
			}
			return;
		}
		}
	}
	//! The type of the geometry starting at `at`, its marker and code left to Geometry.
	uint32_t PeekType() const {
		Require(at + 5 <= w.size());
		uint32_t code;
		std::memcpy(&code, w.data() + at + 1, 4);
		return code % 1000;
	}
};

void CheckWkb(const std::string &wkb, bool curves) {
	Walker walker(wkb, curves);
	walker.Geometry(0, 0, true);
	Require(walker.at == wkb.size());
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	for (int geography = 0; geography < 2; geography++) {
		std::string as_geometry = "x";	// a byte that must survive a refusal
		std::string as_wkb;
		uint32_t srid = 0;
		const auto geometry_status = ToWkb(data, size, geography != 0, false, as_geometry, &srid);
		const auto wkb_status = ToWkb(data, size, geography != 0, true, as_wkb);
		if (geometry_status != SpatialStatus::Ok) {
			Require(as_geometry == "x");
		} else {
			as_geometry.erase(0, 1);
			CheckWkb(as_geometry, false);
			Require(wkb_status == SpatialStatus::Ok && as_wkb == as_geometry);
		}
		if (wkb_status == SpatialStatus::Ok) {
			CheckWkb(as_wkb, true);
			Require(as_wkb.size() <= 16 * size + 64);
		} else {
			Require(as_wkb.empty());
		}
		if (geometry_status == SpatialStatus::NotGeometry) {
			Require(wkb_status == SpatialStatus::Ok || wkb_status == SpatialStatus::BadSegment);
		}
		if (wkb_status == SpatialStatus::NotGeometry) {
			__builtin_trap();  // curves asked for: never said
		}
		// The column kernel keeps one decoder across a chunk's values: one kept across every input this
		// process has seen answers as a fresh one does (nothing of an earlier value leaks into a later one).
		static WkbDecoder kept;
		Require(kept.Read(data, size, geography != 0, true) == wkb_status);
		if (wkb_status == SpatialStatus::Ok) {
			std::string again(kept.Size(), '\0');
			kept.Write(&again[0]);
			Require(again == as_wkb);
		}
	}
	return 0;
}
