// Spec 085 PR 3 S2: the plain `_mssql_lob` map -- written and read back, its entries built from a
// map-only walk, and every malformed map refused by name before it is trusted: another format, no
// entries, a form or flag the format lacks, counts past the bytes, entries out of order, trailing bytes.

#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "backup/backup_lob_map.hpp"

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

LobNode Node(uint32_t page, uint16_t slot, uint32_t offset, uint32_t length, uint8_t depth, uint8_t type) {
	LobNode n;
	n.page.page = page;
	n.page.file = 1;
	n.slot = slot;
	n.offset = offset;
	n.length = length;
	n.depth = depth;
	n.fragment_type = type;
	return n;
}

bool SameNodes(const std::vector<LobNode> &a, const std::vector<LobNode> &b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (size_t i = 0; i < a.size(); i++) {
		if (a[i].page.page != b[i].page.page || a[i].page.file != b[i].page.file || a[i].slot != b[i].slot ||
			a[i].offset != b[i].offset || a[i].length != b[i].length || a[i].depth != b[i].depth ||
			a[i].fragment_type != b[i].fragment_type) {
			return false;
		}
	}
	return true;
}

std::vector<LobMapEntry> Sample() {
	std::vector<LobMapEntry> entries(3);
	entries[0].column_id = 2;
	entries[0].form = OffRowForm::RowOverflow;
	entries[0].pointer = std::string(24, '\x02');
	entries[0].length = 8000;
	entries[0].nodes = {Node(900, 3, 0, 8000, 1, 3)};
	entries[1].column_id = 5;
	entries[1].form = OffRowForm::RootPointer;
	entries[1].is_null = true;
	entries[1].pointer = std::string(16, '\0');
	entries[1].nodes = {Node(901, 0, 0, 0, 0, 8)};
	entries[2].column_id = 7;
	entries[2].form = OffRowForm::InRowRoot;
	entries[2].pointer = std::string(36, '\x04');
	entries[2].length = 9000;
	entries[2].nodes = {Node(902, 1, 0, 8040, 1, 3), Node(903, 4, 8040, 960, 1, 3)};
	return entries;
}

}  // namespace

int main() {
	std::cout << "The off-row map, _mssql_lob (spec 085 PR 3 S2)" << std::endl;
	const std::vector<LobMapEntry> entries = Sample();
	std::string map;
	AppendLobMap(entries, "the test row", map);
	Check(map.size() == 3 + (8 + 24 + 8 + 18) + (8 + 16 + 8 + 18) + (8 + 36 + 8 + 36),
		  "its size is the format's: " + std::to_string(map.size()) + " bytes");
	Check(static_cast<uint8_t>(map[0]) == LOB_MAP_FORMAT, "format byte first");
	const std::vector<LobMapEntry> back =
		ParseLobMap(reinterpret_cast<const uint8_t *>(map.data()), map.size(), "the test row");
	bool same = back.size() == entries.size();
	for (size_t i = 0; same && i < back.size(); i++) {
		same = back[i].column_id == entries[i].column_id && back[i].form == entries[i].form &&
			   back[i].is_null == entries[i].is_null && back[i].pointer == entries[i].pointer &&
			   back[i].length == entries[i].length && SameNodes(back[i].nodes, entries[i].nodes);
	}
	Check(same, "read back entry for entry, node for node");

	auto refused = [&](std::string bytes, const std::string &want, const std::string &what) {
		const std::string e = ErrorOf(
			[&]() { ParseLobMap(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(), "the test row"); });
		Check(e.find(want) != std::string::npos, what + ": " + e);
	};
	std::string other = map;
	other[0] = 2;
	refused(other, "is of format 2", "another format");
	refused(std::string("\x01\x00\x00", 3), "has no entries", "no entries");
	std::string bad_form = map;
	bad_form[3 + 4] = 9;
	refused(bad_form, "of form 9", "a form the format lacks");
	std::string bad_flags = map;
	bad_flags[3 + 5] = 2;
	refused(bad_flags, "with flags 2", "a flag the format lacks");
	std::string many_nodes = map;
	const size_t node_count_at = 3 + 8 + 24 + 4;
	many_nodes[node_count_at + 3] = '\x7f';
	refused(many_nodes, "nodes past its bytes", "a node count past the bytes, refused before it is reserved");
	refused(map.substr(0, map.size() - 1), "", "a map cut short");
	Check(!ErrorOf([&]() { ParseLobMap(reinterpret_cast<const uint8_t *>(map.data()), map.size() - 1, "r"); }).empty(),
		  "... is refused");
	refused(map + "x", "1 bytes past its entries", "trailing bytes");
	std::vector<LobMapEntry> unordered = entries;
	std::swap(unordered[0], unordered[1]);
	std::string out;
	Check(
		ErrorOf([&]() { AppendLobMap(unordered, "the test row", out); }).find("not in ascending") != std::string::npos,
		"entries out of order are not written");
	Check(ErrorOf([&]() { AppendLobMap({}, "the test row", out); }).find("has 0 entries") != std::string::npos,
		  "an empty map is not written (a row with none is NULL)");

	std::cout << "An entry from a map-only walk" << std::endl;
	{
		// An in-row root of two links over DATA fragments, walked without reading a page.
		std::string root(1, '\x04');
		root.append(11, '\0');
		auto link = [&](uint32_t cumulative, uint32_t page, uint16_t slot) {
			for (int i = 0; i < 4; i++)
				root.push_back(static_cast<char>(cumulative >> (8 * i)));
			for (int i = 0; i < 4; i++)
				root.push_back(static_cast<char>(page >> (8 * i)));
			root.push_back(1);
			root.push_back(0);
			root.push_back(static_cast<char>(slot));
			root.push_back(static_cast<char>(slot >> 8));
		};
		link(8040, 950, 0);
		link(9000, 951, 2);
		size_t fetched = 0;
		LobPageFetch fetch = [&](const PagePointer &, Page &) { fetched++; };
		const OffRowPointer pointer =
			ParseOffRowPointer(reinterpret_cast<const uint8_t *>(root.data()), root.size(), "the test value");
		LobUnits units;
		units.lob = 1;
		LobNodeList list;
		const LobValue v = WalkLob(fetch, pointer, units, LobWalk::MapOnly, 1u << 20, "the test value", list);
		LobMapEntry e;
		e.column_id = 3;
		e.form = pointer.form;
		e.pointer = root;
		e.length = static_cast<uint32_t>(v.length);
		e.nodes = list.nodes;
		std::string m;
		AppendLobMap({e}, "the test row", m);
		const auto parsed = ParseLobMap(reinterpret_cast<const uint8_t *>(m.data()), m.size(), "the test row");
		Check(fetched == 0 && parsed.size() == 1 && parsed[0].length == 9000 && parsed[0].nodes.size() == 2 &&
				  parsed[0].nodes[1].page.page == 951 && parsed[0].nodes[1].slot == 2 &&
				  parsed[0].nodes[1].offset == 8040 && parsed[0].nodes[1].length == 960,
			  "two leaves, their places and ranges, no page read");
	}

	std::cout << (failures ? "FAILED" : "PASSED") << " (" << failures << " failure(s))" << std::endl;
	return failures ? 1 : 0;
}
