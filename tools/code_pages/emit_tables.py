#!/usr/bin/env python3
"""Spec 091: turn the server's answers (tools/code_pages/data/, from generate.py) into
src/codec/code_page/code_page_tables.cpp.

    python3 tools/code_pages/emit_tables.py        # make code-pages-tables

The emitter checks every rule the decode kernel relies on and refuses the data when one
fails, so a server that behaved differently is caught here, not as a wrong string later:

  - bytes 0x00-0x7F read as themselves on every page (the kernel's ASCII fast path);
  - a single-byte page reads every byte as exactly one BMP code point, no surrogate;
  - a double-byte page: a lead byte alone reads as nothing (the server drops it); any
    other byte as one code point; a lead byte followed by 0x01-0xFF as one code point;
    a lead byte followed by 0x00 as one code point and then U+0000 (the 0x00 is not
    consumed); a pair whose first byte is not a lead byte as the two bytes read alone;
  - the second collation probed per page (cpNNN.alt.psv) reads every byte and pair
    exactly as the first: the byte reading belongs to the code page, not the collation;
  - the strings probed per double-byte page (cpNNN.strings.psv, 3-12 bytes each) read as
    the rules above say, applied left to right;
  - the code pages the collations name (other than 0, Unicode-only, and 65001, UTF-8)
    are exactly the pages probed; every name is a plain identifier (it is written into
    a C string literal); every version fits the uint8_t it is stored in; the page count
    fits CodePageTable::index (uint8_t).

The strings files are committed. The alt files are not, so a fresh checkout has none to
compare: the note says so and quotes the generation run's own comparison from
provenance.txt. A file that is missing is reported, and the tables are emitted from the
primary data alone.
"""
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]
DATA = REPO / "tools/code_pages/data"
OUT = REPO / "src/codec/code_page/code_page_tables.cpp"

# Code pages with a meaning of their own, never probed: Unicode-only and UTF-8.
UNPROBED = {0, 65001}
PAGE_FILE = re.compile(r"cp(\d+)\.psv")
NAME = re.compile(r"[A-Za-z0-9_]+")
HEX = re.compile(r"(?:[0-9A-F]{2})*")


def fail(msg):
    sys.exit(f"emit_tables: {msg}")


def note(msg):
    print(f"emit_tables: {msg}", file=sys.stderr)


def code_points(hex_utf16):
    """UTF-16LE hex -> list of code points; the generator's outputs are BMP-only."""
    if not HEX.fullmatch(hex_utf16) or len(hex_utf16) % 4:
        fail(f"not UTF-16LE hex: {hex_utf16!r}")
    raw = bytes.fromhex(hex_utf16)
    units = [raw[i] | raw[i + 1] << 8 for i in range(0, len(raw), 2)]
    for u in units:
        if 0xD800 <= u <= 0xDFFF:
            fail(f"surrogate U+{u:04X} in {hex_utf16}")
    return units


def utf8_packed(cp):
    """UTF-8 of a BMP code point packed little-endian in 24 bits, the length on top."""
    b = chr(cp).encode("utf-8")
    v = 0
    for i, x in enumerate(b):
        v |= x << (8 * i)
    return v | (len(b) << 24)


def load_page(path):
    rows = {}
    for line in path.read_text().splitlines():
        i, _, o = line.partition("|")
        rows[i] = code_points(o)
    return rows


