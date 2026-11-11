#!/usr/bin/env python3
"""Find the instructions that index per-player arrays inside singleton objects.

Generalizes slot_sites.py (Racedata) to any singleton with inline `[12]` per-player arrays, as
declared in projects/mkwii/mkw100/array-classes.tsv. Element i >= 12 of every array of a class
lives in shadow window i - 12 (runtime/include/guest_abi/array_overflow.h), laid out like the
object, so the game's own base + displacement arithmetic lands there once the index math is
redirected. Two site kinds, each rewritten by the translator:

  scale  `mulli rD, rA, stride`, or `rlwinm rD, rS, log2(stride), MB, 31-log2(stride)` (an index
         times the array stride), whose product is added to the instance (`add`, or the index of
         an X-form load/store) and dereferenced inside element 0 of an array with that stride.
         Becomes rD = MKW100_IndexScaled(rD, class, stride). The rewritten value is an offset
         into this class's window, so every other instruction that can read it (reached_uses, a
         def-use walk through calls and returns) must add it to the instance again or index an
         instance array with it, and the element address may only be dereferenced inside element
         0. DriverMgr::CalcFinishType (0x8078CFB0) reuses its finishType[i] offset for Raceinfo's
         player array and stays unproven.
  step   `addi rP, rP, stride` walking a pointer over an array: every other reaching definition
         of rP is instance + d, the increment reaches itself (a loop), every load/store through
         rP lands inside element 0 of an array with that stride, and rP is never compared (an
         end-pointer bound would stop at 12). Becomes rP = MKW100_ArrayStep(rP, class, stride, d).

A base is the instance when every reaching definition of it (slot_sites' reaching definitions)
loads the class's instance global, adds to such a value, or is `this` on entry to a method whose
every direct caller passes the instance and whose address is never taken (no vtable or function
pointer slot holds it, so no indirect call can pass anything else). Failing that, a method the
access trace saw touch this object is trusted (virtual methods, or `this` loaded from a pointer
another object holds), unless it loads the instance from the global itself: the trace then
explains nothing about `this`. That is safe because MKW100_IndexScaled only moves indices >= 12 and
MKW100_ArrayStep only moves the live instance's element-11 address: with any other object the
rewritten access was out of bounds already.

A member class (`0xGLOBAL+0xMEMBER` in the table) is the object the singleton points to at that
member: its base is a load of the member from a proven singleton instance.

A class may name its constructor (optional last column): `this` there is the instance being
built, before the game stores it in the global. The runtime publishes the instance before that
constructor runs, so the helpers see it as the live one.

Candidate functions are those that load the instance global, those the oracle race's access trace
saw touching the object (docs/mkw100/<class>-access.tsv), and their callees down to CALLEE_DEPTH
levels (methods handed the instance as `this`, accessors the translator inlines).

  scripts/mkw100/array_sites.py --dol Assets/main.dol --rel Assets/StaticR.rel \
      --function-map projects/mkwii/MAP.txt --classes projects/mkwii/mkw100/array-classes.tsv \
      --out projects/mkwii/mkw100/array-sites.txt --report docs/mkw100/array-sites.tsv
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import limit_scan  # noqa: E402
import slot_sites as ss  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
INLINE = 12
# Callee levels below a root scanned for sites. 2 reaches Item::GessoMgr::ApplyBlooper
# (Item::Manager::Update loads the global, GessoMgr::Update passes it on as `this`).
CALLEE_DEPTH = 2
X_FORM_MEMORY = {23, 55, 87, 119, 151, 183, 215, 247, 279, 311, 343, 375, 407, 439, 535, 567, 599, 631,
                 663, 695, 727, 759}  # lwzx .. stfdux


@dataclass(frozen=True)
class ArrayClass:
    id: int
    name: str
    instance_global: int
    member: int | None  # the object is the pointer at *instance_global + member
    size: int
    arrays: tuple[tuple[int, int], ...]  # (offset, stride)
    trace: Path | None
    ctor: int | None = None  # constructor whose `this` is the instance being built

    def owner(self) -> ArrayClass:
        """The singleton holding the member pointer, as a class without arrays."""
        return ArrayClass(0, f"{self.name}.owner", self.instance_global, None, 0, (), None)

    def element_index(self, offset: int, stride: int) -> int | None:
        """Index (0..11) of the element of an array with this stride that object offset `offset`
        lies in."""
        for array_offset, array_stride in self.arrays:
            if array_stride == stride and array_offset <= offset < array_offset + INLINE * stride:
                return (offset - array_offset) // stride
        return None

    def element_zero(self, offset: int, stride: int) -> int | None:
        """Array offset when object offset `offset` lies in element 0 of an array with this stride."""
        for array_offset, array_stride in self.arrays:
            if array_stride == stride and array_offset <= offset < array_offset + stride:
                return array_offset
        return None


@dataclass(frozen=True)
class Site:
    address: int
    cls: ArrayClass
    kind: str  # 'scale', 'step' or 'unproven'
    stride: int
    offset: int  # step: the pointer's offset from the instance at element 0
    detail: str


def load_classes(path: Path) -> list[ArrayClass]:
    classes = []
    for line in path.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        fields = line.split()
        if len(fields) not in (6, 7):
            raise ValueError(f"{path}: expected 6 or 7 fields: {line}")
        ident, name, instance, size, arrays, trace = fields[:6]
        ctor = fields[6] if len(fields) == 7 else "-"
        global_, _, member = instance.partition("+")
        classes.append(ArrayClass(
            int(ident), name, int(global_, 16), int(member, 16) if member else None, int(size, 16),
            tuple((int(o, 16), int(s, 16)) for o, s in (a.split(":") for a in arrays.split(","))),
            None if trace == "-" else REPO / trace, None if ctor == "-" else int(ctor, 16)))
    return classes


def attested_functions(trace: Path | None) -> set[int]:
    """Function addresses from an object access map (func_XXXXXXXX or wrapper_..._xxxxxxxx names)."""
    if trace is None or not trace.exists():
        return set()
    found = set()
    for line in trace.read_text().splitlines()[1:]:
        if match := re.search(r"_([0-9A-Fa-f]{8})\b", line.split("\t")[0]):
            found.add(int(match[1], 16))
    return found


def dol_data_words(data: bytes) -> set[int]:
    """Every aligned word of the DOL's data sections: a superset of its stored code pointers."""
    words = set()
    for i in range(11):
        offset, = struct.unpack_from(">I", data, 0x1C + i * 4)
        size, = struct.unpack_from(">I", data, 0xAC + i * 4)
        for at in range(offset, offset + size - 3, 4):
            words.add(struct.unpack_from(">I", data, at)[0])
    return words


