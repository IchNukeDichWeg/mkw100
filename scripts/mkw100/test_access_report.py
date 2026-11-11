#!/usr/bin/env python3
"""Tests for access_report.py: offset classification and row folding (no atos needed)."""

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import access_report as report  # noqa: E402


class ClassifyTest(unittest.TestCase):
    def test_header(self):
        self.assertEqual(report.classify(0x4), ("race", "header", 0x4, None))

    def test_player_slot_and_field(self):
        # menu scenario, player 3, field 0x10 (type)
        offset = 0xBF0 + 0x8 + 3 * 0xF0 + 0x10
        self.assertEqual(report.classify(offset), ("menu", "player", 0x10, 3))

    def test_last_player_byte(self):
        self.assertEqual(report.classify(0xB47), ("race", "player", 0xEF, 11))

    def test_settings(self):
        self.assertEqual(report.classify(2 * 0xBF0 + 0xB48 + 0x2C), ("award", "settings", 0x2C, None))


class RowsTest(unittest.TestCase):
    def test_slots_fold_into_one_row(self):
        records = [((0x10, 0x20, 0), 0x8 + 0x10, 4, "R", 2), ((0x10, 0x20, 0), 0x8 + 0xF0 + 0x10, 4, "R", 3)]
        rows = report.build_rows(records, {0x10: "fn", 0x20: "caller"})
        self.assertEqual(rows, [("fn", "race", "player", "0x10", 4, "R", 5, "0,1")])

    def test_parse_reads_only_records(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "trace.log"
            log.write_text("[access-trace] 1 distinct accesses\n"
                           "[access-trace] pc 1000abc,20,0 offset b48 size 4 W count 7\nother\n")
            self.assertEqual(report.parse(log), [((0x1000ABC, 0x20, 0), 0xB48, 4, "W", 7)])

    def test_parse_selects_range(self):
        with tempfile.TemporaryDirectory() as tmp:
            log = Path(tmp) / "trace.log"
            log.write_text("[access-trace] range racedata pc 10,0,0 offset 8 size 4 R count 1\n"
                           "[access-trace] range ai_manager pc 20,0,0 offset 24 size 4 W count 2\n")
            self.assertEqual(report.parse(log), [((0x10, 0, 0), 0x8, 4, "R", 1)])
            self.assertEqual(report.parse(log, "ai_manager"), [((0x20, 0, 0), 0x24, 4, "W", 2)])
            self.assertEqual(report.ranges(log), ["ai_manager", "racedata"])
            rows = report.build_object_rows(report.parse(log, "ai_manager"), {0x20: "fn"})
            self.assertEqual(rows, [("fn", "0x24", 4, "W", 2)])

    def test_accessor_skips_memory_layer(self):
        names = {1: "Memory::Read32(unsigned int)", 2: "fn_80530000", 3: "outer",
                 4: "unsigned int MemoryInline::ReadResolvedFallback<unsigned int>(unsigned int)"}
        self.assertEqual(report.accessor((1, 2, 3), names), "fn_80530000")
        self.assertEqual(report.accessor((1, 4, 2), names), "fn_80530000")
        self.assertEqual(report.accessor((2, 3, 0), names), "fn_80530000")


if __name__ == "__main__":
    unittest.main(verbosity=1)
