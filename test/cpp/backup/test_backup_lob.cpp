// Spec 085 PR 2b step 6: the catalog's LOB tree reader over synthetic text pages, handed over by a
// callback: an in-row root over DATA fragments and over an INTERNAL node, and every bound a crafted
// tree must hit by name -- a child claiming more than its parent (refused before it is read), a
// fragment referenced twice, a slot count past the page, a page of another allocation unit, a value
// past the caller's bound, a leaf whose size is not its parent's claim, a pointer of another kind.

#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "backup/backup_lob.hpp"

using namespace duckdb::mssql::backup;

namespace {

int failures = 0;

void Check(bool ok, const std::string &what) {
	std::cout << (ok ? "  ok   " : "  FAIL ") << what << std::endl;
	if (!ok) {
		failures++;
	}
}

std::string ErrorOf(const std::function<void()> &f) {
	try {
		f();
	} catch (std::exception &e) {
		return e.what();
	}
	return std::string();
}

constexpr uint32_t OBJECT = 7;
constexpr uint64_t UNIT = static_cast<uint64_t>(OBJECT) << 16;

void Put(std::vector<uint8_t> &b, size_t at, uint64_t v, size_t bytes) {
	for (size_t i = 0; i < bytes; i++) {
		b[at + i] = static_cast<uint8_t>(v >> (8 * i));
	}
}

void Append(std::string &b, uint64_t v, size_t bytes) {
	for (size_t i = 0; i < bytes; i++) {
		b.push_back(static_cast<char>(v >> (8 * i)));
	}
}

// A text page of `records` (each a fragment record's bytes), from offset 96, slot i the i-th.
std::vector<uint8_t> TextPage(uint32_t number, const std::vector<std::string> &records, uint32_t object = OBJECT) {
	std::vector<uint8_t> p(PAGE_SIZE, 0);
	p[0] = 1;  // header version
	p[1] = 3;  // a text page (TEXT_MIX)
	Put(p, 22, records.size(), 2);
	Put(p, 24, object, 4);
	Put(p, 32, number, 4);
	Put(p, 36, 1, 2);
	size_t at = 96;
	for (size_t i = 0; i < records.size(); i++) {
		std::copy(records[i].begin(), records[i].end(), p.begin() + static_cast<long>(at));
		Put(p, PAGE_SIZE - 2 - 2 * i, at, 2);
		at += records[i].size();
	}
	return p;
}

// A fragment record of type `type` with `body` after its 14-byte header.
std::string Fragment(uint16_t type, const std::string &body) {
	std::string r;
	r.push_back(0x08);
	r.push_back(0x00);
	Append(r, 14 + body.size(), 2);
	Append(r, 0x1234, 8);  // blob id
	Append(r, type, 2);
	return r + body;
}

std::string Data(const std::string &bytes) {
	return Fragment(3, bytes);
}

struct Link {
	uint64_t cumulative;
	uint32_t page;
	uint16_t slot;
};

std::string Internal(const std::vector<Link> &links) {
	std::string body;
	Append(body, links.size(), 2);	// maxlinks
	Append(body, links.size(), 2);	// curlinks
	Append(body, 0, 2);				// level
	for (const auto &l : links) {
		Append(body, l.cumulative, 8);
		Append(body, l.page, 4);
		Append(body, 1, 2);
		Append(body, l.slot, 2);
	}
	return Fragment(2, body);
}

std::string Root(const std::vector<Link> &links, uint8_t kind = 4, uint8_t level = 0) {
	std::string r;
	r.push_back(static_cast<char>(kind));
	r.push_back(0);
	r.push_back(static_cast<char>(level));
	r.append(9, '\0');
	for (const auto &l : links) {
		Append(r, l.cumulative, 4);
		Append(r, l.page, 4);
		Append(r, 1, 2);
		Append(r, l.slot, 2);
	}
	return r;
}

struct Pages {
	std::map<uint32_t, std::vector<uint8_t>> pages;
	size_t fetched = 0;
	LobPageFetch Fetch() {
		return [this](const PagePointer &at, Page &out) {
			fetched++;
			out.bytes = pages.at(at.page);
			out.header = ReadPageHeader(out.bytes.data());
		};
	}
};

std::string SmallRoot(const std::string &bytes) {
	std::string body;
	Append(body, bytes.size(), 2);
	Append(body, 0, 4);
	return Fragment(0, body + bytes);
}

std::string LargeRoot(const std::vector<Link> &links, uint16_t level) {
	std::string body;
	Append(body, 5, 2);	 // maxlinks
	Append(body, links.size(), 2);
	Append(body, level, 2);
	Append(body, 0, 4);
	for (const auto &l : links) {
		Append(body, l.cumulative, 4);
		Append(body, l.page, 4);
		Append(body, 1, 2);
		Append(body, l.slot, 2);
	}
	return Fragment(5, body);
}

std::string InternalAt(const std::vector<Link> &links, uint16_t level) {
	std::string r = Internal(links);
	r[14 + 4] = static_cast<char>(level);
	return r;
}

// The 16-byte pointer to a root record, and a row-overflow pointer.
std::string RootPointer(uint32_t page, uint16_t slot) {
	std::string r;
	Append(r, 0x12340000, 8);
	Append(r, page, 4);
	Append(r, 1, 2);
	Append(r, slot, 2);
	return r;
}

std::string Overflow(uint32_t length, uint32_t page, uint16_t slot) {
	std::string r(1, '\x02');
	r.append(11, '\0');
	Append(r, length, 4);
	Append(r, page, 4);
	Append(r, 1, 2);
	Append(r, slot, 2);
	return r;
}

struct MapSink : LobSink {
	std::string data;
	std::vector<LobNode> nodes;
	void Data(const uint8_t *d, size_t n) override {
		data.append(reinterpret_cast<const char *>(d), n);
	}
	void Node(const LobNode &node) override {
		nodes.push_back(node);
	}
};

constexpr uint64_t OVERFLOW_UNIT = static_cast<uint64_t>(OBJECT + 2) << 16;

LobValue Walk(Pages &p, const std::string &pointer, LobWalk mode, MapSink &sink, uint64_t lob = UNIT,
			  uint64_t overflow = OVERFLOW_UNIT, uint64_t max_bytes = 1 << 20) {
	const OffRowPointer parsed =
		ParseOffRowPointer(reinterpret_cast<const uint8_t *>(pointer.data()), pointer.size(), "the test value");
	LobUnits units;
	units.lob = lob;
	units.row_overflow = overflow;
	return WalkLob(p.Fetch(), parsed, units, mode, max_bytes, "the test value", sink);
}

std::string Read(Pages &p, const std::string &root, uint64_t max_bytes = 1 << 20) {
	return ReadLobValue(p.Fetch(), root, UNIT, "the test value", max_bytes);
}

}  // namespace