def scale_stride(word: int) -> tuple[int, int, int] | None:
    """(destination, source, stride) when `word` multiplies a register by a power of two or by an
    immediate: mulli, or rlwinm with SH = log2(stride) and ME = 31 - SH."""
    op, rd, ra, rb = ss.fields(word)
    if op == 7:
        return rd, ra, ss.simm(word)
    if op == 21:
        sh, mb, me = rb, (word >> 6) & 31, (word >> 1) & 31
        if 1 <= sh <= 8 and me == 31 - sh and mb <= me:
            return ra, rd, 1 << sh
    return None


def branch_target(address: int, word: int) -> int | None:
    if word >> 26 != 18:
        return None
    offset = word & 0x03FFFFFC
    offset -= (offset & 0x02000000) << 1
    return ((0 if word & 2 else address) + offset) & 0xFFFFFFFF


X_STORES = {150, 151, 183, 215, 247, 407, 439, 662, 918}  # stwcx. stwx stwux stbx stbux sthx sthux stwbrx sthbrx
X_ARITH_RA = {104, 202, 234, 200, 232}  # neg addze addme subfze subfme: rA only
X_UNARY_RS = {26, 922, 954}  # cntlzw extsh extsb: rS only
# add addc adde subf subfc subfe mullw mulhw mulhwu divw divwu: rD = f(rA, rB); 9-bit XO, the
# 10th bit is OE
X_ARITH_RA_RB = {266, 10, 138, 40, 8, 136, 235, 75, 11, 491, 459}