def build_page(cp, rows):
    single = {}
    for b in range(256):
        key = f"{b:02X}"
        if key not in rows:
            fail(f"cp{cp}: byte {key} missing")
        single[b] = rows[key]
        if b < 0x80 and single[b] != [b]:
            fail(f"cp{cp}: byte {key} reads as {single[b]}, not itself")
    leads = [b for b in range(0x80, 0x100) if single[b] == []]
    for b in range(256):
        if b not in leads and len(single[b]) != 1:
            fail(f"cp{cp}: byte {b:02X} reads as {len(single[b])} code points")
    pairs = None
    if leads:
        pairs = []
        for lead in range(0x80, 0x100):
            for trail in range(256):
                key = f"{lead:02X}{trail:02X}"
                if key not in rows:
                    fail(f"cp{cp}: pair {key} missing")
                got = rows[key]
                if lead not in leads:
                    want = single[lead] + single[trail] if trail not in leads else single[lead]
                    if got != want:
                        fail(f"cp{cp}: non-lead pair {key} reads as {got}, not {want}")
                    continue
                if trail == 0:
                    if len(got) != 2 or got[1] != 0:
                        fail(f"cp{cp}: {key} reads as {got}, not [replacement, 0]")
                    pairs.append(got[0])
                elif len(got) != 1:
                    fail(f"cp{cp}: pair {key} reads as {got}, not one code point")
                else:
                    pairs.append(got[0])
    return single, leads, pairs


def decode(page, data):
    """The rules build_page checked, applied left to right: what a string reads as."""
    single, leads, pairs = page
    out, i = [], 0
    while i < len(data):
        b = data[i]
        if b in leads:
            if i + 1 == len(data):
                break  # a lead byte that ends the string reads as nothing
            row = leads.index(b)
            out.append(pairs[row * 256 + data[i + 1]])
            i += 2 if data[i + 1] else 1  # a 0x00 after a lead byte reads on its own
            continue
        out.extend(single[b])
        i += 1
    return out


def check_alt(cp, rows):
    """True when cpNNN.alt.psv was checked, False when it is missing."""
    path = DATA / f"cp{cp}.alt.psv"
    if not path.exists():
        return False
    alt = load_page(path)
    if alt != rows:
        differ = sorted(k for k in set(alt) | set(rows) if alt.get(k) != rows.get(k))
        fail(f"{path.name}: the second collation reads {len(differ)} inputs differently from "
             f"cp{cp}.psv, first {differ[:8]}: the byte reading is not the code page's alone")
    return True


def check_strings(cp, page):
    """The strings of cpNNN.strings.psv checked, None when it is missing."""
    path = DATA / f"cp{cp}.strings.psv"
    if not path.exists():
        return None
    count = 0
    for line in path.read_text().splitlines():
        i, _, o = line.partition("|")
        if not HEX.fullmatch(i) or not 3 <= len(i) // 2 <= 12:
            fail(f"{path.name}: not a 3-12 byte hex string: {i!r}")
        want = code_points(o)
        got = decode(page, bytes.fromhex(i))
        if got != want:
            fail(f"{path.name}: {i} reads as {[hex(c) for c in want]} on the server, "
                 f"{[hex(c) for c in got]} by the rules")
        count += 1
    return count


def recorded_alt_check():
    """What provenance.txt says the generation run's second-collation check found, as a
    sentence for the note above (the .alt.psv probes themselves are not committed)."""
    path = DATA / "provenance.txt"
    for line in path.read_text().splitlines() if path.exists() else []:
        key, _, value = line.partition("|")
        if key == "second_collation":
            return f" provenance.txt records the generation run's comparison: {value}."
    return ""


def load_collations():
    rows = []
    for line in (DATA / "collations.psv").read_text().splitlines():
        name, cid, lcid, cp, ver = line.split("|")
        rows.append((name, int(cid), int(lcid), int(cp), int(ver)))
    for name, cid, lcid, cp, ver in rows:
        if not NAME.fullmatch(name):
            fail(f"collation name {name!r} is not a plain identifier (it is emitted as a C string literal)")
        if not 0 <= ver <= 0xFF:
            fail(f"{name}: version {ver} does not fit CollationRow::version (uint8_t)")
    if len({r[1] for r in rows}) != len(rows):
        fail("two collations share a CollationId")
    if len({r[0].lower() for r in rows}) != len(rows):
        fail("two collation names differ only in case")
    if len(rows) > 0xFFFF:
        fail("more collations than a uint16_t index holds")
    by_sort, by_lcid = {}, {}
    for name, cid, lcid, cp, ver in rows:
        sort_id = (cid & 0xFFFFFFFF) >> 24
        if sort_id:
            if by_sort.setdefault(sort_id, cp) != cp:
                fail(f"SortId {sort_id} maps to two code pages")
        else:
            key = (lcid, name.endswith("_UTF8"))
            if by_lcid.setdefault(key, cp) != cp:
                fail(f"LCID {lcid} utf8={key[1]} maps to two code pages")
    return rows, by_sort, by_lcid


