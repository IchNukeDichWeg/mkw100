#!/usr/bin/env python3
"""Find the `mulli rD, rA, 0xF0` sites that index Racedata player slots.

The N-player layout keeps RacedataScenario's 12 inline player slots and moves players 12+ to an
overflow region (docs/mkw100/DESIGN.md). The translator rewrites each listed site into a call to
MKW100_PlayerSlotOffset (runtime/include/guest_abi/player_slots.h), so this list decides which
code reaches the overflow slots.

A site is listed only when the product is provably added to a Racedata base and the sum is
dereferenced at a player-slot displacement:
  base  = every definition reaching the add (reaching definitions over the function's direct
          control flow) is the same: a load of 0x809BD728 (Racedata::spInstance), that plus a
          scenario offset (addi), or `this` on entry to a RaceConfig / RaceConfig::Scenario method
          (receiver classes from riidefi/mkw's module symbols),
  use   = add rX, base, rD (either order), then a load/store off rX, or an addi forming a
          Player* from it (Scenario::getPlayer), whose displacement falls in slot 0 of a scenario's
          player array (race 0x28.., menu 0xC18.., award 0x1808.., or 0x8.. off a scenario pointer).
          Every dereference of the sum up to the end of the block must land in a player array.
  unrolled = when one of them lands in slot k > 0 (CodeWarrior unrolls `for i < count` loops 8 or
          6 ways: one product, then fixed displacements for players i+1..i+7), the rewritten product
          only moves player i; each of those dereferences is listed as `deref` and lifted through
          MKW100_PlayerSlotAddress, which resolves the slot pointer back to its player and scenario.
Anything else is reported as `unproven` with the reason; unproven sites are not rewritten (a
missed site leaves players 12+ reading the wrong memory). Measured 10.11.2026: 505 of 543 proven
(17 in unrolled loops, 325 deref sites), covering every player-slot indexer the access trace of
the oracle race found.

  scripts/mkw100/slot_sites.py --dol Assets/main.dol --rel Assets/StaticR.rel \
      --out projects/mkwii/mkw100/player-slot-sites.txt --report docs/mkw100/player-slot-sites.tsv
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import limit_scan  # noqa: E402

RACEDATA_GLOBAL = 0x809BD728
STRIDE = 0xF0
SCENARIOS = {0x20: "race", 0xC10: "menu", 0x1800: "award"}
PLAYERS = 0x8
PLAYER_ARRAY = 12 * STRIDE
BLOCK_LIMIT = 24  # instructions scanned each way; sites are straight-line address math
UNROLL_LIMIT = 200  # instructions scanned for dereferences: an 8-way unrolled loop body is ~60

D_FORM_MEMORY = set(range(32, 56))  # lwz .. stfdu
# What the translator can lift a deref site through (PlayerSlotStrideSites.RewriteDeref):
# lwz lbz stw stb lhz lha sth, and addi.
DEREF_REWRITABLE = {32, 34, 36, 38, 40, 42, 44, 14}


def simm(word: int) -> int:
    return struct.unpack(">h", struct.pack(">H", word & 0xFFFF))[0]


def fields(word: int) -> tuple[int, int, int, int]:
    return word >> 26, (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31


def is_branch(word: int) -> bool:
    op = word >> 26
    return op in (16, 18) or (op == 19 and ((word >> 1) & 0x3FF) in (16, 528))


def is_call(word: int) -> bool:
    return word >> 26 == 18 and word & 1 == 1  # bl


# CodeWarrior's __save_fpr .. _rest_gpr_31 thunks (MAP.txt): prologues and epilogues `bl` into
# them. They store or load r14..r31/f14..f31 through r11 and leave every volatile register as is.
SAVE_RESTORE_THUNKS = range(0x800214D4, 0x80021604)
RESTORE_GPR_THUNKS = range(0x800215B8, 0x80021604)


def call_target(address: int, word: int) -> int:
    offset = word & 0x03FFFFFC
    offset -= (offset & 0x02000000) << 1
    return ((0 if word & 2 else address) + offset) & 0xFFFFFFFF


def is_indirect_call(word: int) -> bool:
    return word >> 26 == 19 and ((word >> 1) & 0x3FF) in (16, 528) and word & 1 == 1  # blrl, bctrl


def crosses(word: int, reg: int) -> bool:
    """True when the forward scans in classify_site may step over `word`: any non-branch, a
    conditional branch (the scan follows its fall-through), or a call (bl, bctrl, blrl) when `reg`
    is callee-saved (r14..r31 survive a call under the EABI)."""
    return not is_branch(word) or word >> 26 == 16 or ((is_call(word) or is_indirect_call(word)) and reg >= 14)


def branch_targets(code: dict[int, int]) -> dict[int, list[int]]:
    """Direct b/bc target -> branch sources: the join points a backward scan meets."""
    targets: dict[int, list[int]] = {}
    for address, word in code.items():
        op = word >> 26
        if op == 18 and not word & 1:
            offset = word & 0x03FFFFFC
            offset -= (offset & 0x02000000) << 1
            targets.setdefault((0 if word & 2 else address) + offset & 0xFFFFFFFF, []).append(address)
        elif op == 16:
            offset = word & 0xFFFC
            offset -= (offset & 0x8000) << 1
            targets.setdefault((0 if word & 2 else address) + offset & 0xFFFFFFFF, []).append(address)
    return targets


def ends_flow(word: int) -> bool:
    """No fall-through after this instruction: b, or an unconditional blr/bctr (BO=20)."""
    op = word >> 26
    if op == 18:
        return not word & 1
    if op == 19 and ((word >> 1) & 0x3FF) in (16, 528):
        return (word >> 21) & 31 == 20 and not word & 1
    return False


def writes(word: int) -> int | None:
    """Destination GPR of the common integer forms used in address math, else None."""
    op, rd, ra, _ = fields(word)
    if op in (7, 14, 15) or op in (32, 33, 34, 35, 40, 41, 42, 43):
        return rd
    if op == 31:
        xo = (word >> 1) & 0x3FF
        if xo in (444, 28, 60, 124, 316, 476, 412, 24, 536, 792, 824):  # or/and/andc/nor/xor/nand/orc/slw/srw/sraw/srawi
            return ra
        if xo in (266, 40, 8, 10, 136, 138, 202, 234, 235, 11, 75, 491, 459, 104, 23, 87, 279, 311, 343, 375):
            return rd
    if op in (21, 23, 24, 25, 26, 27, 28, 29):  # rlwinm rlwnm ori oris xori xoris andi. andis.
        return ra
    return None


class Image:
    """Decoded code plus function starts; `receivers` maps a method start to the class `this`
    (r3 on entry) points at: 'racedata' (System::RaceConfig) or 'scenario' (RaceConfig::Scenario)."""

    def __init__(self, code: dict[int, int], starts: set[int], receivers: dict[int, str]):
        self.code, self.starts, self.receivers = code, starts, receivers
        self.targets = branch_targets(code)
        self.sorted_starts = sorted(starts)
        # classify_site's output: proven site -> its sum's dereferences to rewrite (unrolled loops)
        self.derefs: dict[int, list[int]] = {}

    def function_range(self, address: int) -> tuple[int, int]:
        import bisect
        index = bisect.bisect_right(self.sorted_starts, address) - 1
        start = self.sorted_starts[index]
        end = self.sorted_starts[index + 1] if index + 1 < len(self.sorted_starts) else start + 0x10000
        return start, end


ENTRY = -1
UNKNOWN = -2
VISIT_LIMIT = 4000


def reaching_definitions(image: Image, at: int, reg: int) -> set[int]:
    """Addresses of the instructions whose write to `reg` can reach the instruction at `at`, over
    the function's control flow (fall-through plus direct b/bc edges). ENTRY marks the value on
    function entry, UNKNOWN a call clobbering a volatile register or a walk too long to finish.
    Indirect jumps (bctr jump tables) are not followed; their targets are not in Image.targets."""
    defs: set[int] = set()
    seen: set[int] = set()
    work = [at]
    while work:
        position = work.pop()
        if position in image.starts:
            defs.add(ENTRY)
            continue
        predecessors = list(image.targets.get(position, []))
        previous = image.code.get(position - 4)
        if previous is not None and not ends_flow(previous):
            predecessors.append(position - 4)
        for source in predecessors:
            if source in seen:
                continue
            seen.add(source)
            if len(seen) > VISIT_LIMIT:
                return {UNKNOWN}
            word = image.code[source]
            thunk = is_call(word) and call_target(source, word) in SAVE_RESTORE_THUNKS
            if thunk and reg >= 14 and call_target(source, word) in RESTORE_GPR_THUNKS:
                defs.add(UNKNOWN)
            elif (is_call(word) or is_indirect_call(word)) and not thunk and reg < 14 and reg != 1:
                defs.add(UNKNOWN)
            elif writes(word) == reg:
                defs.add(source)
            else:
                work.append(source)
    return defs


def resolve_base(image: Image, at: int, reg: int, depth: int = 0) -> tuple[str | None, int]:
    """(kind, offset) for the value of `reg` at `at`: kind 'racedata' (offset from the Racedata
    object) or 'scenario' (offset from a RacedataScenario); (None, 0) unless every reaching
    definition agrees."""
    if depth > 4:
        return None, 0
    values = set()
    for definition in reaching_definitions(image, at, reg):
        if definition == ENTRY:
            receiver = image.receivers.get(image.function_range(at)[0])
            values.add((receiver, 0) if receiver and reg == 3 else (None, 0))
        elif definition == UNKNOWN:
            values.add((None, 0))
        else:
            values.add(define_base(image, definition, depth))
        if (None, 0) in values or len(values) > 1:
            return None, 0
    return values.pop() if values else (None, 0)


def define_base(image: Image, address: int, depth: int) -> tuple[str | None, int]:
    """Value written by the instruction at `address`: lwz from the Racedata global, addi off a
    base, or mr of one."""
    word = image.code[address]
    op, rd, ra, rb = fields(word)
    if op == 32 and ra != 0:
        hi = resolve_lis(image, address, ra)
        if hi is not None and ((hi << 16) + simm(word)) & 0xFFFFFFFF == RACEDATA_GLOBAL:
            return "racedata", 0
    elif op == 14 and ra != 0:
        kind, offset = resolve_base(image, address, ra, depth + 1)
        return (kind, offset + simm(word)) if kind else (None, 0)
    elif op == 31 and ((word >> 1) & 0x3FF) == 444 and rd == rb:
        return resolve_base(image, address, rd, depth + 1)
    return None, 0


def resolve_lis(image: Image, at: int, reg: int) -> int | None:
    """High half loaded by `lis reg` when it is the only definition reaching `at`."""
    values = set()
    for definition in reaching_definitions(image, at, reg):
        if definition < 0:
            return None
        op, _, ra, _ = fields(image.code[definition])
        if op != 15 or ra != 0:
            return None
        values.add(image.code[definition] & 0xFFFF)
    return values.pop() if len(values) == 1 else None


def slot_field(kind: str, offset: int, displacement: int) -> tuple[str, int] | None:
    """(scenario name, slot k) when the dereference lands in slot k of a player array, counting
    from the slot the product indexes (k > 0: an unrolled loop reaching the players after it)."""
    total = offset + displacement
    if kind == "scenario":
        starts = {0: "scenario"}
    else:
        starts = SCENARIOS
    for start, name in starts.items():
        if start + PLAYERS <= total < start + PLAYERS + PLAYER_ARRAY:
            return name, (total - start - PLAYERS) // STRIDE
    return None


def classify_site(image: Image, site: int) -> tuple[str, str]:
    """('racedata', scenario) or ('unproven', reason)."""
    code = image.code
    _, rd, _, _ = fields(code[site])
    address = site + 4
    sum_reg = base_reg = None
    for _ in range(BLOCK_LIMIT):
        word = code.get(address)
        if word is None or not crosses(word, rd):
            break
        op, d, a, b = fields(word)
        if op == 31 and ((word >> 1) & 0x3FF) == 266 and rd in (a, b):
            sum_reg, base_reg = d, (b if a == rd else a)
            break
        if writes(word) == rd:
            return "unproven", "product overwritten before an add"
        address += 4
    if sum_reg is None:
        return "unproven", "no add of the product in the block"
    kind, offset = resolve_base(image, address, base_reg)
    if kind is None:
        return "unproven", f"base r{base_reg} not provably Racedata"
    use = address + 4
    name = None
    derefs: list[int] = []
    unrolled = False
    for _ in range(UNROLL_LIMIT):
        word = code.get(use)
        if word is None or not crosses(word, sum_reg) or (use != address + 4 and use in image.targets):
            break
        op, _, a, _ = fields(word)
        if (op in D_FORM_MEMORY or op == 14) and a == sum_reg:  # a dereference, or addi forming a Player*
            field = slot_field(kind, offset, simm(word))
            if field is None:
                return "unproven", f"displacement 0x{simm(word) & 0xFFFF:X} off {kind}+0x{offset:X} is not a player slot"
            name = name or field[0]
            unrolled |= field[1] > 0
            derefs.append(use)
        if writes(word) == sum_reg:
            break
        use += 4
    if name is None:
        return "unproven", "sum not dereferenced in the block"
    if unrolled and any(code[d] >> 26 not in DEREF_REWRITABLE for d in derefs):
        return "unproven", "unrolled loop dereference is not a plain load, store or addi"
    image.derefs[site] = derefs if unrolled else []
    return "racedata", name


def align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) & ~(alignment - 1)


def relocated_rel_text(data: bytes, load: int, pointers: set[int] | None = None) -> list[tuple[int, bytes]]:
    """StaticR's executable sections with its relocations applied, laid out as the translator does
    (translator/src/Translator.Core/Parsing/Rel/RelFile.cs): sections at load + file offset, bss
    right after the heap-rounded image (alignUp(size + 0x20, 0x20), then the module's bss align).
    `pointers` collects every address a data word (ADDR32: vtables, function pointer tables) or a
    lis/addi pair (ADDR16) is relocated to: the module's address-taken code."""
    module_id, = struct.unpack_from(">I", data, 0)
    count, table = struct.unpack_from(">II", data, 0x0C)
    bss_size, = struct.unpack_from(">I", data, 0x20)
    imp_offset, imp_size = struct.unpack_from(">II", data, 0x28)
    bss_align = struct.unpack_from(">I", data, 0x44)[0] or 0x20
    bss_cursor = align_up(align_up(len(data) + 0x20, 0x20), bss_align)
    addresses, executable = {}, []
    for i in range(count):
        raw, size = struct.unpack_from(">II", data, table + i * 8)
        offset = raw & ~1
        if offset:
            addresses[i] = load + offset
            if raw & 1 and size:
                executable.append((i, offset, size))
        elif size:
            addresses[i] = load + bss_cursor
            bss_cursor += size
    image = bytearray(data)
    for entry in range(imp_offset, imp_offset + imp_size, 8):
        target_module, rel_offset = struct.unpack_from(">II", data, entry)
        section = cursor = 0
        while True:
            delta, kind, target_section, addend = struct.unpack_from(">HBBI", data, rel_offset)
            rel_offset += 8
            if kind == 203:  # R_DOLPHIN_END
                break
            cursor += delta
            if kind == 202:  # R_DOLPHIN_SECTION
                section, cursor = target_section, 0
                continue
            if kind == 201:  # R_DOLPHIN_NOP
                continue
            value = addend if target_module == 0 else addresses[target_section] + addend
            if pointers is not None and kind in (1, 4, 5, 6):
                pointers.add(value & 0xFFFFFFFF)
            where = (addresses[section] - load) + cursor
            if kind == 4:  # R_PPC_ADDR16_LO
                struct.pack_into(">H", image, where, value & 0xFFFF)
            elif kind == 5:  # R_PPC_ADDR16_HI
                struct.pack_into(">H", image, where, (value >> 16) & 0xFFFF)
            elif kind == 6:  # R_PPC_ADDR16_HA
                struct.pack_into(">H", image, where, ((value + 0x8000) >> 16) & 0xFFFF)
            elif kind == 1:  # R_PPC_ADDR32
                struct.pack_into(">I", image, where, value & 0xFFFFFFFF)
            elif kind == 10:  # R_PPC_REL24: callers of a method are how array_sites types `this`
                word, = struct.unpack_from(">I", image, where)
                delta = (value - (load + where)) & 0x03FFFFFC
                struct.pack_into(">I", image, where, (word & ~0x03FFFFFC) | delta)
            # REL14 only moves conditional branch targets, which stay inside one function.
    assert module_id != 0
    return [(load + offset, bytes(image[offset:offset + size])) for _, offset, size in executable]


def load_code(dol: Path, rel: Path) -> dict[int, int]:
    code: dict[int, int] = {}
    sections = limit_scan.dol_text_sections(dol.read_bytes())
    sections += relocated_rel_text(rel.read_bytes(), limit_scan.REL_LOAD_ADDRESS)
    for base, blob in sections:
        for offset in range(0, len(blob) - 3, 4):
            code[base + offset] = struct.unpack_from(">I", blob, offset)[0]
    return code


RECEIVERS = (("Q36System10RaceConfig8Scenario", "scenario"), ("Q26System10RaceConfig", "racedata"))


def rel_text_base(rel: Path) -> int:
    """Guest address of StaticR's .text (section 1), the base of the module symbol offsets."""
    data = rel.read_bytes()
    _, table = struct.unpack_from(">II", data, 0x0C)
    raw, _ = struct.unpack_from(">II", data, table + 8)
    return limit_scan.REL_LOAD_ADDRESS + (raw & ~1)


def load_receivers(module_symbols: Path, text_base: int) -> dict[int, str]:
    """Method starts by receiver class, from riidefi/mkw's module symbols (`name = .text:0xOFF;`)."""
    receivers = {}
    for line in module_symbols.read_text().splitlines():
        name, _, rest = line.partition(" = .text:0x")
        if not rest or "type:function" not in rest:
            continue
        mangled_class = name.split("__", 1)[1] if "__" in name[1:] else ""
        for marker, kind in RECEIVERS:
            if mangled_class.startswith(marker) and mangled_class[len(marker):][:1] in ("F", "C"):
                receivers[text_base + int(rest.split(";")[0], 16)] = kind
                break
    return receivers


def load_starts(function_map: Path) -> set[int]:
    starts = set()
    for line in function_map.read_text().splitlines():
        token = line.split()[0] if line.split() else ""
        if len(token) == 8:
            try:
                starts.add(int(token, 16))
            except ValueError:
                pass
    return starts


def find_sites(image: Image) -> list[tuple[int, str, str]]:
    out = []
    for address, word in sorted(image.code.items()):
        if word >> 26 == 7 and (word & 0xFFFF) == STRIDE:
            kind, detail = classify_site(image, address)
            out.append((address, kind, detail))
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dol", type=Path, required=True)
    parser.add_argument("--rel", type=Path, required=True)
    parser.add_argument("--symbols", type=Path, action="append", default=[], help="names for the report")
    parser.add_argument("--function-map", type=Path, required=True, help="projects/mkwii/MAP.txt")
    parser.add_argument("--module-symbols", type=Path, required=True,
                        help="riidefi/mkw config/RMCP01/module/symbols.txt (receiver classes)")
    parser.add_argument("--out", type=Path, required=True, help="site list for the translator")
    parser.add_argument("--report", type=Path, required=True, help="every 0xF0 mulli with its verdict")
    args = parser.parse_args()

    image = Image(load_code(args.dol, args.rel), load_starts(args.function_map),
                  load_receivers(args.module_symbols, rel_text_base(args.rel)))
    sites = find_sites(image)
    symbols = limit_scan.load_symbols(args.symbols) if args.symbols else {}
    starts = sorted(symbols)

    def owner(address: int) -> str:
        import bisect
        index = bisect.bisect_right(starts, address) - 1
        return symbols[starts[index]] if index >= 0 else "?"

    proven = [s for s in sites if s[1] == "racedata"]
    with args.out.open("w") as out:
        out.write("# mulli 0xF0 sites indexing Racedata player slots; generated by scripts/mkw100/slot_sites.py\n")
        out.write("# The translator rewrites each into MKW100_PlayerSlotOffset (players 12+ go to the overflow region).\n")
        out.write("# `deref` lines: the slot pointer's loads/stores in an unrolled loop, rewritten into\n")
        out.write("# MKW100_PlayerSlotAddress (they reach the players after the indexed one).\n")
        for address, _, scenario in proven:
            out.write(f"0x{address:08X}  # {scenario}\n")
            for deref in image.derefs.get(address, []):
                out.write(f"0x{deref:08X} deref  # of 0x{address:08X}\n")
    with args.report.open("w") as report:
        report.write("address\tverdict\tdetail\tfunction\n")
        for address, kind, detail in sites:
            derefs = image.derefs.get(address)
            if derefs:
                detail += f" unrolled, {len(derefs)} derefs"
            report.write(f"{address:08X}\t{kind}\t{detail}\t{owner(address)}\n")
    unrolled = sum(1 for derefs in image.derefs.values() if derefs)
    print(f"{len(sites)} mulli 0xF0 sites: {len(proven)} index Racedata player slots "
          f"({unrolled} in unrolled loops), {len(sites) - len(proven)} unproven -> {args.out}, {args.report}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