def gpr_sources(word: int) -> set[int]:
    """GPRs `word` reads. Conservative: a form not decoded here reads every register field, so an
    unknown instruction counts as a use and keeps a site unproven rather than wrongly proven."""
    op, d, a, b = ss.fields(word)
    if op in (16, 18, 19, 59, 63):
        return set()  # bc, b, CR logic/bclr/bcctr/isync, floating-point arithmetic
    if op in (7, 8, 10, 11, 12, 13) or op in (32, 33, 34, 35, 40, 41, 42, 43, 46) or 48 <= op <= 55:
        return {a}  # mulli subfic cmpli cmpi addic addic. | loads lmw | FP loads/stores (rA only)
    if op in (14, 15):
        return {a} if a else set()  # addi/addis: rA = 0 is the literal 0
    if op in (56, 57, 60, 61):
        return {a}  # psq_l psq_lu psq_st psq_stu
    if op in (36, 37, 38, 39, 44, 45, 47):
        return {d, a}  # stores, stmw: rS and rA
    if op in (24, 25, 26, 27, 28, 29, 21):
        return {d}  # ori oris xori xoris andi. andis. rlwinm: rS
    if op == 20:
        return {d, a}  # rlwimi also keeps bits of rA
    if op == 23:
        return {d, b}  # rlwnm
    if op == 31:
        xo = (word >> 1) & 0x3FF
        if xo in X_FORM_MEMORY or xo in (20, 534, 790, 86, 54, 470, 982):  # indexed loads/stores, lwarx, cache ops
            sources = {b} | ({a} if a else set())
            return sources | ({d} if xo in X_STORES else set())  # FP stores' rS is an FPR
        if xo in (0, 32):
            return {a, b}  # cmp cmpl: the D field is a CR field
        if xo & 0x1FF in X_ARITH_RA_RB:
            return {a, b}
        if xo & 0x1FF in X_ARITH_RA:
            return {a}
        if xo in X_UNARY_RS:
            return {d}
        if xo == 824:
            return {d}  # srawi: rB is the shift amount
        if xo in (444, 28, 60, 124, 316, 476, 412, 24, 536, 792):
            return {d, b}  # or and andc nor xor nand orc slw srw sraw: rS, rB
        if xo in (467, 144):
            return {d}  # mtspr mtcrf
        if xo in (339, 371, 19, 83):
            return set()  # mfspr mftb mfcr mfmsr
    return {d, a, b}


# How many calls deep the liveness walk follows a register into a callee or back into a caller
# before it counts the call or return as a use. 2 settles every site the walk flagged on PAL
# (SetController's leftover r3, AI::Manager's r4 passed to a one-argument callee).
LIVENESS_DEPTH = 2


class CallGraph:
    def __init__(self, image: ss.Image, callers: dict[int, list[int]], address_taken: set[int]):
        self.image = image
        self.callers = callers  # function -> bl / tail-call b sites
        self.address_taken = address_taken


def reached_uses(image: ss.Image, site: int, reg: int, graph: CallGraph | None = None) -> set[int]:
    """Instructions that can read the value `reg` holds after `site`, following fall-through and
    direct branches inside the function until `reg` is overwritten. A call reading an argument
    register, a return of r3, or an indirect jump (bctr) counts as a use; a call ends a volatile
    register's value. With `graph`, a direct call is a use only when the callee can read the
    register on entry, and a return only when a caller can read r3 after its bl (both
    LIVENESS_DEPTH calls deep; an address-taken function's return always counts).

    A returned r4 is not a use. Simplification by the ABI: r4 returns only the low word of a
    64-bit result, never an element address (structs come back through a hidden pointer). Proving
    it instead fails on paths no compiler takes: 4,051 of the 12,337 bctrl sites on PAL can reach
    an r4 read along some path, so every function called through a pointer would count."""
    return _walk(image, site + 4, reg, graph, 0, ())


