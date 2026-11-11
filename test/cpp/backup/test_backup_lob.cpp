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

std::string Root(const std::vector<Link> &links, uint8_t kind = 4) {
	std::string r;
	r.push_back(static_cast<char>(kind));
	r.append(11, '\0');
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
	Check(Read(p, Root({{8, 101, 0}}, 5)) == "helloabc", "a root over an INTERNAL node over two DATA fragments");

	auto refused = [&](const std::string &root, const std::string &want, const std::string &what,
					   uint64_t max = 1 << 20) {
		const std::string e = ErrorOf([&]() { Read(p, root, max); });
		Check(e.find(want) != std::string::npos, what + ": " + e);
	};
	// A child claiming more than its parent, refused before it is fetched.
	p.pages[102] = TextPage(102, {Internal({{5, 100, 0}, {100000, 100, 1}})});
	p.fetched = 0;
	refused(Root({{8, 102, 0}}), "where its parent claims 8", "a child claiming more than its parent");
	Check(p.fetched == 2, "... is refused before it is read (2 pages fetched: the node and its first child)");
	refused(Root({{5, 100, 0}, {10, 100, 0}}), "referenced twice", "a fragment referenced twice");
	p.pages[103] = TextPage(103, {Data("x")});
	Put(p.pages[103], 22, 0xFFFF, 2);
	refused(Root({{1, 103, 5000}}), "more than it holds", "a slot count past the page");
	p.pages[104] = TextPage(104, {Data("hello")}, OBJECT + 1);
	refused(Root({{5, 104, 0}}), "not the value's LOB unit", "a page of another allocation unit");
	refused(Root({{5, 100, 0}, {8, 100, 1}}), "more than the 4", "a value past the caller's bound", 4);
	refused(Root({{6, 100, 0}}), "holds 5 bytes where its parent claims 6",
			"a leaf whose size is not its parent's claim");
	refused(Root({{5, 100, 0}}, 2), "kind 2", "a row-overflow or text pointer");
	refused(Root({{5, 100, 0}, {5, 100, 1}}), "do not ascend", "root links whose sizes do not ascend");
	refused(Root({{5, 100, 2}}), "past the page's 2", "a slot past the page's slots");

	std::cout << (failures ? "FAILED" : "PASSED") << " (" << failures << " failure(s))" << std::endl;
	return failures ? 1 : 0;
}
