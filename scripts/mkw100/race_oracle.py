#!/usr/bin/env python3
"""Run the headless race oracle and compare it with the committed baseline.

Boots `WiiCompiled --race-cpu --headless` (all-CPU fixed-seed VS race, see
runtime/src/hle/race_oracle.cpp) under its own HOME (build-macos/oracle-home)
so the user's real Config.toml and NAND are never touched, streams progress,
and checks the result and trajectory hashes against
docs/mkw100/oracle-baseline.json. Runs with the runtime's --lockstep (one VI
retrace per produced frame from race start): same hashes as wall-clock
pacing, measured 57.5 s against 110 s per run on the M5 Pro.

Exit codes: 0 match, 1 mismatch, 2 race never finished or crashed.
Lag frames are reported, not judged: 6,015 of them left the hashes unchanged.

Usage:
  scripts/mkw100/race_oracle.py [--binary build-macos/WiiCompiled]
      [--dvd-root Assets/DATA] [--home build-macos/oracle-home]
      [--seed-nand build-macos/oracle-seed-nand] [--wall-clock]
      [--baseline docs/mkw100/oracle-baseline.json] [--log FILE] [--players N]
      [--runs N] [--write-baseline]
"""
from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

CHECKPOINT = re.compile(r"\[race-oracle\] frame (\d+) trajectory ([0-9a-f]{16}) lag (\d+)")
FINISHED = re.compile(r"\[race-oracle\] finished at race frame (\d+), (\d+) racers")
LAG = re.compile(r"\[race-oracle\] lag frames (\d+) of (\d+)")
RESULT = re.compile(r"\[race-oracle\] result ([0-9a-f]{16}) trajectory ([0-9a-f]{16})")
RACER = re.compile(r"\[race-oracle\] racer +(\d+) place +(\d+) time (\S+)")

CONFIG = """[video]
widescreen = true
resolution_multiplier = 1.0
graphics_api = "metal"

[paths]
dvd_root = "{dvd_root}"
"""


def fmt_seconds(seconds: float) -> str:
    seconds = int(seconds)
    return f"{seconds // 60}:{seconds % 60:02d}"


def prepare_home(home: Path, dvd_root: Path, seed_nand: Path | None = None) -> Path:
    """Reset the oracle HOME to Config.toml, the renderer cache and the seed NAND.

    The cache is kept on purpose: a cold GPU pipeline cache stalls the boot
    race on shader compiles, the stall shows up as lag frames, and one lag
    frame changes the race (measured, see race_oracle.cpp). The NAND is
    restored from the seed snapshot: without the save file rksys.dat the boot
    race takes another path and leaves the race at race frame 2,341 with no
    finish (measured 10.11.2026), so every run must start from the same saves.
    """
    data_dir = home / "Library" / "Application Support" / "WiiCompiled"
    data_dir.mkdir(parents=True, exist_ok=True)
    for entry in data_dir.iterdir():
        if entry.name == "Cache":
            continue
        if entry.is_dir():
            shutil.rmtree(entry)
        else:
            entry.unlink()
    (data_dir / "Config.toml").write_text(CONFIG.format(dvd_root=dvd_root))
    if seed_nand is not None and seed_nand.exists():
        shutil.copytree(seed_nand, data_dir / "NAND")
    return data_dir / "Cache"


def runtime_args(lockstep: bool, players: int = 12) -> list[str]:
    return ["--race-cpu", "--headless", *(["--lockstep"] if lockstep else []),
            *([f"--race-players={players}"] if players != 12 else [])]


