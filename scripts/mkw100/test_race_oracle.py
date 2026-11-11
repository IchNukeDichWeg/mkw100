#!/usr/bin/env python3
"""Pins race_oracle.py's log parsing and its HOME reset to the oracle's output format."""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import race_oracle  # noqa: E402


class ParseTests(unittest.TestCase):
    def test_checkpoint_line(self):
        match = race_oracle.CHECKPOINT.search("[race-oracle] frame 1200 trajectory fd8e2604fd565c7c lag 0")
        self.assertEqual((match[1], match[2], match[3]), ("1200", "fd8e2604fd565c7c", "0"))

    def test_result_line(self):
        match = race_oracle.RESULT.search("[race-oracle] result fa906b50b69477e8 trajectory f1a83d9a9ca37311")
        self.assertEqual((match[1], match[2]), ("fa906b50b69477e8", "f1a83d9a9ca37311"))

    def test_racer_line_with_and_without_time(self):
        timed = race_oracle.RACER.search("[race-oracle] racer  0 place  6 time 1:40.349")
        self.assertEqual((timed[1], timed[2], timed[3]), ("0", "6", "1:40.349"))
        untimed = race_oracle.RACER.search("[race-oracle] racer 11 place 12 time none")
        self.assertEqual(untimed[3], "none")


class RuntimeArgsTests(unittest.TestCase):
    def test_lockstep_flag_follows_the_switch(self):
        self.assertEqual(race_oracle.runtime_args(True), ["--race-cpu", "--headless", "--lockstep"])
        self.assertEqual(race_oracle.runtime_args(False), ["--race-cpu", "--headless"])

    def test_players_flag_only_off_the_default(self):
        self.assertNotIn("--race-players=12", race_oracle.runtime_args(True, 12))
        self.assertEqual(race_oracle.runtime_args(True, 24)[-1], "--race-players=24")


class PrepareHomeTests(unittest.TestCase):
    def test_keeps_only_cache_and_writes_config(self):
        with tempfile.TemporaryDirectory() as tmp:
            home = Path(tmp)
            data = home / "Library" / "Application Support" / "WiiCompiled"
            (data / "Cache").mkdir(parents=True)
            (data / "Cache" / "pipeline_cache.db").write_text("warm")
            (data / "NAND" / "title").mkdir(parents=True)
            (data / "imgui.ini").write_text("x")
            cache = race_oracle.prepare_home(home, Path("/dvd/DATA"))
            self.assertEqual(cache, data / "Cache")
            self.assertEqual(sorted(p.name for p in data.iterdir()), ["Cache", "Config.toml"])
            self.assertEqual((data / "Cache" / "pipeline_cache.db").read_text(), "warm")
            self.assertIn('dvd_root = "/dvd/DATA"', (data / "Config.toml").read_text())

    def test_restores_seed_nand_over_previous_saves(self):
        with tempfile.TemporaryDirectory() as tmp:
            home = Path(tmp) / "home"
            seed = Path(tmp) / "seed"
            (seed / "title").mkdir(parents=True)
            (seed / "title" / "rksys.dat").write_text("seed")
            data = home / "Library" / "Application Support" / "WiiCompiled"
            (data / "NAND" / "title").mkdir(parents=True)
            (data / "NAND" / "title" / "rksys.dat").write_text("changed by last run")
            race_oracle.prepare_home(home, Path("/dvd/DATA"), seed)
            self.assertEqual((data / "NAND" / "title" / "rksys.dat").read_text(), "seed")


if __name__ == "__main__":
    unittest.main()
