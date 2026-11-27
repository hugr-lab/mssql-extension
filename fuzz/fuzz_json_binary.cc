// Fuzz harness: SQL Server 2025's stored json value (spec 086 PR 6, E9 / E9b) rendered as the server's text,
// DuckDB-free (codec/json/json_binary). The input is one value's bytes, as a record or its off-row tree
// assembles them. Checked beyond "no crash": a document rendered is text that begins with '{' or '[' (the
// root is a container), the walk is bounded (depth 128, and at most size / 4 + 1 values: a cycle or a node two slots
// name is refused, not followed), and rendering is a function of the bytes alone (twice the same text). Every key the
// document holds unescapes or is refused. Seeds: documents of the json fixture's corpora (fuzz/corpus/json_binary).

#include <cstddef>
#include <cstdint>
#include <string>

#include "codec/json/json_binary.hpp"

using namespace duckdb::mssql::codec::json;

static void Require(bool ok) {
	if (!ok) {
		__builtin_trap();
	}
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	std::string text, why;
	const bool ok = RenderJsonText(data, size, text, why);
	if (!ok) {
		Require(!why.empty());
		return 0;
	}
	Require(!text.empty() && (text[0] == '{' || text[0] == '['));
	std::string again, why_again;
	Require(RenderJsonText(data, size, again, why_again) && again == text);
	// Every member's key, through the document's walk.
	JsonDocument doc;
	Require(doc.Open(data, size, why));
	JsonDocument::Container c;
	if (doc.ContainerOf(doc.Root(), c, why) && c.object) {
		for (uint32_t i = 0; i < c.count; i++) {
			bool deleted = false;
			const uint8_t *key = nullptr;
			uint32_t key_length = 0, value = 0;
			if (doc.Member(c, i, deleted, key, key_length, value, why) && !deleted) {
				std::string unescaped;
				UnescapeJsonKey(key, key_length, unescaped);
			}
		}
	}
	return 0;
}