def hex_rows(values, fmt, per_line):
    out = []
    for i in range(0, len(values), per_line):
        out.append("\t" + ", ".join(fmt.format(v) for v in values[i:i + per_line]) + ",")
    return "\n".join(out)


def main():
    # cpNNN.psv only: cpNNN.alt.psv and cpNNN.strings.psv are checks, not pages.
    page_files = sorted((int(m.group(1)), p) for p in DATA.glob("cp*.psv")
                        for m in [PAGE_FILE.fullmatch(p.name)] if m)
    pages, raw = {}, {}
    for cp, path in page_files:
        raw[cp] = load_page(path)
        pages[cp] = build_page(cp, raw[cp])
    if len(pages) > 0x100:
        fail(f"{len(pages)} code pages do not fit CodePageTable::index (uint8_t)")
    collations, by_sort, by_lcid = load_collations()
    named = {r[3] for r in collations} - UNPROBED
    if named != set(pages):
        fail(f"the collations name code pages {sorted(named)}, the data has {sorted(pages)}: "
             f"unprobed {sorted(named - set(pages))}, unnamed {sorted(set(pages) - named)}")
    alts = sum(check_alt(cp, raw[cp]) for cp in pages)
    strings = 0
    missing = []
    for cp, page in pages.items():
        if page[1]:  # a double-byte page
            n = check_strings(cp, page)
            if n is None:
                missing.append(f"cp{cp}.strings.psv")
            else:
                strings += n
    if alts < len(pages):
        note(f"{len(pages) - alts} of {len(pages)} second-collation probes are not here, so not compared "
             "now: the probes are not committed, and `make code-pages-generate` (needs the test server) "
             "writes them." + recorded_alt_check())
    if missing:
        note(f"not probed, so not checked: {', '.join(missing)}. Run `make code-pages-generate` (needs "
             "the test server) to probe them; the tables are emitted from the primary data alone.")

    out = []
    out.append("// GENERATED by tools/code_pages/emit_tables.py from tools/code_pages/data/ (spec 091).")
    out.append("// Do not edit: rerun `make code-pages-tables`. The data is the server's own reading of")
    out.append("// every byte; see tools/code_pages/generate.py for how it is asked.")
    out.append("// clang-format off")
    out.append('#include "codec/code_page/code_page_tables.hpp"')
    out.append("")
    out.append("namespace duckdb {\nnamespace mssql {\nnamespace codec {\nnamespace code_page {\nnamespace tables {")
    out.append("")
    out.append("namespace {")
    for cp, (single, leads, pairs) in pages.items():
        packed = [0 if b in leads else utf8_packed(single[b][0]) for b in range(256)]
        out.append(f"\n// cp{cp}: UTF-8 per byte, packed little-endian, length in the top octet"
                   + (" (0 for a lead byte)" if leads else "") + ".")
        out.append(f"const uint32_t SINGLE_{cp}[256] = {{\n{hex_rows(packed, '0x{:08X}', 8)}\n}};")
        units = [0 if b in leads else single[b][0] for b in range(256)]
        out.append(f"// cp{cp}: the UTF-16 code unit of each byte read alone" + (" (0 for a lead byte)" if leads else "") + ".")
        out.append(f"const uint16_t SINGLE16_{cp}[256] = {{\n{hex_rows(units, '0x{:04X}', 12)}\n}};")
        if leads:
            row_of = [0] * 256
            for r, lead in enumerate(leads):
                row_of[lead] = r + 1
            out.append(f"// cp{cp}: 1 + the pair-table row of each lead byte, 0 for any other byte.")
            out.append(f"const uint8_t LEAD_{cp}[256] = {{\n{hex_rows(row_of, '{:3d}', 16)}\n}};")
            rows = pairs  # one row of 256 per lead byte, in lead order
            out.append(f"// cp{cp}: the code point of lead x trail, {len(leads)} rows of 256; entry 0 of a row is")
            out.append("// the replacement a lead byte followed by 0x00 reads as (the 0x00 is read on its own).")
            out.append(f"const uint16_t PAIRS_{cp}[{len(rows)}] = {{\n{hex_rows(rows, '0x{:04X}', 12)}\n}};")
    out.append("\n} // namespace\n")
    out.append("// {code page, index (the entry's position here), single, single16, lead_row, pairs}")
    out.append("const CodePageTable CODE_PAGES[] = {")
    for index, (cp, (single, leads, pairs)) in enumerate(pages.items()):
        if leads:
            out.append(f"\t{{{cp}, {index}, SINGLE_{cp}, SINGLE16_{cp}, LEAD_{cp}, PAIRS_{cp}}},")
        else:
            out.append(f"\t{{{cp}, {index}, SINGLE_{cp}, SINGLE16_{cp}, nullptr, nullptr}},")
    out.append("};")
    out.append(f"const size_t CODE_PAGE_COUNT = {len(pages)};")
    out.append("")
    out.append("// Every collation of sys.fn_helpcollations(), sorted by CollationId.")
    out.append("const CollationRow COLLATIONS[] = {")
    for name, cid, lcid, cp, ver in sorted(collations, key=lambda r: r[1]):
        out.append(f'\t{{"{name}", {cid}, {lcid}, {cp}, {ver}}},')
    out.append("};")
    out.append(f"const size_t COLLATION_COUNT = {len(collations)};")
    by_id = sorted(collations, key=lambda r: r[1])
    by_name = sorted(range(len(by_id)), key=lambda i: by_id[i][0].lower())
    out.append("")
    out.append("// Indices into COLLATIONS sorted by the ASCII-lowercased name.")
    out.append(f"const uint16_t COLLATION_BY_NAME[] = {{\n{hex_rows(by_name, '{:4d}', 16)}\n}};")
    out.append("")
    out.append("// SortId (CollationId >> 24, the fifth wire byte) -> code page; -1 where no SQL collation has it.")
    sort_table = [by_sort.get(s, -1) for s in range(256)]
    out.append(f"const int32_t SORT_ID_CODE_PAGE[256] = {{\n{hex_rows(sort_table, '{:5d}', 12)}\n}};")
    out.append("")
    out.append("// (LCID, UTF-8 flag) -> code page for the collations whose SortId is 0, sorted.")
    out.append("const LcidCodePage LCID_CODE_PAGES[] = {")
    for (lcid, utf8), cp in sorted(by_lcid.items()):
        out.append(f"\t{{{lcid}, {'true' if utf8 else 'false'}, {cp}}},")
    out.append("};")
    out.append(f"const size_t LCID_CODE_PAGE_COUNT = {len(by_lcid)};")
    out.append("")
    out.append("} // namespace tables\n} // namespace code_page\n} // namespace codec\n} // namespace mssql\n} // namespace duckdb")
    out.append("// clang-format on")
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text("\n".join(out) + "\n")
    print(f"wrote {OUT.relative_to(REPO)}: {len(pages)} code pages, {len(collations)} collations, "
          f"{len(by_sort)} SortIds, {len(by_lcid)} LCID keys; checked {alts} second collations, "
          f"{strings} double-byte strings")


if __name__ == "__main__":
    main()
