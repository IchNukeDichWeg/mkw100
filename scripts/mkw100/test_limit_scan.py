#!/usr/bin/env python3
"""Pins limit_scan.classify to hand-assembled PowerPC encodings."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from limit_scan import classify  # noqa: E402


class ClassifyTests(unittest.TestCase):
    def test_compare_immediates(self):
        self.assertEqual(classify(0x2C03000C), "cmp12")  # cmpwi r3, 12
        self.assertEqual(classify(0x2803000C), "cmp12")  # cmplwi r3, 12
        self.assertEqual(classify(0x2C03000B), "cmp11")  # cmpwi r3, 11
        self.assertIsNone(classify(0x2C03000D))           # cmpwi r3, 13

    def test_load_immediate_needs_ra_zero(self):
        self.assertEqual(classify(0x3860000C), "li12")   # li r3, 12
        self.assertIsNone(classify(0x3864000C))           # addi r3, r4, 12 is not li

    def test_raceconfig_player_stride(self):
        self.assertEqual(classify(0x1C6400F0), "mulF0")  # mulli r3, r4, 0xF0
        self.assertIsNone(classify(0x1C6400F4))           # mulli r3, r4, 0xF4

    def test_negative_immediate_is_not_a_hit(self):
        self.assertIsNone(classify(0x2C03FFF4))           # cmpwi r3, -12


if __name__ == "__main__":
    unittest.main()