def run_once(binary: Path, dvd_root: Path, home: Path, seed_nand: Path | None, expected_frames: int | None,
             label: str, lockstep: bool = False, log: Path | None = None, players: int = 12) -> dict:
    cache = prepare_home(home, dvd_root, seed_nand)
    if not cache.exists():
        print(f"{label}: renderer cache is cold; shader compiles will cause lag frames this run")
    started = time.monotonic()
    process = subprocess.Popen(
        [str(binary), *runtime_args(lockstep, players)],
        env={"HOME": str(home), "PATH": "/usr/bin:/bin"},
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    outcome: dict = {"racers": [], "checkpoints": []}
    tty = sys.stdout.isatty()
    assert process.stdout is not None
    log_file = log.open("w") if log else None
    for line in process.stdout:
        if log_file:
            log_file.write(line)
        if match := CHECKPOINT.search(line):
            frame = int(match[1])
            outcome["checkpoints"].append([frame, match[2]])
            elapsed = time.monotonic() - started
            if expected_frames:
                share = min(frame / expected_frames, 1.0)
                eta = elapsed / share * (1 - share) if share else 0
                status = (f"{label} race frame {frame:,}/{expected_frames:,} ({share:.0%}) "
                          f"lag {match[3]} elapsed {fmt_seconds(elapsed)} ETA {fmt_seconds(eta)}")
            else:
                status = f"{label} race frame {frame:,} lag {match[3]} elapsed {fmt_seconds(elapsed)}"
            print(("\r" + status) if tty else status, end="" if tty else "\n", flush=True)
        elif match := FINISHED.search(line):
            outcome["race_frames"], outcome["racer_count"] = int(match[1]), int(match[2])
        elif match := RACER.search(line):
            outcome["racers"].append([int(match[1]), int(match[2]), match[3]])
        elif match := LAG.search(line):
            outcome["lag_frames"] = int(match[1])
        elif match := RESULT.search(line):
            outcome["result"], outcome["trajectory"] = match[1], match[2]
    process.wait()
    if log_file:
        log_file.close()
    if tty:
        print()
    outcome["exit"] = process.returncode
    outcome["wall_seconds"] = round(time.monotonic() - started, 1)
    return outcome


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", type=Path, default=REPO / "build-macos" / "WiiCompiled")
    parser.add_argument("--dvd-root", type=Path, default=REPO / "Assets" / "DATA")
    parser.add_argument("--baseline", type=Path, help="default docs/mkw100/oracle-baseline.json for 12 racers, "
                        "oracle-baseline-<N>.json otherwise")
    parser.add_argument("--home", type=Path, default=REPO / "build-macos" / "oracle-home",
                        help="persistent HOME for the oracle; only its renderer cache survives a run")
    parser.add_argument("--seed-nand", type=Path, default=REPO / "build-macos" / "oracle-seed-nand",
                        help="NAND snapshot restored before every run; created by a seeding run if missing")
    parser.add_argument("--runs", type=int, default=1, help="sequential runs; all must agree")
    parser.add_argument("--wall-clock", action="store_true",
                        help="pace VI retraces by the wall clock instead of the runtime's --lockstep")
    parser.add_argument("--players", type=int, default=12,
                        help="racers (13+ use the overflow slots); baseline defaults to oracle-baseline-<N>.json")
    parser.add_argument("--log", type=Path, help="write the runtime's full output of the last run to this file")
    parser.add_argument("--write-baseline", action="store_true", help="record this result as the baseline")
    args = parser.parse_args()

    if not args.binary.exists():
        print(f"no binary at {args.binary}", file=sys.stderr)
        return 2
    dvd_root = args.dvd_root.resolve()
    if not (dvd_root / "sys" / "fst.bin").exists():
        print(f"{dvd_root} has no sys/fst.bin; point --dvd-root at the extracted DATA partition", file=sys.stderr)
        return 2
    if args.baseline is None:
        name = "oracle-baseline.json" if args.players == 12 else f"oracle-baseline-{args.players}.json"
        args.baseline = REPO / "docs" / "mkw100" / name
    baseline = json.loads(args.baseline.read_text()) if args.baseline.exists() else None

    home = args.home.resolve()
    seed_nand = args.seed_nand.resolve()
    if not seed_nand.exists():
        # The game writes rksys.dat on its first boot; that run itself is not comparable.
        print("seeding: no seed NAND yet, one run creates the save file (about 4 min, not scored)")
        seeding = run_once(args.binary, dvd_root, home, None, None, "seed run")
        created = home / "Library" / "Application Support" / "WiiCompiled" / "NAND"
        save = created / "title" / "00010004" / "524d4350" / "data" / "rksys.dat"
        if not save.exists():
            print(f"FAIL: seeding run (exit {seeding['exit']}) did not create {save}")
            return 2
        shutil.copytree(created, seed_nand)
        print(f"seeded {seed_nand}")

    outcomes = []
    for run in range(1, args.runs + 1):
        outcome = run_once(args.binary, dvd_root, home, seed_nand, baseline and baseline.get("race_frames"),
                           f"run {run}/{args.runs}", not args.wall_clock, args.log, args.players)
        outcomes.append(outcome)
        print(f"run {run}/{args.runs}: exit {outcome['exit']}, {outcome.get('racer_count', '?')} racers, "
              f"race frame {outcome.get('race_frames', '?')}, lag {outcome.get('lag_frames', '?')}, "
              f"result {outcome.get('result', 'none')} trajectory {outcome.get('trajectory', 'none')}, "
              f"{outcome['wall_seconds']} s wall")
        if "result" not in outcome:
            print("FAIL: race did not finish (see exit code)")
            return 2

    first = outcomes[0]
    if any((o["result"], o["trajectory"]) != (first["result"], first["trajectory"]) for o in outcomes[1:]):
        print("MISMATCH between runs of the same binary: the race is not deterministic")
        return 1

    if args.write_baseline:
        record = {key: first[key] for key in ("racer_count", "race_frames", "result", "trajectory", "racers",
                                              "checkpoints")}
        record["course"] = "0x08 Luigi Circuit, 150cc, --race-cpu fixed seeds"
        args.baseline.write_text(json.dumps(record, indent=1) + "\n")
        print(f"wrote baseline {args.baseline}")
        return 0

    if baseline is None:
        print(f"no baseline at {args.baseline}; run with --write-baseline first")
        return 2
    if (first["result"], first["trajectory"]) == (baseline["result"], baseline["trajectory"]):
        print("MATCH baseline")
        return 0
    print(f"MISMATCH baseline: expected result {baseline['result']} trajectory {baseline['trajectory']}")
    for frame, value in first["checkpoints"]:
        expected = dict(baseline["checkpoints"]).get(frame)
        if expected != value:
            print(f"first differing checkpoint: race frame {frame} (expected {expected}, got {value})")
            break
    return 1


if __name__ == "__main__":
    sys.exit(main())