def _walk(image: ss.Image, first: int, reg: int, graph: CallGraph | None, depth: int,
          returns: tuple[int, ...]) -> set[int]:
    """`returns` holds the instruction after each call the walk entered, innermost last: a return
    inside a callee continues there rather than in every caller of the callee."""
    _, end = image.function_range(first)
    uses: set[int] = set()
    seen: set[int] = set()
    work = [first]
    follow = graph is not None and depth < LIVENESS_DEPTH
    while work:
        at = work.pop()
        if at in seen or at >= end or at not in image.code:
            continue
        seen.add(at)
        if len(seen) > ss.VISIT_LIMIT:
            return {ss.UNKNOWN}
        word = image.code[at]
        if ss.is_call(word) or ss.is_indirect_call(word):
            if 3 <= reg <= 10 or (ss.is_indirect_call(word) and reg == 12):
                target = branch_target(at, word) if ss.is_call(word) else None
                if not (follow and target in image.starts
                        and not _walk(image, target, reg, graph, depth + 1, returns + (at + 4,))):
                    uses.add(at)
            if reg < 14 and reg != 1:
                continue
            work.append(at + 4)
            continue
        op = word >> 26
        if op == 19 and ((word >> 1) & 0x3FF) == 528:  # bcctr: a jump table goes where we can't follow
            uses.add(at)
            continue
        if op == 19 and ((word >> 1) & 0x3FF) == 16:  # bclr: the caller sees r3
            if reg == 3:
                if returns:
                    if _walk(image, returns[-1], reg, graph, depth, returns[:-1]):
                        uses.add(at)
                elif not (follow and _return_dead(image, at, reg, graph, depth)):
                    uses.add(at)
            if not ss.ends_flow(word):
                work.append(at + 4)
            continue
        if reg in gpr_sources(word):
            uses.add(at)
        if ss.writes(word) == reg:
            continue
        if op == 18:
            target = branch_target(at, word)
            if target is not None:
                work.append(target)
            continue
        if op == 16:
            offset = word & 0xFFFC
            offset -= (offset & 0x8000) << 1
            work.append(((0 if word & 2 else at) + offset) & 0xFFFFFFFF)
        work.append(at + 4)
    return uses


def _return_dead(image: ss.Image, at: int, reg: int, graph: CallGraph, depth: int) -> bool:
    """True when no caller of the function returning at `at` reads `reg` after its call."""
    function = image.function_range(at)[0]
    sites = graph.callers.get(function, [])
    if function in graph.address_taken or not sites:
        return False
    return all(image.code[c] & 1 and (c + 4 >= image.function_range(c)[1]
                                      or not _walk(image, c + 4, reg, graph, depth + 1, ())) for c in sites)


