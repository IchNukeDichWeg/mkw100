#!/usr/bin/env python3
"""Inventory the instructions in MKWii PAL that can encode the 12-player limit.

Scans main.dol and StaticR.rel (placed in place at the recomp's REL load
address) for PowerPC instructions whose immediate is a player-count bound,
then groups the hits by function using MAP.txt and, when given, an extra
symbol list (MKW-SP's symbols.txt uses the same "0xADDR name" format).

Patterns, each a separate column so a site can be triaged by kind:
  cmp12  cmpwi/cmplwi rA, 12   loop bound or count check
  cmp11  cmpwi/cmplwi rA, 11   "last player" check, <= 11
  li12   li rD, 12             count passed to a call or loop init
  mulF0  mulli rD, rA, 0xF0    RaceConfig::Player stride

These are candidates, not confirmed sites: 12 is also a common size and
offset. The point is a complete, rankable list to triage against, so a
missed site is a scanner bug rather than a gap in someone's memory.

Usage:
  scripts/mkw100/limit_scan.py --dol Assets/main.dol --rel Assets/StaticR.rel \
      --map projects/mkwii/MAP.txt [--symbols ../refs/mkw-sp/symbols.txt] \
      [--out docs/mkw100/limit-sites.tsv]
"""
from __future__ import annotations

import argparse
import bisect
import struct
import sys
from collections import Counter, defaultdict
from pathlib import Path

REL_LOAD_ADDRESS = 0x805102E0  # projects/mkwii/recomp.yml inputs.rel.load_address
RACECONFIG_PLAYER_STRIDE = 0xF0  # runtime/src/hle/boot_race.cpp kPlayerSize

KINDS = ("cmp12", "cmp11", "li12", "mulF0")


def dol_text_sections(data: bytes) -> list[tuple[int, bytes]]:
    sections = []
    for i in range(7):
        offset, = struct.unpack_from(">I", data, 0x00 + i * 4)
        address, = struct.unpack_from(">I", data, 0x48 + i * 4)
        size, = struct.unpack_from(">I", data, 0x90 + i * 4)
        if size:
            sections.append((address, data[offset:offset + size]))
    return sections


def rel_text_sections(data: bytes, load_address: int) -> list[tuple[int, bytes]]:
    count, table = struct.unpack_from(">II", data, 0x0C)
    sections = []
    for i in range(count):
        offset, size = struct.unpack_from(">II", data, table + i * 8)
        if size and offset & 1:  # bit 0 marks an executable section
            start = offset & ~3
            sections.append((load_address + start, data[start:start + size]))
    return sections


def classify(word: int) -> str | None:
    opcode = word >> 26
    simm = word & 0xFFFF
    if simm & 0x8000:
        simm -= 0x10000
    ra = (word >> 16) & 0x1F
    if opcode in (10, 11):  # cmpli / cmpi
        if simm == 12:
            return "cmp12"
        if simm == 11:
            return "cmp11"
    elif opcode == 14 and ra == 0 and simm == 12:  # addi rD, 0, 12 == li
        return "li12"
    elif opcode == 7 and simm == RACECONFIG_PLAYER_STRIDE:  # mulli
        return "mulF0"
    return None


def load_symbols(paths: list[Path]) -> dict[int, str]:
    names: dict[int, str] = {}
    for path in paths:
        for line in path.read_text(errors="replace").splitlines():
            parts = line.split()
            if len(parts) < 2:
                continue
            try:
                address = int(parts[0], 16)
            except ValueError:
                continue
            name = parts[1]
            # MAP.txt names unnamed functions by their own address; a real name wins.
            if address not in names or names[address].startswith("0x"):
                names[address] = name
    return names


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dol", type=Path, required=True)
    parser.add_argument("--rel", type=Path, required=True)
    parser.add_argument("--map", type=Path, required=True, help="function starts, one 'ADDR name' per line")
    parser.add_argument("--symbols", type=Path, action="append", default=[], help="extra 'ADDR name' list")
    parser.add_argument("--out", type=Path, help="write per-function TSV here")
    args = parser.parse_args()

    function_starts = sorted(load_symbols([args.map]))
    names = load_symbols([args.map, *args.symbols])

    sections = dol_text_sections(args.dol.read_bytes()) + rel_text_sections(args.rel.read_bytes(), REL_LOAD_ADDRESS)
    hits: dict[int, Counter] = defaultdict(Counter)
    sites: dict[int, list[tuple[int, str]]] = defaultdict(list)
    totals: Counter = Counter()
    unowned = 0
    for base, blob in sections:
        for offset in range(0, len(blob) - 3, 4):
            kind = classify(struct.unpack_from(">I", blob, offset)[0])
            if kind is None:
                continue
            address = base + offset
            index = bisect.bisect_right(function_starts, address) - 1
            if index < 0:
                unowned += 1
                continue
            owner = function_starts[index]
            hits[owner][kind] += 1
            sites[owner].append((address, kind))
            totals[kind] += 1

    rows = sorted(hits.items(), key=lambda item: (-sum(item[1].values()), item[0]))
    named = sum(1 for owner, _ in rows if not names.get(owner, "0x").startswith("0x"))
    print(f"functions in map: {len(function_starts):,}")
    print(f"functions with candidate sites: {len(rows):,} ({named:,} named)")
    print("candidate instructions: " + ", ".join(f"{kind} {totals[kind]:,}" for kind in KINDS)
          + f", outside any function {unowned:,}")

    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        with args.out.open("w") as handle:
            handle.write("function\tname\t" + "\t".join(KINDS) + "\tsites\n")
            for owner, counter in rows:
                site_list = " ".join(f"{address:08X}:{kind}" for address, kind in sites[owner])
                handle.write(f"{owner:08X}\t{names.get(owner, '')}\t"
                             + "\t".join(str(counter[kind]) for kind in KINDS) + f"\t{site_list}\n")
        print(f"wrote {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
