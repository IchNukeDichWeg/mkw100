#!/usr/bin/env python3
"""Tests for array_sites.py on hand-assembled PowerPC (no game files needed)."""

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import array_sites as arrays  # noqa: E402
import slot_sites as ss  # noqa: E402

GLOBAL = 0x809C3648


def d_form(op, rd, ra, imm):
    return (op << 26) | (rd << 21) | (ra << 16) | (imm & 0xFFFF)


def lis(rd, imm):
    return d_form(15, rd, 0, imm)


def lwz(rd, ra, imm):
    return d_form(32, rd, ra, imm)


def mulli(rd, ra, imm):
    return d_form(7, rd, ra, imm)


def add(rd, ra, rb):
    return (31 << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (266 << 1)


def lwzx(rd, ra, rb):
    return (31 << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (23 << 1)


def stw(rs, ra, imm):
    return d_form(36, rs, ra, imm)


def li(rd, imm):
    return d_form(14, rd, 0, imm)


def bl(at, target):
    return (18 << 26) | ((target - at) & 0x3FFFFFC) | 1


BLR = 0x4E800020


def high_adjusted(address):
    return ((address + 0x8000) >> 16) & 0xFFFF


def classes_file(text):
    handle = tempfile.NamedTemporaryFile("w", suffix=".tsv", delete=False)
    handle.write(text)
    handle.close()
    return Path(handle.name)


class LoadClassesTest(unittest.TestCase):
    def test_six_columns_have_no_constructor(self):
        path = classes_file("# comment\n6 item_manager 0x809C3618 0x430 0x18:4 -\n")
        (cls,) = arrays.load_classes(path)
        self.assertEqual((cls.id, cls.instance_global, cls.arrays, cls.trace, cls.ctor),
                         (6, 0x809C3618, ((0x18, 4),), None, None))

    def test_seventh_column_is_the_constructor(self):
        path = classes_file("6 item_manager 0x809C3618 0x430 0x18:4 - 0x807992D8\n")
        (cls,) = arrays.load_classes(path)
        self.assertEqual(cls.ctor, 0x807992D8)

    def test_other_column_counts_are_rejected(self):
        with self.assertRaises(ValueError):
            arrays.load_classes(classes_file("6 item_manager 0x809C3618 0x430\n"))


def analysis_for(functions, address_taken=()):
    """functions: {start: [words]}; returns an Analysis over them."""
    code = {}
    for start, words in functions.items():
        for i, word in enumerate(words):
            code[start + i * 4] = word
    image = ss.Image(code, set(functions), {})
    return arrays.Analysis(image, set(address_taken))


CLASS = arrays.ArrayClass(7, "gesso_manager", GLOBAL, None, 0x54, ((0x10, 4),), None)


class ConstructorTest(unittest.TestCase):
    def test_constructor_this_is_the_instance(self):
        ctor = 0x80001000
        analysis = analysis_for({ctor: [BLR]})
        with_ctor = arrays.ArrayClass(**{**CLASS.__dict__, "ctor": ctor})
        self.assertTrue(analysis.this_is_instance(with_ctor, ctor))

    def test_uncalled_method_is_not(self):
        method = 0x80001000
        analysis = analysis_for({method: [BLR]})
        self.assertFalse(analysis.this_is_instance(CLASS, method))


class CalleeDepthTest(unittest.TestCase):
    # root loads the instance and calls first(this), which calls second(this), which indexes
    # ink[i] with `mulli; add; lwz 0x10`.
    ROOT, FIRST, SECOND = 0x80002000, 0x80003000, 0x80004000

    def functions(self):
        return {
            self.ROOT: [lis(3, high_adjusted(GLOBAL)), lwz(3, 3, GLOBAL & 0xFFFF), bl(self.ROOT + 8, self.FIRST), BLR],
            self.FIRST: [bl(self.FIRST, self.SECOND), BLR],
            self.SECOND: [mulli(4, 5, 4), add(4, 3, 4), lwz(6, 4, 0x10), BLR],
        }

    def test_second_level_callee_site_is_proven(self):
        analysis = analysis_for(self.functions(), address_taken={self.ROOT})
        sites = {site.address: site for site in arrays.find_sites(analysis, [CLASS])}
        self.assertEqual(sites[self.SECOND].kind, "scale")

    def test_one_level_misses_it(self):
        analysis = analysis_for(self.functions(), address_taken={self.ROOT})
        saved, arrays.CALLEE_DEPTH = arrays.CALLEE_DEPTH, 1
        try:
            found = {site.address for site in arrays.find_sites(analysis, [CLASS])}
        finally:
            arrays.CALLEE_DEPTH = saved
        self.assertNotIn(self.SECOND, found)


class AttestedThisTest(unittest.TestCase):
    # A function the access trace saw in the object indexes ink[i] off `this` (r3 on entry).
    METHOD = 0x80005000
    SITE = [mulli(4, 5, 4), add(4, 3, 4), lwz(6, 4, 0x10)]

    def sites(self, words):
        trace = classes_file("function\toffset\tsize\tkind\tcount\n"
                             f"func_{self.METHOD:08X}\t0x10\t4\tR\t1\n")
        attested = arrays.ArrayClass(**{**CLASS.__dict__, "trace": trace})
        analysis = analysis_for({self.METHOD: words + [BLR]})
        return {site.address: site.kind for site in arrays.find_sites(analysis, [attested])}

    def test_attested_this_is_trusted(self):
        self.assertEqual(self.sites(self.SITE)[self.METHOD], "scale")

    def test_not_when_the_function_loads_the_instance_itself(self):
        # The trace is explained by the global load; `this` is some other object (Item::PowMgr's
        # update reads DriverMgr through its global).
        load = [lis(7, high_adjusted(GLOBAL)), lwz(7, 7, GLOBAL & 0xFFFF)]
        self.assertEqual(self.sites(self.SITE + load)[self.METHOD], "unproven")


class ScaledIndexReuseTest(unittest.TestCase):
    # A function loads the instance and indexes ink[i] (0x10) with `mulli r4; add r6, r3, r4`.
    # MKW100_IndexScaled turns r4 into a window offset of this class, so every other read of r4
    # must index the instance again.
    ROOT, CALLEE = 0x80006000, 0x80007000
    TWO_ARRAYS = arrays.ArrayClass(**{**CLASS.__dict__, "arrays": ((0x10, 4), (0x40, 4))})
    LOAD = [lis(3, high_adjusted(GLOBAL)), lwz(3, 3, GLOBAL & 0xFFFF)]
    SITE = [mulli(4, 5, 4), add(6, 3, 4), lwz(7, 6, 0x10)]

    def kind(self, words, callee=(BLR,)):
        analysis = analysis_for({self.ROOT: self.LOAD + words + [BLR], self.CALLEE: list(callee)})
        sites = {site.address: site for site in arrays.find_sites(analysis, [self.TWO_ARRAYS])}
        return sites[self.ROOT + 4 * len(self.LOAD)].kind

    def test_reuse_for_another_base_is_unproven(self):
        # DriverMgr::CalcFinishType: finishType[i]'s offset also indexes Raceinfo's players.
        self.assertEqual(self.kind(self.SITE + [lwzx(8, 9, 4)]), "unproven")

    def test_reuse_for_another_array_of_the_instance_is_proven(self):
        # DriverMgr::Update: frameCounter[i] and the +0x130 array share one window.
        self.assertEqual(self.kind(self.SITE + [add(8, 3, 4), stw(7, 8, 0x40)]), "scale")

    def test_overwriting_the_index_is_not_a_read(self):
        self.assertEqual(self.kind(self.SITE + [add(4, 9, 10), lwz(7, 4, 0)]), "scale")

    def test_call_whose_callee_ignores_the_register_is_not_a_read(self):
        call = [bl(self.ROOT + 4 * (len(self.LOAD) + 3), self.CALLEE)]
        self.assertEqual(self.kind(self.SITE + call, callee=(li(4, 0), BLR)), "scale")
        self.assertEqual(self.kind(self.SITE + call, callee=(lwz(5, 4, 0), BLR)), "unproven")


if __name__ == "__main__":
    unittest.main()
