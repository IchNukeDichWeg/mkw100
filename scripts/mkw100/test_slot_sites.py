#!/usr/bin/env python3
"""Tests for slot_sites.py on hand-assembled PowerPC (no game files needed)."""

import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import slot_sites as sites  # noqa: E402


def d_form(op, rd, ra, imm):
    return (op << 26) | (rd << 21) | (ra << 16) | (imm & 0xFFFF)


def lis(rd, imm):
    return d_form(15, rd, 0, imm)


def li(rd, imm):
    return d_form(14, rd, 0, imm)


def addi(rd, ra, imm):
    return d_form(14, rd, ra, imm)


def lwz(rd, ra, imm):
    return d_form(32, rd, ra, imm)


def mulli(rd, ra, imm):
    return d_form(7, rd, ra, imm)


def add(rd, ra, rb):
    return (31 << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (266 << 1)


def mr(ra, rs):
    return (31 << 26) | (rs << 21) | (ra << 16) | (rs << 11) | (444 << 1)


def b(offset):
    return (18 << 26) | (offset & 0x03FFFFFC)


def beq(offset):
    return (16 << 26) | (12 << 21) | (2 << 16) | (offset & 0xFFFC)


def bl(offset):
    return b(offset) | 1


BLR = 0x4E800020
START = 0x80100000
RACEDATA_LO = 0xD728  # -0x28D8: lis 0x809C + this = Racedata::spInstance


def image(words, receivers=None):
    code = {START + 4 * i: w for i, w in enumerate(words)}
    return sites.Image(code, {START, START + 4 * len(words)}, receivers or {})


def verdict(words, receivers=None):
    img = image(words, receivers)
    site = next(a for a, w in img.code.items() if w >> 26 == 7)
    return sites.classify_site(img, site)


class ClassifySiteTest(unittest.TestCase):
    def test_racedata_global_race_slot(self):
        self.assertEqual(verdict([lis(3, 0x809C), mulli(0, 4, 0xF0), lwz(3, 3, RACEDATA_LO),
                                  add(3, 3, 0), lwz(3, 3, 0x38), BLR]), ("racedata", "race"))

    def test_menu_slot(self):
        self.assertEqual(verdict([lis(3, 0x809C), mulli(0, 4, 0xF0), lwz(3, 3, RACEDATA_LO),
                                  add(3, 0, 3), lwz(3, 3, 0xC18 + 0x10), BLR]), ("racedata", "menu"))

    def test_settings_displacement_is_not_a_slot(self):
        kind, detail = verdict([lis(3, 0x809C), mulli(0, 4, 0xF0), lwz(3, 3, RACEDATA_LO),
                                add(3, 3, 0), lwz(3, 3, 0xB70), BLR])
        self.assertEqual(kind, "unproven")
        self.assertIn("not a player slot", detail)

    def test_other_global_is_unproven(self):
        kind, _ = verdict([lis(3, 0x809C), mulli(0, 4, 0xF0), lwz(3, 3, 0xD730),
                           add(3, 3, 0), lwz(3, 3, 0x38), BLR])
        self.assertEqual(kind, "unproven")

    def test_scenario_receiver(self):
        words = [mulli(0, 4, 0xF0), add(5, 3, 0), lwz(3, 5, 0x10), BLR]
        self.assertEqual(verdict(words, {START: "scenario"}), ("racedata", "scenario"))
        self.assertEqual(verdict(words)[0], "unproven")

    def test_diamond_agreeing_definitions(self):
        # r31 = Racedata on both arms of an if/else, then the site.
        words = [lis(30, 0x809C), beq(12), lwz(31, 30, RACEDATA_LO), b(8),
                 lwz(31, 30, RACEDATA_LO), mulli(0, 4, 0xF0), add(3, 31, 0), lwz(3, 3, 0x38), BLR]
        self.assertEqual(verdict(words), ("racedata", "race"))

    def test_diamond_disagreeing_definitions(self):
        words = [lis(30, 0x809C), beq(12), lwz(31, 30, RACEDATA_LO), b(8),
                 li(31, 0), mulli(0, 4, 0xF0), add(3, 31, 0), lwz(3, 3, 0x38), BLR]
        self.assertEqual(verdict(words)[0], "unproven")

    def test_call_clobbers_volatile_base(self):
        words = [lis(3, 0x809C), lwz(5, 3, RACEDATA_LO), bl(0x100), mulli(0, 4, 0xF0),
                 add(3, 5, 0), lwz(3, 3, 0x38), BLR]
        self.assertEqual(verdict(words)[0], "unproven")

    def test_callee_saved_base_survives_call(self):
        words = [lis(3, 0x809C), lwz(29, 3, RACEDATA_LO), bl(0x100), mulli(0, 4, 0xF0),
                 mr(6, 29), add(3, 6, 0), lwz(3, 3, 0x38), BLR]
        self.assertEqual(verdict(words), ("racedata", "race"))

    def test_get_player_pointer(self):
        # Scenario::getPlayer: mulli r0,r4,0xF0; add r3,r3,r0; addi r3,r3,8; blr
        words = [mulli(0, 4, 0xF0), add(3, 3, 0), addi(3, 3, 8), BLR]
        self.assertEqual(verdict(words, {START: "scenario"}), ("racedata", "scenario"))

    def test_scenario_offset_via_addi(self):
        words = [lis(3, 0x809C), lwz(3, 3, RACEDATA_LO), addi(31, 3, 0xC10), mulli(0, 4, 0xF0),
                 add(3, 31, 0), lwz(3, 3, 0x8 + 0x10), BLR]
        self.assertEqual(verdict(words), ("racedata", "menu"))


def stb(rs, ra, imm):
    return d_form(38, rs, ra, imm)


def stbu(rs, ra, imm):
    return d_form(39, rs, ra, imm)


class UnrolledLoopTest(unittest.TestCase):
    """A product for player i, then fixed displacements for players i, i+1 (CodeWarrior unrolling)."""

    HEAD = [lis(3, 0x809C), lwz(3, 3, RACEDATA_LO), mulli(0, 4, 0xF0), add(5, 3, 0)]

    def classify(self, tail):
        img = image(self.HEAD + tail + [BLR])
        site = START + 8
        return sites.classify_site(img, site), img.derefs.get(site)

    def test_later_players_list_every_dereference(self):
        verdict, derefs = self.classify([stb(6, 5, 0x28 + 0xE0), stb(6, 5, 0x28 + 0xF0 + 0xE0)])
        self.assertEqual(verdict, ("racedata", "race"))
        self.assertEqual(derefs, [START + 16, START + 20])

    def test_single_player_lists_none(self):
        verdict, derefs = self.classify([stb(6, 5, 0x28 + 0xE0), stb(6, 5, 0x28 + 0xE9)])
        self.assertEqual(verdict, ("racedata", "race"))
        self.assertEqual(derefs, [])

    def test_update_form_is_unproven(self):
        verdict, _ = self.classify([stb(6, 5, 0x28 + 0xE0), stbu(6, 5, 0x28 + 0xF0 + 0xE0)])
        self.assertEqual(verdict[0], "unproven")

    def test_later_settings_dereference_is_unproven(self):
        verdict, _ = self.classify([stb(6, 5, 0x28 + 0xE0), stb(6, 5, 0xB70)])
        self.assertEqual(verdict[0], "unproven")
        self.assertIn("not a player slot", verdict[1])

    def test_redefinition_ends_the_scan(self):
        verdict, derefs = self.classify([stb(6, 5, 0x28 + 0xE0), li(5, 0), stb(6, 5, 0xB70)])
        self.assertEqual(verdict, ("racedata", "race"))
        self.assertEqual(derefs, [])


class LoadReceiversTest(unittest.TestCase):
    def test_receiver_classes(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "symbols.txt"
            path.write_text(
                "getHudPlayerId__Q26System10RaceConfigFUc = .text:0x00000010; // type:function size:0x10\n"
                "computePlayerCounts__Q36System10RaceConfig8ScenarioFRUcRUcRUc = .text:0x00000020; // type:function\n"
                "setCharacter__Q36System10RaceConfig6PlayerFQ26System11CharacterId = .text:0x00000030; // type:function\n"
                "lbl_1 = .data:0x00000040; // type:object\n")
            self.assertEqual(sites.load_receivers(path, 0x80510000),
                             {0x80510010: "racedata", 0x80510020: "scenario"})


if __name__ == "__main__":
    unittest.main(verbosity=1)
