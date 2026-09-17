#pragma once

//===----------------------------------------------------------------------===//
// Spec 077 W1: which index becomes a table's rowid.
//
// rowid used to mean "the primary key". It now means "a key this server
// guarantees to address one row AND that we can send back": the primary key
// if it is usable, else a usable unique index. Usable is decided here, in one
// pure function over the candidates the discovery query returns, so that:
//
//   - the rule is unit-testable without a server (this header is
//     self-contained: no DuckDB, no TDS, plain std::);
//   - a rejected candidate can be reported WITH its reason. A candidate
//     filtered out inside the SQL never comes back, and the W5b refusal has
//     to say "index X was rejected because its key column Y is nullable" —
//     so the query filters on nothing but is_unique (a non-unique index is
//     not a candidate under any reading, and a table can carry many) and
//     everything else — filtered, disabled, hypothetical, the key columns —
//     is decided here, where it can be named.
//
// The two client-side criteria the SQL cannot see are carried on the key
// column by the caller: `cast_required` (the read is lossy — sql_variant,
// hierarchyid, a CLR UDT — the column's own MSSQLColumnInfo flag) and the
// type name, from which the datetime exclusion is decided below.
//===----------------------------------------------------------------------===//

#include <cstdint>
#include <string>
#include <vector>

namespace duckdb {
namespace mssql {

struct RowIdKeyColumn {
	std::string name;
	int32_t column_id = 0;
	int32_t key_ordinal = 0;
	std::string type_name;	 // as sys.types / TYPE_NAME reports it
	int16_t max_length = 0;	 // bytes; -1 for the MAX forms
	uint8_t precision = 0;
	uint8_t scale = 0;
	std::string collation_name;
	bool is_nullable = false;
	bool is_identity = false;
	bool cast_required = false;	 // MSSQLColumnInfo::is_cast_required for this column
};

struct RowIdKeyCandidate {
	int32_t index_id = 0;
	std::string index_name;
	bool is_primary_key = false;
	bool is_unique = false;
	bool has_filter = false;
	bool is_disabled = false;
	bool is_hypothetical = false;
	std::vector<RowIdKeyColumn> columns;  // ordered by key_ordinal
};

enum class RowIdKeySource : uint8_t { NONE, PRIMARY_KEY, UNIQUE_INDEX };

struct RowIdKeyRejection {
	std::string index_name;
	bool is_primary_key = false;
	std::string reason;
	//! The key EXISTS and is unique, but a rowid literal cannot match one of its
	//! columns (#354, #358). The refusal words this differently from "no usable
	//! key": telling the user to add an index they already have is wrong.
	bool unmatchable = false;
};

struct RowIdKeyChoice {
	RowIdKeySource source = RowIdKeySource::NONE;
	int32_t index_id = 0;
	std::string index_name;
	std::vector<RowIdKeyColumn> columns;
	std::vector<RowIdKeyRejection> rejections;	// every candidate that was not usable, and why

