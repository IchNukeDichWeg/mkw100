#!/usr/bin/env python3
"""Turn an access-trace log into a per-function map of Racedata scenario accesses.

Input is the runtime output of an MKW100_ACCESS_TRACE build (runtime/include/guest_abi/
access_trace.h) whose watch range is the three Racedata scenarios, as boot_race.cpp sets it.
PCs are symbolicated with atos against the same traced binary.

  scripts/mkw100/access_report.py build-trace/out/trace.log --binary build-trace/WiiCompiled \
      --out docs/mkw100/racedata-access.tsv

One TSV row per (function, scenario, region, field offset, size, kind): region is `header`,
`player` (field offset inside RacedataPlayer, slots folded together) or `settings`.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

LINE = re.compile(r"\[access-trace\] (?:range (\S+) )?pc ([0-9a-f,]+) offset ([0-9a-f]+) size (\d+) ([RW]) count (\d+)")

# Racedata: scenarios at +0x20 (race), +0xC10 (menu), +0x1800 (award); the watch range starts at +0x20.
# Frames inside the runtime's guest memory layer; the accessing function is the first frame outside.
MEMORY_LAYER = re.compile(r"(^|\s)(Memory|GuestFlat|MemoryInline|AccessTrace)::|Try(Read|Write)GuestScalar")

SCENARIO_SIZE = 0xBF0
SCENARIOS = ("race", "menu", "award")
PLAYERS = 0x8
PLAYER_SIZE = 0xF0
PLAYER_COUNT = 12
SETTINGS = 0xB48


def classify(offset: int) -> tuple[str, str, int, int | None]:
    """(scenario, region, field offset, player slot) for an offset from the race scenario."""
    scenario = SCENARIOS[offset // SCENARIO_SIZE]
    inner = offset % SCENARIO_SIZE
    if inner < PLAYERS:
        return scenario, "header", inner, None
    if inner < SETTINGS:
        slot, field = divmod(inner - PLAYERS, PLAYER_SIZE)
        return scenario, "player", field, slot
    return scenario, "settings", inner - SETTINGS, None


def parse(log: Path, range_name: str = "racedata") -> list[tuple[tuple[int, ...], int, int, str, int]]:
    """(caller pcs innermost first, offset, size, kind, count) per record of one watched range;
    lines without a range tag (older logs) are the Racedata range."""
    records = []
    for line in log.read_text(errors="replace").splitlines():
        if (match := LINE.search(line)) and (match[1] or "racedata") == range_name:
            pcs = tuple(int(pc, 16) for pc in match[2].split(","))
            records.append((pcs, int(match[3], 16), int(match[4]), match[5], int(match[6])))
    return records


def ranges(log: Path) -> list[str]:
    return sorted({match[1] or "racedata" for line in log.read_text(errors="replace").splitlines()
                   if (match := LINE.search(line))})


def build_object_rows(records, names) -> list[tuple]:
    """One row per (function, offset, size, kind) for a watched singleton object."""
    rows: dict[tuple, int] = defaultdict(int)
    for pcs, offset, size, kind, count in records:
        rows[(accessor(pcs, names), f"0x{offset:X}", size, kind)] += count
    return [(*key, count) for key, count in sorted(rows.items())]


def accessor(pcs: tuple[int, ...], names: dict[int, str]) -> str:
    for pc in pcs:
        if pc and not MEMORY_LAYER.search(names[pc]):
            return names[pc]
    return names[pcs[0]]


def symbolicate(binary: Path, pcs: list[int]) -> dict[int, str]:
    names: dict[int, str] = {}
    chunk = 500
    for start in range(0, len(pcs), chunk):
        part = pcs[start:start + chunk]
        output = subprocess.run(["atos", "-o", str(binary), *(hex(pc) for pc in part)],
                                capture_output=True, text=True, check=True).stdout.splitlines()
        if len(output) != len(part):
            raise SystemExit(f"atos returned {len(output)} lines for {len(part)} addresses")
        for pc, name in zip(part, output):
            names[pc] = name.split(" (in ")[0].strip()
        done = min(start + chunk, len(pcs))
        print(f"symbolicated {done}/{len(pcs)} ({done / len(pcs):.0%})", file=sys.stderr)
    return names


def build_rows(records, names) -> list[tuple]:
    rows: dict[tuple, list] = defaultdict(lambda: [0, set()])
    for pcs, offset, size, kind, count in records:
        scenario, region, field, slot = classify(offset)
        entry = rows[(accessor(pcs, names), scenario, region, field, size, kind)]
        entry[0] += count
        if slot is not None:
            entry[1].add(slot)
    out = []
    for (function, scenario, region, field, size, kind), (count, slots) in sorted(rows.items()):
        slot_text = ",".join(str(s) for s in sorted(slots)) if slots else "-"
        out.append((function, scenario, region, f"0x{field:X}", size, kind, count, slot_text))
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log", type=Path)
    parser.add_argument("--binary", type=Path, required=True, help="the traced binary that wrote the log")
    parser.add_argument("--out", type=Path, required=True, help="Racedata scenario map (TSV)")
    parser.add_argument("--objects-dir", type=Path,
                        help="also write <range>-access.tsv for every watched singleton object here")
    args = parser.parse_args()

    records = parse(args.log)
    if not records:
        print(f"no [access-trace] records in {args.log}", file=sys.stderr)
        return 1
    names = symbolicate(args.binary, sorted({pc for r in records for pc in r[0] if pc}))
    rows = build_rows(records, names)
    if args.objects_dir:
        for name in ranges(args.log):
            if name == "racedata":
                continue
            object_records = parse(args.log, name)
            object_names = symbolicate(args.binary, sorted({pc for r in object_records for pc in r[0] if pc}))
            path = args.objects_dir / f"{name.replace('_', '-')}-access.tsv"
            with path.open("w") as out:
                out.write("function\toffset\tsize\tkind\tcount\n")
                for row in build_object_rows(object_records, object_names):
                    out.write("\t".join(str(v) for v in row) + "\n")
            print(f"{name}: {len(object_records)} records -> {path}")
    with args.out.open("w") as out:
        out.write("function\tscenario\tregion\tfield\tsize\tkind\tcount\tslots\n")
        for row in rows:
            out.write("\t".join(str(v) for v in row) + "\n")
    functions = {row[0] for row in rows}
    print(f"{len(records)} records, {len(functions)} functions, {len(rows)} rows -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