class Analysis:
    def __init__(self, image: ss.Image, address_taken: set[int]):
        self.image = image
        self.address_taken = address_taken
        self.callers: dict[int, list[int]] = {}
        for address, word in image.code.items():
            target = branch_target(address, word)
            if target is None or target not in image.starts:
                continue
            # bl, or a tail-call b into another function (r3 passes through unchanged)
            if word & 1 or image.function_range(address)[0] != target:
                self.callers.setdefault(target, []).append(address)
        self.graph = CallGraph(image, self.callers, address_taken)
        self._this: dict[tuple, bool] = {}
        # (instance_global, member) -> functions the oracle race's access trace saw in the object
        self.attested: dict[tuple[int, int | None], set[int]] = {}
        self.used_attestation: set[tuple] = set()
        self.classes: list[ArrayClass] = []
        self.strict = False  # True while proving without the access trace

    def callees(self, function: int) -> set[int]:
        start, end = self.image.function_range(function)
        found = set()
        for address in range(start, end, 4):
            word = self.image.code.get(address)
            if word is not None and word >> 26 == 18 and word & 1:
                target = branch_target(address, word)
                if target in self.image.starts:
                    found.add(target)
        return found

    def claimed_elsewhere(self, cls: ArrayClass, function: int) -> bool:
        """True when, without trusting the trace, `this` of `function` is another class's instance."""
        saved, self.strict = self.strict, True
        try:
            return any(self.this_is_instance(other, function) for other in self.classes
                       if (other.instance_global, other.member) != (cls.instance_global, cls.member))
        finally:
            self.strict = saved

    def this_is_instance(self, cls: ArrayClass, function: int) -> bool:
        key = (cls.instance_global, cls.member, function, self.strict)
        if key in self._this:
            return self._this[key]
        self._this[key] = False  # recursion: a cycle proves nothing
        sites = self.callers.get(function, [])
        # The constructor builds the instance; the runtime publishes it before the call
        # (runtime/src/hle/item_manager.cpp), so the helpers recognize it.
        proven = function == cls.ctor
        if not proven and function not in self.address_taken:
            # Calls from dead code (a function nothing calls, points to or was seen running)
            # pass nothing.
            live = [site for site in sites if not self.dead(cls, self.image.function_range(site)[0])]
            proven = bool(live) and all(self.resolve(cls, site, 3) == ("instance", 0) for site in live)
        if (not proven and not self.strict
                and function in self.attested.get((cls.instance_global, cls.member), set())
                and not self.claimed_elsewhere(cls, function)
                and not self.loads_instance(cls, function)):
            # Virtual, or reached through a pointer held elsewhere: the access trace saw this very
            # function inside the object (and `this` is not provably another class's instance: a
            # method of the owner reaches the member object too). Safe to trust: the helpers only
            # move indices >= 12 of the live instance, out of bounds for any other object anyway.
            # Not when the function loads the instance itself: that explains what the trace saw, and
            # `this` is then some other object (Item::PowMgr's update reads DriverMgr this way).
            proven = True
            self.used_attestation.add(key)
        self._this[key] = proven
        return proven

    def loads_instance(self, cls: ArrayClass, function: int) -> bool:
        """True when the function loads the class's instance from its global (or its owner's)."""
        start, end = self.image.function_range(function)
        return any(self.image.code.get(a, 0) >> 26 == 32 and self.define(cls, a, 0) == ("instance", 0)
                   for a in range(start, end, 4))

    def dead(self, cls: ArrayClass, function: int) -> bool:
        return (function not in self.callers and function not in self.address_taken
                and function not in self.attested.get((cls.instance_global, cls.member), set()))

    def resolve(self, cls: ArrayClass, at: int, reg: int, depth: int = 0) -> tuple[str | None, int]:
        """('instance', offset) when every reaching definition of reg at `at` is instance + offset."""
        if depth > 6:
            return None, 0
        values = set()
        for definition in ss.reaching_definitions(self.image, at, reg):
            if definition == ss.ENTRY:
                function = self.image.function_range(at)[0]
                value = ("instance", 0) if reg == 3 and self.this_is_instance(cls, function) else (None, 0)
            elif definition == ss.UNKNOWN:
                value = (None, 0)
            else:
                value = self.define(cls, definition, depth)
            values.add(value)
            if (None, 0) in values or len(values) > 1:
                return None, 0
        return values.pop() if values else (None, 0)

    def define(self, cls: ArrayClass, address: int, depth: int) -> tuple[str | None, int]:
        word = self.image.code[address]
        op, rd, ra, rb = ss.fields(word)
        if op == 32 and ra != 0 and cls.member is None:
            hi = ss.resolve_lis(self.image, address, ra)
            if hi is not None and ((hi << 16) + ss.simm(word)) & 0xFFFFFFFF == cls.instance_global:
                return "instance", 0
        elif op == 32 and ra != 0:
            kind, offset = self.resolve(cls.owner(), address, ra, depth + 1)
            if kind and offset + ss.simm(word) == cls.member:
                return "instance", 0
        elif op == 14 and ra != 0:
            kind, offset = self.resolve(cls, address, ra, depth + 1)
            return (kind, offset + ss.simm(word)) if kind else (None, 0)
        elif op == 31 and ((word >> 1) & 0x3FF) == 444 and rd == rb:
            return self.resolve(cls, address, rd, depth + 1)
        return None, 0

    # -- scale sites --------------------------------------------------------------------------

    def classify_scale(self, site: int, cls: ArrayClass) -> Site:
        code = self.image.code
        rd, _, stride = scale_stride(code[site])

        def verdict(kind: str, detail: str) -> Site:
            return Site(site, cls, kind, stride, 0, detail)

        if all(stride != s for _, s in cls.arrays):
            return verdict("unproven", f"stride {stride} matches no array")
        address = site + 4
        for _ in range(ss.BLOCK_LIMIT):
            word = code.get(address)
            if word is None or not ss.crosses(word, rd):
                break
            op, d, a, b = ss.fields(word)
            xo = (word >> 1) & 0x3FF
            if op == 31 and xo == 266 and rd in (a, b):  # add sum, base, scaled
                base = b if a == rd else a
                kind, base_offset = self.resolve(cls, address, base)
                if kind is None:
                    return verdict("unproven", f"base r{base} not provably the instance")
                if (other := self.other_use(site, rd, address, cls, stride)) is not None:
                    return verdict("unproven", other)
                return self.deref_verdict(address, d, base_offset, cls, stride, verdict)
            if op == 31 and xo in X_FORM_MEMORY and b == rd and a != 0:  # lwzx rD, base, scaled
                kind, base_offset = self.resolve(cls, address, a)
                if kind is None:
                    return verdict("unproven", f"base r{a} not provably the instance")
                array = cls.element_zero(base_offset, stride)
                if array is None:
                    return verdict("unproven", f"indexed access at object+0x{base_offset:X} is not an array element")
                if (other := self.other_use(site, rd, address, cls, stride)) is not None:
                    return verdict("unproven", other)
                return verdict("scale", f"0x{array:X}")
            if ss.writes(word) == rd:
                return verdict("unproven", "scaled index overwritten before use")
            address += 4
        return verdict("unproven", "scaled index not added to a base")

    def other_use(self, site: int, reg: int, proven: int, cls: ArrayClass, stride: int) -> str | None:
        """Why the scaled index is unsafe to rewrite, when some read other than `proven` can see it:
        MKW100_IndexScaled moves it into a window of this class, so it may only index this class's
        instance again (DriverMgr::Update adds it to `this` for frameCounter and the +0x130 array:
        the window holds both), never another base (DriverMgr::CalcFinishType reused its
        finishType[i] offset for Raceinfo's player array)."""
        others = reached_uses(self.image, site, reg, self.graph) - {proven}
        for other in sorted(others):
            if other == ss.UNKNOWN or not self.indexes_instance(other, reg, cls, stride):
                where = "a walk too long to follow" if other == ss.UNKNOWN else f"0x{other:08X}"
                return f"scaled index also read at {where}"
        return None

    def indexes_instance(self, at: int, reg: int, cls: ArrayClass, stride: int) -> bool:
        """True when the instruction at `at` adds the scaled index in `reg` to the instance and the
        sum only reaches array elements, or is an indexed access of an array of the instance."""
        word = self.image.code[at]
        op, d, a, b = ss.fields(word)
        xo = (word >> 1) & 0x3FF
        if op != 31:
            return False
        if xo == 266 and reg in (a, b) and a != b:
            kind, base_offset = self.resolve(cls, at, b if a == reg else a)
            return kind is not None and all(
                use != ss.UNKNOWN and self.element_access(use, d, base_offset, cls, stride)
                for use in reached_uses(self.image, at, d, self.graph))
        if xo in X_FORM_MEMORY and b == reg and a not in (0, reg) and not (xo in X_STORES and d == reg):
            kind, base_offset = self.resolve(cls, at, a)
            return kind is not None and cls.element_zero(base_offset, stride) is not None
        return False

    def element_access(self, at: int, sum_reg: int, base_offset: int, cls: ArrayClass, stride: int) -> bool:
        """True when the instruction at `at` only dereferences the element address in `sum_reg`
        (or takes an address inside it) within element 0 of an array with this stride."""
        word = self.image.code.get(at)
        if word is None:
            return False
        op, d, a, _ = ss.fields(word)
        if not (op in ss.D_FORM_MEMORY or op == 14) or a != sum_reg:
            return False
        if op in (36, 37, 38, 39, 44, 45) and d == sum_reg:  # stores the address itself
            return False
        return cls.element_zero(base_offset + ss.simm(word), stride) is not None

    def deref_verdict(self, add_at: int, sum_reg: int, base_offset: int, cls: ArrayClass, stride: int,
                      verdict) -> Site:
        use = add_at + 4
        for _ in range(ss.BLOCK_LIMIT):
            word = self.image.code.get(use)
            if word is None or not ss.crosses(word, sum_reg):
                break
            op, _, a, _ = ss.fields(word)
            if (op in ss.D_FORM_MEMORY or op == 14) and a == sum_reg:
                offset = base_offset + ss.simm(word)
                array = cls.element_zero(offset, stride)
                if array is None:
                    return verdict("unproven", f"displacement 0x{offset & 0xFFFF:X} is not an array element")
                for other in sorted(reached_uses(self.image, add_at, sum_reg, self.graph)):
                    if not self.element_access(other, sum_reg, base_offset, cls, stride):
                        where = "a walk too long to follow" if other == ss.UNKNOWN else f"0x{other:08X}"
                        return verdict("unproven", f"element address also read at {where}")
                return verdict("scale", f"0x{array:X}")
            if ss.writes(word) == sum_reg:
                break
            use += 4
        return verdict("unproven", "element address not dereferenced in the block")

    # -- step sites ---------------------------------------------------------------------------

    def classify_step(self, site: int, cls: ArrayClass) -> Site | None:
        """None when the addi is not a pointer increment by an array stride."""
        word = self.image.code[site]
        op, rd, ra, _ = ss.fields(word)
        stride = ss.simm(word)
        if op != 14 or rd != ra or ra == 0 or all(stride != s for _, s in cls.arrays):
            return None

        def verdict(kind: str, offset: int, detail: str) -> Site:
            return Site(site, cls, kind, stride, offset, detail)

        definitions = ss.reaching_definitions(self.image, site, ra)
        if site not in definitions:
            return None  # not a loop increment
        inits = definitions - {site}
        values = {self.define(cls, d, 0) if d >= 0 else (None, 0) for d in inits}
        if len(values) != 1 or (None, 0) in values:
            return None  # walks something else (common: any 4-byte pointer walk)
        _, offset = values.pop()
        walk = inits | {site}
        start, end = self.image.function_range(site)
        # The walk may start past element 0 (a loop from index 1): `origin` is where the pointer
        # would be at element 0, which is what the runtime's window arithmetic keys on.
        origins = set()
        for use in range(start, end, 4):
            word = self.image.code.get(use)
            if word is None:
                continue
            uop, _, ua, ub = ss.fields(word)
            compare = uop in (10, 11) or (uop == 31 and ((word >> 1) & 0x3FF) in (0, 32))
            reads_base = (uop in ss.D_FORM_MEMORY and ua == ra) or (compare and ra in (ua, ub))
            if not reads_base or not ss.reaching_definitions(self.image, use, ra) <= walk:
                continue
            if compare:
                return verdict("unproven", offset, f"pointer compared at {use:08X} (end-pointer bound)")
            displacement = offset + ss.simm(word)
            element = cls.element_index(displacement, stride)
            if element is None:
                return verdict("unproven", offset, f"access at object+0x{displacement & 0xFFFF:X} ({use:08X}) is not an array element")
            origins.add(offset - element * stride)
        if not origins:
            return verdict("unproven", offset, "pointer never dereferenced")
        if len(origins) > 1:
            return verdict("unproven", offset, "accesses disagree on the element the walk starts at")
        origin = origins.pop()
        return Site(site, cls, "step", stride, origin, f"pointer from object+0x{offset:X}, element 0 at +0x{origin:X}")