int main() {
	std::cout << "LOB trees (spec 085 PR 2b step 6)" << std::endl;
	Pages p;
	p.pages[100] = TextPage(100, {Data("hello"), Data("abc")});
	p.pages[101] = TextPage(101, {Internal({{5, 100, 0}, {8, 100, 1}})});

	Check(Read(p, Root({{5, 100, 0}, {8, 100, 1}})) == "helloabc", "a root over two DATA fragments");
	Check(Read(p, Root({{8, 101, 0}}, 5, 1)) == "helloabc", "a root over an INTERNAL node over two DATA fragments");

	auto refused = [&](const std::string &root, const std::string &want, const std::string &what,
					   uint64_t max = 1 << 20) {
		const std::string e = ErrorOf([&]() { Read(p, root, max); });
		Check(e.find(want) != std::string::npos, what + ": " + e);
	};
	// A child claiming more than its parent, refused before it is fetched.
	p.pages[102] = TextPage(102, {Internal({{5, 100, 0}, {100000, 100, 1}})});
	p.fetched = 0;
	refused(Root({{8, 102, 0}}, 4, 1), "where its parent claims 8", "a child claiming more than its parent");
	Check(p.fetched == 2, "... is refused before it is read (2 pages fetched: the node and its first child)");
	refused(Root({{5, 100, 0}, {10, 100, 0}}), "referenced twice", "a fragment referenced twice");
	p.pages[103] = TextPage(103, {Data("x")});
	Put(p.pages[103], 22, 0xFFFF, 2);
	refused(Root({{1, 103, 5000}}), "more than it holds", "a slot count past the page");
	p.pages[104] = TextPage(104, {Data("hello")}, OBJECT + 1);
	refused(Root({{5, 104, 0}}), "not the value's unit", "a page of another allocation unit");
	refused(Root({{5, 100, 0}, {8, 100, 1}}), "more than the 4", "a value past the caller's bound", 4);
	Check(Read(p, Root({{5, 100, 0}, {7, 100, 1}})) == "helloab",
		  "the last DATA fragment holding more than its link claims: its first claimed bytes (a truncating .WRITE)");
	refused(Root({{3, 100, 0}, {6, 100, 1}}), "holds 5 bytes where its parent claims 3",
			"... but not a fragment before the last");
	refused(Root({{6, 100, 0}}), "holds 5 bytes where its parent claims 6",
			"a leaf whose size is not its parent's claim");
	refused(Root({{5, 100, 0}}, 2), "kind 2", "a row-overflow pointer, to the catalog reader");
	refused(Root({{5, 100, 0}, {5, 100, 1}}), "do not ascend", "root links whose sizes do not ascend");
	refused(Root({{5, 100, 2}}), "past the page's 2", "a slot past the page's slots");

	std::cout << "Off-row forms and the walk (spec 085 PR 3, E21)" << std::endl;
	auto form_of = [](const std::string &b) {
		return ParseOffRowPointer(reinterpret_cast<const uint8_t *>(b.data()), b.size(), "v").form;
	};
	for (char first : {'\x00', '\x02', '\x04', '\x05'}) {
		std::string pointer = RootPointer(100, 0);
		pointer[0] = first;
		Check(form_of(pointer) == OffRowForm::RootPointer,
			  "16 bytes: a root pointer, whatever its first byte (" + std::to_string(int(first)) + ")");
	}
	Check(form_of(Overflow(5, 100, 0)) == OffRowForm::RowOverflow, "24 bytes of kind 2: row-overflow");
	Check(form_of(Root({{5, 100, 0}})) == OffRowForm::InRowRoot, "12 + 12n bytes of kind 4: an in-row root");
	const std::string odd(20, '\x07');
	Check(ErrorOf([&]() { form_of(odd); }).find("none of the forms") != std::string::npos,
		  "any other shape is refused by name");

	p.pages[200] = TextPage(200, {SmallRoot("tiny"), LargeRoot({{5, 100, 0}, {8, 100, 1}}, 0),
								  Fragment(8, std::string(10, '\0')), Data("x"), LargeRoot({{8, 101, 0}}, 1)});
	{
		MapSink s;
		const LobValue v = Walk(p, RootPointer(200, 0), LobWalk::Assemble, s);
		Check(s.data == "tiny" && v.length == 4 && !v.is_null, "a root pointer to a SMALL_ROOT");
	}
	{
		MapSink s;
		const LobValue v = Walk(p, RootPointer(200, 1), LobWalk::Assemble, s);
		Check(s.data == "helloabc" && v.length == 8, "a root pointer to a LARGE_ROOT over two DATA fragments");
	}
	{
		MapSink s;
		const LobValue v = Walk(p, RootPointer(200, 2), LobWalk::Assemble, s);
		Check(v.is_null && v.length == 0 && s.data.empty(), "a NULL root is a NULL value");
	}
	{
		MapSink s;
		const LobValue v = Walk(p, RootPointer(200, 4), LobWalk::Assemble, s);
		Check(s.data == "helloabc" && v.length == 8, "a LARGE_ROOT of level 1 over an INTERNAL node");
	}
	p.pages[300] = TextPage(300, {Data("hello")}, OBJECT + 2);
	{
		MapSink s;
		const LobValue v = Walk(p, Overflow(5, 300, 0), LobWalk::Assemble, s);
		Check(s.data == "hello" && v.length == 5, "row-overflow: one DATA fragment on the overflow unit");
	}
	auto walk_refused = [&](const std::string &pointer, const std::string &want, const std::string &what,
							uint64_t lob = UNIT) {
		MapSink s;
		const std::string e = ErrorOf([&]() { Walk(p, pointer, LobWalk::Assemble, s, lob); });
		Check(e.find(want) != std::string::npos, what + ": " + e);
	};
	walk_refused(Overflow(5, 100, 0), "not the value's unit", "row-overflow naming the LOB unit");
	walk_refused(RootPointer(300, 0), "not the value's unit", "a root pointer naming the overflow unit");
	walk_refused(RootPointer(200, 3), "is a root record of fragment type 3", "a root pointer to a DATA fragment");
	walk_refused(Root({{8, 101, 0}}), "where its parent's level 0 names data",
				 "an in-row root of level 0 over an INTERNAL node");
	p.pages[201] = TextPage(201, {InternalAt({{8, 101, 0}}, 1)});
	Check(Read(p, Root({{8, 201, 0}}, 4, 2)) == "helloabc", "a root of level 2 over INTERNAL nodes of levels 1 and 0");
	walk_refused(Root({{8, 101, 0}}, 4, 2), "is at level 0 where its parent names level 1",
				 "an INTERNAL node at another level than its parent names");
	p.pages[202] = TextPage(202, {Fragment(8, std::string(10, '\0'))});
	walk_refused(Root({{1, 202, 0}}), "fragment type 8", "a NULL fragment below a root");
	walk_refused(Root({{5, 100, 0}}), "has no LOB_DATA unit", "a rowset without a LOB unit", 0);
	walk_refused(Overflow(6, 300, 0), "holds 5 bytes where its parent claims 6",
				 "a row-overflow fragment of another length than the pointer's");
	p.pages[203] = TextPage(203, {LargeRoot({{5, 100, 0}, {9, 100, 1}}, 0)});
	walk_refused(RootPointer(203, 0), "holds 3 bytes where its parent claims 4", "a LARGE_ROOT whose links over-claim");
	std::string short_small = SmallRoot("tiny");
	short_small[14] = 100;	// a size past the record
	p.pages[204] = TextPage(204, {short_small});
	{
		MapSink s;
		const std::string e = ErrorOf([&]() { Walk(p, RootPointer(204, 0), LobWalk::Assemble, s); });
		Check(!e.empty(), "... refused: " + e);
	}

	std::cout << "The bound, before anything past it is read" << std::endl;
	auto bounded = [&](const std::string &pointer, uint64_t max, size_t fetches, const std::string &what) {
		MapSink s;
		p.fetched = 0;
		const std::string e = ErrorOf([&]() { Walk(p, pointer, LobWalk::Assemble, s, UNIT, OVERFLOW_UNIT, max); });
		Check(e.find("more than the " + std::to_string(max)) != std::string::npos && p.fetched == fetches,
			  what + " (" + std::to_string(p.fetched) + " page(s) read): " + e);
	};
	bounded(RootPointer(200, 0), 3, 1, "a SMALL_ROOT of 4 bytes past a bound of 3");
	bounded(RootPointer(200, 1), 7, 1, "a LARGE_ROOT of 8 bytes past a bound of 7, its children unread");
	bounded(Overflow(5, 300, 0), 4, 0, "row-overflow of 5 bytes past a bound of 4, nothing read");
	bounded(Root({{5, 100, 0}, {8, 100, 1}}), 7, 0, "an in-row root of 8 bytes past a bound of 7, nothing read");

	std::cout << "Map-only walks read no leaf" << std::endl;
	{
		MapSink s;
		p.fetched = 0;
		const LobValue v = Walk(p, Root({{5, 100, 0}, {8, 100, 1}}), LobWalk::MapOnly, s);
		Check(p.fetched == 0 && v.length == 8 && s.data.empty(), "an in-row root of level 0: no page read");
		Check(s.nodes.size() == 2 && s.nodes[0].offset == 0 && s.nodes[0].length == 5 && s.nodes[1].offset == 5 &&
				  s.nodes[1].length == 3 && s.nodes[1].page.page == 100 && s.nodes[1].slot == 1 &&
				  s.nodes[1].fragment_type == 3 && s.nodes[1].depth == 1,
			  "... its leaves listed with their places and ranges");
	}
	{
		MapSink s;
		p.fetched = 0;
		Walk(p, RootPointer(200, 1), LobWalk::MapOnly, s);
		Check(p.fetched == 1 && s.nodes.size() == 3 && s.nodes[0].depth == 0 && s.nodes[0].fragment_type == 5 &&
				  s.nodes[0].length == 8,
			  "a root pointer: its root record read, its leaves not");
	}
	{
		MapSink s;
		p.fetched = 0;
		Walk(p, RootPointer(200, 4), LobWalk::MapOnly, s);
		Check(p.fetched == 2 && s.nodes.size() == 4, "a level-1 root: the root and the INTERNAL node read");
	}
	{
		MapSink s;
		p.fetched = 0;
		Walk(p, Overflow(5, 300, 0), LobWalk::MapOnly, s);
		Check(p.fetched == 0 && s.nodes.size() == 1 && s.nodes[0].length == 5, "row-overflow: no page read");
	}
	{
		MapSink s;
		const std::string e = ErrorOf([&]() { Walk(p, Root({{5, 100, 0}, {10, 100, 0}}), LobWalk::MapOnly, s); });
		Check(e.find("referenced twice") != std::string::npos, "a leaf listed twice is refused unread: " + e);
	}
	{
		// More fragments than the linear lookup holds: the set takes over, and a repeat is still found.
		std::vector<Link> links;
		for (uint16_t i = 0; i < 40; i++) {
			links.push_back({uint64_t(i + 1), 400, i});
		}
		links.push_back({41, 400, 3});
		std::vector<std::string> ones(40, Data("z"));
		p.pages[400] = TextPage(400, ones);
		p.pages[401] = TextPage(401, {InternalAt(links, 0)});
		MapSink s;
		const std::string e = ErrorOf([&]() { Walk(p, Root({{41, 401, 0}}, 4, 1), LobWalk::MapOnly, s); });
		Check(e.find("referenced twice") != std::string::npos, "... past 32 fragments too: " + e);
		links.pop_back();
		p.pages[402] = TextPage(402, {InternalAt(links, 0)});
		MapSink t;
		const LobValue v = Walk(p, Root({{40, 402, 0}}, 4, 1), LobWalk::Assemble, t);
		Check(v.length == 40 && t.data == std::string(40, 'z'), "40 distinct fragments assemble");
	}

	std::cout << (failures ? "FAILED" : "PASSED") << " (" << failures << " failure(s))" << std::endl;
	return failures ? 1 : 0;
}
