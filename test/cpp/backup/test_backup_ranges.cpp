// Spec 085 task 2, D3a: Coalesce, the planning of range requests. DuckDB-free.
//
// Every non-empty wanted range is served by exactly one request that contains it; requests merge
// across a gap up to max_gap and up to max_length, never across files; overlaps and duplicates
// cost no extra request; a range longer than max_length stays one request.

#include <iostream>
#include <string>
#include <vector>

#include "backup/backup_ranges.hpp"

using namespace duckdb::mssql::backup;

namespace {

int failures = 0;

void Check(bool ok, const std::string &what) {
	std::cout << (ok ? "  ok   " : "  FAIL ") << what << std::endl;
	if (!ok) {
		failures++;
	}
}

ByteRange R(uint32_t family, uint64_t offset, uint64_t length) {
	ByteRange r;
	r.family = family;
	r.offset = offset;
	r.length = length;
	return r;
}

//! Every non-empty wanted range is in exactly one request, and that request contains it.
bool Covers(const std::vector<ByteRange> &wanted, const CoalescePlan &plan) {
	std::vector<int> seen(wanted.size(), 0);
	for (const auto &request : plan.requests) {
		for (size_t m : request.members) {
			const auto &w = wanted[m];
			if (w.family != request.range.family || w.offset < request.range.offset || w.End() > request.range.End()) {
				return false;
			}
			seen[m]++;
		}
	}
	for (size_t m : plan.empty) {
		seen[m]++;
	}
	for (size_t i = 0; i < wanted.size(); i++) {
		if (seen[i] != 1) {
			return false;
		}
	}
	return true;
}

}  // namespace

int main() {
	std::cout << "Coalesce (spec 085 D3a)" << std::endl;
	CoalescePolicy policy;
	policy.max_gap = 100;
	policy.max_length = 1000;

	{
		// Out of order, adjacent and within the gap: one request.
		std::vector<ByteRange> wanted = {R(0, 300, 100), R(0, 0, 100), R(0, 100, 100), R(0, 250, 40)};
		auto plan = Coalesce(wanted, policy);
		Check(plan.requests.size() == 1 && plan.requests[0].range.offset == 0 && plan.requests[0].range.length == 400,
			  "adjacent and near ranges become one request [0, 400)");
		Check(Covers(wanted, plan), "every range is served once, inside its request");
	}
	{
		std::vector<ByteRange> wanted = {R(0, 0, 100), R(0, 201, 100)};
		auto plan = Coalesce(wanted, policy);
		Check(plan.requests.size() == 2 && Covers(wanted, plan), "a gap of 101 > max_gap splits");
		wanted = {R(0, 0, 100), R(0, 200, 100)};
		plan = Coalesce(wanted, policy);
		Check(plan.requests.size() == 1 && plan.requests[0].range.length == 300, "a gap of exactly max_gap merges");
	}
	{
		std::vector<ByteRange> wanted = {R(0, 0, 600), R(0, 600, 400), R(0, 1000, 1)};
		auto plan = Coalesce(wanted, policy);
		Check(plan.requests.size() == 2 && plan.requests[0].range.length == 1000 && Covers(wanted, plan),
			  "max_length caps a merge at exactly 1000 bytes");
	}
	{
		std::vector<ByteRange> wanted = {R(0, 0, 5000), R(0, 10, 20), R(0, 4990, 10), R(0, 5010, 10)};
		auto plan = Coalesce(wanted, policy);
		Check(plan.requests.size() == 2 && plan.requests[0].range.length == 5000 && Covers(wanted, plan),
			  "a range past max_length stays one request; ranges inside it cost nothing; a later one starts anew");
	}
	{
		std::vector<ByteRange> wanted = {R(1, 0, 100), R(0, 0, 100), R(1, 100, 100), R(0, 50, 100), R(0, 50, 100)};
		auto plan = Coalesce(wanted, policy);
		Check(plan.requests.size() == 2 && plan.requests[0].range.family == 0 && plan.requests[0].range.length == 150 &&
				  plan.requests[1].range.family == 1 && plan.requests[1].range.length == 200,
			  "never across files; overlaps and duplicates in one request; files in order");
		Check(Covers(wanted, plan), "duplicates are each served once");
	}
	{
		std::vector<ByteRange> wanted = {R(0, 10, 0), R(0, 0, 8), R(0, 1u << 20, 0)};
		auto plan = Coalesce(wanted, policy);
		Check(plan.requests.size() == 1 && plan.empty.size() == 2 && Covers(wanted, plan),
			  "empty ranges are served without a request");
		Check(Coalesce({}, policy).requests.empty(), "nothing wanted, nothing requested");
	}
	{
		// A corrupt length whose end overflows must not hide inside another request.
		std::vector<ByteRange> wanted = {R(0, 100, 50), R(0, 120, UINT64_MAX - 60)};
		auto plan = Coalesce(wanted, policy);
		Check(plan.requests.size() == 2 && plan.requests[1].range.Wraps() && plan.requests[1].members.size() == 1,
			  "a range whose end wraps is a request of its own, merged with nothing");
		std::vector<ByteRange> first = {R(0, 10, UINT64_MAX), R(0, 20, 5)};
		plan = Coalesce(first, policy);
		Check(plan.requests.size() == 2, "nor does a wrapping range absorb the next one");
	}
	{
		std::vector<ByteRange> wanted = {R(0, 0, 100), R(0, 0, 5000)};
		auto plan = Coalesce(wanted, policy);
		Check(plan.requests.size() == 1 && plan.requests[0].range.length == 5000 && Covers(wanted, plan),
			  "at one offset the longer range is requested and the shorter falls inside it");
	}

	std::cout << (failures ? "FAILED: " + std::to_string(failures) + " check(s)" : "all checks passed") << std::endl;
	return failures ? 1 : 0;
}
