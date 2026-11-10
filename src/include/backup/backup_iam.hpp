//===----------------------------------------------------------------------===//
//                         DuckDB MSSQL Extension
//
// backup/backup_iam.hpp
//
// Spec 085 PR 2b step 4 (task 11a, DQ-40): an allocation unit's pages, from its IAM chain. An IAM
// page (type 10) holds two records: slot 0 the header -- the chain's sequence number u32 +4, the
// start page of the GAM interval it maps +40 (a 6-byte pointer), eight single-page slots +46 (6
// bytes each: the AU's pages in mixed extents) -- and slot 1 the extent bitmap, its fixed part
// 7,988 bytes (bit i, LSB first: extent i of the interval belongs to the AU). Read through the
// records, not page offsets: slot 1 lies at 190 or 192 on the samples, not 194. The chain follows the
// page header's next pointer; each page's header names the AU it maps (m_objId / m_indexId).
// Measured on the lane's 2017-2025 backups and the committed samples (2026-11-10). DuckDB-free (D15).
//===----------------------------------------------------------------------===//

#pragma once

#include "backup/backup_page.hpp"

#include <cstdint>
#include <vector>

namespace duckdb {
namespace mssql {
namespace backup {

static constexpr size_t IAM_SINGLE_PAGES = 8;

struct IamPage {
	uint32_t sequence = 0;
	PagePointer start;	// the first page of the GAM interval the page maps
	PagePointer single[IAM_SINGLE_PAGES];
	std::vector<uint8_t> extents;  // 7,988 bytes, a bit per extent of the interval
};

//! The IAM page `page` (type 10) read.
IamPage ReadIamPage(const Page &page);

//! A run of extents of one file: [first, first + count).
struct IamExtentRun {
	uint16_t file = 0;
	uint32_t first = 0;
	uint32_t count = 0;
};

//! An allocation unit's pages, as its IAM chain lists them: the pages of its mixed extents (the
//! single-page slots, of the first IAM page only), and its extents, in chain order -- the order
//! SQL Server appended the IAM pages in, not sorted (with two files: f1 / interval 0, f2 / interval 0,
//! f1 / interval 1, ...; Randal, "IAM pages, IAM chains, and allocation units").
struct AuPages {
	std::vector<PagePointer> iam_pages;
	std::vector<PagePointer> single_pages;
	std::vector<IamExtentRun> extents;

	//! Every page these list, the single pages and every page of every extent.
	uint64_t PageCount() const;
};

//! The chain of the AU `au` from its first IAM page `first_iam` (sysallocunits.pgfirstiam), every page
//! read live through `store`: each of type 10 and naming `au` in its header, its previous-page
//! pointer the page before it (null for the first), its sequence number above the one before (not
//! from 0 nor gapless: pages that mapped an emptied, removed file leave the chain unrenumbered, E19),
//! single-page slots on the first page only, each start page the first page of a GAM interval and no
//! interval mapped twice, no single page twice nor inside one of the AU's extents. FormatError naming
//! the page otherwise. A null `first_iam` (an AU with no pages) is empty.
AuPages ReadIamChain(const PageStore &store, PagePointer first_iam, uint64_t au);

}  // namespace backup
}  // namespace mssql
}  // namespace duckdb
