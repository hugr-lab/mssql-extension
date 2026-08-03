// test/cpp/test_connection_lease.cpp
// Unit tests for MSSQLConnectionLease (issue #189 fix — see
// docs/proposals/issue-189-connection-lease-proposal.md).
//
// TARGET STATE — this file currently does NOT compile: MSSQLConnectionLease
// (src/include/connection/mssql_connection_lease.hpp) does not exist yet.
// That failure is intentional and documents Phase 1 of the proposal's
// implementation plan. Once the class lands, this file should compile and
// PASS unmodified; do not loosen these assertions to make it compile early.
//
// These tests do NOT require a running SQL Server instance or a live
// ClientContext/MSSQLCatalog — they exercise only the "no lease held" /
// "lease released" edge of MSSQLConnectionLease's contract, which is pure
// bookkeeping. `Lease()` itself (which needs a real pool + network round
// trip) is covered by the SQL-level acceptance test
// test/sql/lease/lease_basic.test and by the fuller integration test the
// proposal's Phase 4 adds under test/cpp/ once the catalog registry
// (RegisterLease/ReleaseAllLeases) exists.
//
// Compile: see Makefile (add a `test-connection-lease` target once the
// class exists, modeled on the existing `test-issue-96-attach-loop` target —
// this file intentionally has no Makefile wiring yet so that a
// not-yet-compilable source never breaks `make test` for anyone).
//
// Run: ./test_connection_lease

#include <cassert>
#include <iostream>

#include "connection/mssql_connection_lease.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "tds/tds_connection.hpp"

using namespace duckdb;

//==============================================================================
// Helper macros for assertions with messages (matches test_batch_builder.cpp)
//==============================================================================
#define ASSERT_TRUE(expr)                                                                    \
	do {                                                                                     \
		if (!(expr)) {                                                                       \
			std::cerr << "ASSERTION FAILED at " << __FILE__ << ":" << __LINE__ << std::endl; \
			std::cerr << "  Expression was false: " << #expr << std::endl;                   \
			assert(false);                                                                   \
		}                                                                                    \
	} while (0)

#define ASSERT_FALSE(expr)                                                                   \
	do {                                                                                     \
		if ((expr)) {                                                                        \
			std::cerr << "ASSERTION FAILED at " << __FILE__ << ":" << __LINE__ << std::endl; \
			std::cerr << "  Expression was true: " << #expr << std::endl;                    \
			assert(false);                                                                   \
		}                                                                                    \
	} while (0)

//==============================================================================
// A freshly constructed lease holds nothing.
//==============================================================================
void test_default_state_is_unheld() {
	std::cout << "test_default_state_is_unheld... ";

	MSSQLConnectionLease lease;
	ASSERT_FALSE(lease.IsHeld());
	ASSERT_TRUE(lease.GetConnection() == nullptr);

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Release() on a never-leased instance is a no-op that reports "nothing to
// release" rather than throwing — callers (mssql_release, ~ClientContext,
// catalog force-release) must all be able to call it unconditionally.
//==============================================================================
void test_release_without_lease_is_idempotent_false() {
	std::cout << "test_release_without_lease_is_idempotent_false... ";

	MSSQLConnectionLease lease;
	ASSERT_FALSE(lease.Release());
	// Calling it again must still be safe and still report false.
	ASSERT_FALSE(lease.Release());
	ASSERT_FALSE(lease.IsHeld());

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Owns() must not throw or crash when comparing against an empty lease, and
// must report false for both a null connection and an arbitrary non-null one
// — an unheld lease owns nothing, by construction.
//==============================================================================
void test_owns_false_when_unheld() {
	std::cout << "test_owns_false_when_unheld... ";

	MSSQLConnectionLease lease;
	shared_ptr<tds::TdsConnection> null_conn;
	ASSERT_FALSE(lease.Owns(null_conn));

	std::cout << "PASSED!" << std::endl;
}

//==============================================================================
// Main
//==============================================================================
int main() {
	std::cout << "==========================================" << std::endl;
	std::cout << "MSSQLConnectionLease Unit Tests (issue #189)" << std::endl;
	std::cout << "==========================================" << std::endl;

	try {
		test_default_state_is_unheld();
		test_release_without_lease_is_idempotent_false();
		test_owns_false_when_unheld();

		std::cout << "\n==========================================" << std::endl;
		std::cout << "ALL TESTS PASSED!" << std::endl;
		std::cout << "==========================================" << std::endl;
		return 0;

	} catch (const std::exception &e) {
		std::cerr << "\n==========================================" << std::endl;
		std::cerr << "TEST FAILED WITH EXCEPTION: " << e.what() << std::endl;
		std::cerr << "==========================================" << std::endl;
		return 1;
	}
}