	bool Found() const {
		return source != RowIdKeySource::NONE;
	}
};

namespace rowid_key_detail {

inline std::string Lower(std::string s) {
	for (auto &c : s) {
		if (c >= 'A' && c <= 'Z') {
			c = static_cast<char>(c - 'A' + 'a');
		}
	}
	return s;
}

//! The literal-cannot-match exclusion (issue #358). A `datetime` counts in
//! 1/300 s ticks, and since compatibility level 130 a datetime compared with a
//! datetime2 is converted more precisely than datetime2(7) can represent —
//! measured: no datetime2 literal at any precision equals such a column, so a
//! rowid built on it finds no row and the UPDATE reports success and changes
//! nothing. Lifted when #358 renders these keys as CAST(… AS DATETIME).
inline bool IsLiteralMismatchType(const std::string &type_name) {
	const std::string t = Lower(type_name);
	return t == "datetime" || t == "smalldatetime";
}

//! Why this candidate cannot address a row. `reason` is empty when it can;
//! `unmatchable` marks the two reasons that are about the key's TYPE rather
//! than its shape.
inline RowIdKeyRejection Unusable(const RowIdKeyCandidate &c) {
	RowIdKeyRejection r;
	r.index_name = c.index_name;
	r.is_primary_key = c.is_primary_key;
	if (!c.is_unique) {
		r.reason = "it is not unique";
	} else if (c.has_filter) {
		r.reason = "it is a filtered index, so rows outside its filter are unaddressable";
	} else if (c.is_disabled) {
		r.reason = "it is disabled";
	} else if (c.is_hypothetical) {
		r.reason = "it is hypothetical";
	} else if (c.columns.empty()) {
		r.reason = "it has no key columns";
	}
	if (!r.reason.empty()) {
		return r;
	}
	for (const auto &col : c.columns) {
		if (col.is_nullable) {
			r.reason = "its key column '" + col.name + "' is nullable, and NULL is neither unique nor addressable";
			return r;
		}
	}
	for (const auto &col : c.columns) {
		if (col.cast_required) {
			r.reason = "its key column '" + col.name + "' has type " + col.type_name +
					   ", which is read through a lossy CAST and cannot identify its row (see issue #354)";
			r.unmatchable = true;
			return r;
		}
		if (IsLiteralMismatchType(col.type_name)) {
			r.reason = "its key column '" + col.name + "' has type " + col.type_name +
					   ", which no rowid literal can match (see issue #358)";
			r.unmatchable = true;
			return r;
		}
	}
	return r;
}

//! Declared key width in bytes, for the tie-break. A MAX column is wider than
//! anything bounded.
inline int64_t KeyWidth(const RowIdKeyCandidate &c) {
	int64_t width = 0;
	for (const auto &col : c.columns) {
		width += col.max_length < 0 ? 1000000 : col.max_length;
	}
	return width;
}

inline bool SingleIdentityKey(const RowIdKeyCandidate &c) {
	return c.columns.size() == 1 && c.columns[0].is_identity;
}

//! Steps 2-5 of the spec's order, as a strict weak ordering over USABLE
//! candidates: identity single-column key first, then fewest key columns,
//! then narrowest, then lowest index_id — which always separates two rows.
inline bool Before(const RowIdKeyCandidate &a, const RowIdKeyCandidate &b) {
	const bool ai = SingleIdentityKey(a), bi = SingleIdentityKey(b);
	if (ai != bi) {
		return ai;
	}
	if (a.columns.size() != b.columns.size()) {
		return a.columns.size() < b.columns.size();
	}
	const int64_t aw = KeyWidth(a), bw = KeyWidth(b);
	if (aw != bw) {
		return aw < bw;
	}
	return a.index_id < b.index_id;
}

}  // namespace rowid_key_detail

//! The choice. Step 1: the primary key, if usable. Steps 2-5 order the usable
//! unique indexes; they order rather than filter, because duplicate indexes
//! are allowed and step 2 can yield more than one. Every candidate that is
//! not usable — the primary key included — is listed in `rejections` with its
//! reason, whether or not something else was chosen.
inline RowIdKeyChoice ChooseRowIdKey(const std::vector<RowIdKeyCandidate> &candidates) {
	RowIdKeyChoice choice;
	const RowIdKeyCandidate *usable_pk = nullptr;
	const RowIdKeyCandidate *best_unique = nullptr;
	for (const auto &c : candidates) {
		RowIdKeyRejection why = rowid_key_detail::Unusable(c);
		if (!why.reason.empty()) {
			choice.rejections.push_back(std::move(why));
			continue;
		}
		if (c.is_primary_key) {
			if (!usable_pk) {
				usable_pk = &c;
			}
		} else if (!best_unique || rowid_key_detail::Before(c, *best_unique)) {
			best_unique = &c;
		}
	}
	const RowIdKeyCandidate *best = usable_pk ? usable_pk : best_unique;
	if (best) {
		choice.source = best->is_primary_key ? RowIdKeySource::PRIMARY_KEY : RowIdKeySource::UNIQUE_INDEX;
		choice.index_id = best->index_id;
		choice.index_name = best->index_name;
		choice.columns = best->columns;
	}
	return choice;
}

//! One sentence per rejected candidate, for the W5b refusal.
inline std::string DescribeRejections(const RowIdKeyChoice &choice) {
	std::string out;
	for (const auto &r : choice.rejections) {
		if (!out.empty()) {
			out += "; ";
		}
		out += (r.is_primary_key ? "primary key '" : "unique index '") + r.index_name + "' was rejected because " +
			   r.reason;
	}
	return out;
}

}  // namespace mssql
}  // namespace duckdb