def find_sites(analysis: Analysis, classes: list[ArrayClass]) -> list[Site]:
    image = analysis.image
    out = []
    analysis.classes = list(classes)
    for cls in classes:
        analysis.attested[(cls.instance_global, cls.member)] = attested_functions(cls.trace)
    for cls in classes:
        attested = analysis.attested[(cls.instance_global, cls.member)]
        roots = attested | {
            image.function_range(a)[0] for a, w in image.code.items()
            if w >> 26 == 32 and (w & 0xFFFF) == (cls.instance_global & 0xFFFF)}
        functions = set(roots)
        frontier = set(roots)
        for _ in range(CALLEE_DEPTH):
            frontier = {callee for function in frontier for callee in analysis.callees(function)} - functions
            functions |= frontier
        for function in sorted(functions):
            start, end = image.function_range(function)
            for address in range(start, end, 4):
                word = image.code.get(address)
                if word is None:
                    continue
                if scale_stride(word) is not None:
                    out.append(analysis.classify_scale(address, cls))
                elif word >> 26 == 14 and (site := analysis.classify_step(address, cls)) is not None:
                    out.append(site)
    # One instruction can be rewritten for one class only: a site two classes claim is unproven.
    claims: dict[int, int] = {}
    for site in out:
        if site.kind != "unproven":
            claims[site.address] = claims.get(site.address, 0) + 1
    return [Site(s.address, s.cls, "unproven", s.stride, s.offset, "claimed by more than one class")
            if s.kind != "unproven" and claims[s.address] > 1 else s for s in out]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dol", type=Path, required=True)
    parser.add_argument("--rel", type=Path, required=True)
    parser.add_argument("--function-map", type=Path, required=True)
    parser.add_argument("--classes", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True, help="site list for the translator")
    parser.add_argument("--report", type=Path, required=True, help="every candidate with its verdict")
    args = parser.parse_args()

    pointers: set[int] = set()
    code: dict[int, int] = {}
    sections = limit_scan.dol_text_sections(args.dol.read_bytes())
    sections += ss.relocated_rel_text(args.rel.read_bytes(), limit_scan.REL_LOAD_ADDRESS, pointers)
    for base, blob in sections:
        for offset in range(0, len(blob) - 3, 4):
            code[base + offset] = struct.unpack_from(">I", blob, offset)[0]
    image = ss.Image(code, ss.load_starts(args.function_map), {})
    analysis = Analysis(image, pointers | dol_data_words(args.dol.read_bytes()))
    sites = find_sites(analysis, load_classes(args.classes))
    proven = [s for s in sites if s.kind != "unproven"]
    with args.out.open("w") as out:
        out.write("# per-player array sites in singletons; generated by scripts/mkw100/array_sites.py\n")
        out.write("# address  class-id  kind  stride  offset  # class detail\n")
        for s in proven:
            out.write(f"0x{s.address:08X}  {s.cls.id}  {s.kind}  {s.stride}  0x{s.offset:X}  # {s.cls.name} {s.detail}\n")
    with args.report.open("w") as report:
        report.write("address\tclass\tverdict\tstride\tdetail\n")
        for s in sites:
            report.write(f"{s.address:08X}\t{s.cls.name}\t{s.kind}\t{s.stride}\t{s.detail}\n")
    kinds = {k: sum(s.kind == k for s in sites) for k in ("scale", "step", "unproven")}
    print(f"{len(sites)} candidate sites: {kinds['scale']} scale, {kinds['step']} step, "
          f"{kinds['unproven']} unproven -> {args.out}, {args.report}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
