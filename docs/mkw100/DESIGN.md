# mkw100: 100-player races on WiiCompiled

Fork of WiiCompiled v0.9.3 (`patchzyy/Wiicompiled`, GPL-3.0), branch `mkw100`.
Goal: real races with up to 100 human players on one track, online.

Started 09.11.2026. This file is the plan of record; update it in the same
commit as any change that moves a milestone or reverses a decision.

## Why this is a rewrite and not a patch

WiiCompiled translates the PAL game (`main.dol` + `StaticR.rel`) function by
function into C++ but keeps the Wii memory map and every struct layout. The
12-player limit lives in four separate layers, and all four have to move:

| Layer | Where 12 lives | Measured / inferred |
|---|---|---|
| Race engine | inline `Player[12]` arrays (RaceConfig: 12 x 0xF0 at +0x8, settings start at +0xB48), `< 12` loops, `new T[12]`, per-player u32 bitmasks | measured: 1,098 functions hold candidate instructions (below) |
| Retro Rewind (Pulsar `Code.pul`) | its own hooks assume the same 12 slots | inferred, not scanned yet |
| Netcode | DWC P2P full mesh, aid bitmaps, RKNet per-aid packet slots | inferred from symbols (`DWC_GetDirectConnectedAIDBitmap`, `RKNet::RH1Handler::GetPlayerCount`) |
| Server | Retro WFC rooms cap at 12, and it is not our service | inferred |

A full mesh of 100 peers is 4,950 NAT-punched links with every client sending
to 99 others. That design is replaced, not widened (see Netcode).

## Instruments

- **Limit inventory:** `scripts/mkw100/limit_scan.py` decodes every PPC
  instruction in the DOL and the in-place REL and lists candidate sites per
  function in `docs/mkw100/limit-sites.tsv`. Baseline on 09.11.2026:
  29,792 functions in MAP.txt, 1,098 with candidates (787 named), 2,105
  instructions: `cmp12` 383, `cmp11` 158, `li12` 1,021, `mulF0` 543.
  Candidates, not confirmed sites: `li 12` is also a size or an offset. A site
  leaves the list only by being triaged in the TSV, never by memory.
- **Race oracle (M1):** `scripts/mkw100/race_oracle.py` runs
  `--race-cpu --headless` (12 CPUs, Luigi Circuit 150cc, fixed seeds) and
  compares against `docs/mkw100/oracle-baseline.json`. The hook in
  `runtime/src/hle/race_oracle.cpp` wraps `Raceinfo::Update`, hashes every
  racer's position, lap, completion and state flags each race frame (the
  trajectory, checkpointed every 600 frames for bisecting) and, at
  RACESTAGE_FINISHED, every racer's place and finish time (the result).
  Baseline 10.11.2026: finish at race frame 6,255, result `fa906b50b69477e8`,
  trajectory `f1a83d9a9ca37311`, reproduced by every run since. 56 s wall
  per run with `--lockstep` (the runner default), 110 s with `--wall-clock`.
  The N=12 hashes must not change while the engine is rewritten.
  What it depends on, all measured:
  - **Save file.** A fresh NAND has no `rksys.dat` and the boot race takes
    another path (leaves the race at race frame 2,341, no finish). The runner
    seeds a NAND once and restores it before every run.
  - **Renderer cache.** A cold GPU pipeline cache stalls on shader compiles.
    The runner keeps `build-macos/oracle-home/.../Cache` between runs.
  - **Not on host speed.** Guest time (VI retraces, time base, alarms)
    follows the host clock, so a late frame spans 2+ retraces (a lag frame).
    With an artificial 40 ms per race frame, 6,015 of 6,255 race frames
    lagged and both hashes still matched: the race steps once per frame, not
    once per retrace. `--lockstep` matched as well, with 0 lag.
- **Diagnostics for N-player overruns:** `--heap-check` walks every ExpHeap
  each time the race scene switches heap group ids and names the group whose
  allocations were written past their end (`runtime/src/hle/heap_check.cpp`).
  A guest access violation prints the host stack, which names the translated
  `func_XXXXXXXX` that made the access. A hang is located with `sample <pid>`.
- **Build:** clean base build 73 s on an Apple M5 Pro (18 cores, macOS 27.0.1,
  arm64), translation 12 s. Incremental builds are faster. Measured on that
  machine only.

## Approach

Mechanisms, chosen per site:

1. **Native replacement** (`REGISTER_NATIVE_FUNCTION_AS`, as
   `runtime/src/hle/boot_race.cpp` already does). Whole functions or subsystems
   that own per-player state are rewritten in C++ against an N-sized layout.
   MKW-SP (MIT, PAL symbols, `refs/mkw-sp`) and the `riidefi/mkw` decomp are
   the specification for what each function does.
2. **Translator rewrite table.** For the long tail of call sites that only read
   a field (`mulli rD, rA, 0xF0` into RaceConfig, `cmpwi rA, 12` loop bounds),
   a table of instruction addresses the translator rewrites before codegen.
   First instance, in use since 10.11.2026: `translation.player_slot_sites` in
   `projects/mkwii/recomp.yml` lists the 0xF0 sites that index Racedata player
   slots; each lifts to `MKW100_PlayerSlotOffset` instead of a multiply
   (`translator/src/Translator.Core/Lifting/PlayerSlotStrideSites.cs`). The
   list is generated, not hand-kept: `scripts/mkw100/slot_sites.py` proves each
   site by reaching definitions over the function's control flow (base is the
   Racedata global, a scenario offset of it, or `this` of a RaceConfig or
   Scenario method) and a dereference inside a player slot. 505 of 543 sites
   prove; the 38 left are menu, awards, credits, points (natively rewritten)
   and unrelated code. The bundled v0.9.3 translator does not know the key, so
   the fork's translator is built from source (`scripts/mkw100/build.sh`).
   CodeWarrior unrolls fixed player loops 8 or 6 ways (one product, then fixed
   displacements for players i+1..i+7); each such dereference is listed as a
   `deref` line and lifted through `MKW100_PlayerSlotAddress`, which maps the
   slot pointer back to its player and scenario (17 sites, 325 derefs).
3. **Singleton shadow windows.** Managers that keep inline `x[12]` arrays next
   to their own fields (AI::Manager and its objects, Input::Manager,
   Item::Manager, Item::GessoMgr, Race::DriverMgr, ArchiveMgr) cannot grow in place. Element i >= 12 of
   every array of such a class lives in shadow window i - 12 at 0x82000000
   (`runtime/include/guest_abi/array_overflow.h`).
   `scripts/mkw100/array_sites.py` proves the scale and step instructions that
   index them (the instance loaded from its global, a listed constructor's
   `this`, or a method up to 2 calls from the load) from
   `projects/mkwii/mkw100/array-classes.tsv`; the translator routes them
   through `MKW100_IndexScaled` / `MKW100_ArrayStep`. A scaled index becomes an
   offset into the class's window, so every other read of it (a def-use walk
   through calls and returns) must index the same instance again; one reused
   for another object stays unproven (DriverMgr::CalcFinishType, now native
   above 12). 70 sites over 9 classes. Struct layouts are checked against Pulsar's
   `GameSource` headers (`refs/Pulsar`) where they exist.
4. **Native above 12, original up to 12.** Functions that keep per-player
   state in stack arrays of 12 (HandlePositionTracking, the AI re-rankers, the
   item row remap) are wrapped: up to 12 players the game's function runs, so
   the oracle cannot move; above, a native version does the same work for N.
5. **Count clamps.** Where an array of 12 is indexed as plain `base + i` and
   the class's fields follow it, so no rewrite reaches it (RaceBalloons
   positions, PowMgr hit flags), the function runs with the count it reads
   lowered to 12. A stated gameplay or visual limit, listed below.
6. **Fixed runtime tables.** Tables sized for 12 that live as long as the game
   (the EGG effect manager's group slots, nw4r activity lists) move to fixed
   addresses in the last shadow span (`ArrayOverflow::kNativeTables`).

Runtime sources are built as unity files, so names in anonymous namespaces must
be unique across `runtime/src/hle` (prefix per file).

Layout decision for RacedataScenario, measured 10.11.2026 with the access
tracer (`MKW100_ACCESS_TRACE` build, `scripts/mkw100/access_report.py`) on the
oracle race, map in `docs/mkw100/racedata-access.tsv`: 233 functions touch the
three scenarios; 43 index the player slots, 185 read the settings at fixed
displacements (167 read nothing else, most of them kart physics on the hot
path), 11 touch the menu or award scenario. The plan was to grow the struct in
place unless displacements outnumbered index sites; they do, about 4 to 1. So
the 12 inline slots stay where they are, players 12..N-1 live in an overflow
table, and only the player indexers are rewritten to reach it. The trace covers
one offline VS race; GP, online, battle and award paths are not traced yet and
may add indexers.

Guest memory: the race scene's MEM1 object heap (structsMem1, 7 MB at the
top of MEM1) holds about 85 KB per racer and runs out at 78 racers. MEM1
cannot grow (the overflow slots and shadow windows sit above it), so past 12
players an allocation that fails there is made in the scene's MEM2 object heap
instead (`runtime/src/hle/race_heap.cpp`, at MEMAllocFromExpHeapEx, with frees
and resizes of those blocks sent to the heap that holds them).

Per-player u32 bitmasks cap at 32 and need their own pass. They are not in the
scanner yet.

## Netcode

Replace DWC matchmaking and the P2P mesh with a client to server relay: each
client sends its own state to the server, the server fans out a culled
snapshot (nearby racers at full rate, far racers at a lower rate). Hooked at
the RKNet packet layer so the race code above it keeps its interface. Our own
server, never Retro WFC: a 100-player client must not touch a service whose
rooms and rules assume 12.

## Milestones

| | Milestone | Done when |
|---|---|---|
| M0 | Fork builds and runs on macOS | done 09.11.2026: base build boots `--race-cpu --headless` into a 12-CPU race |
| M1 | Race oracle | done 10.11.2026: headless race exits at the finish, hashes stable over 4 runs |
| M1b | Lockstep clock | done 10.11.2026: `--lockstep` pays one VI retrace per presented frame from race start; hashes unchanged, oracle 2x faster |
| M2 | 24 karts offline | 24-CPU headless race finishes, N=12 hash unchanged. done 10.11.2026 (M5 Pro): `--race-players=24 --lockstep --heap-check` finishes at race frame 5,864, all 24 racers with real times (1:27.714 to 1:35.949), 0 lag, no heap reports; N=12 oracle unchanged. Deterministic since 10.11.2026: 6 of 6 `--lockstep` runs give result 0288f83ab7d1220a at race frame 5,681 (times 1:29.552 to 1:38.365), then `oracle-baseline-24.json`. Since the side-by-side start grid (10.11.2026): result 84ba229abd1f5dff at race frame 5,619 (1:28.643 to 1:35.343), 2 of 2 runs |
| M3 | 100 karts offline | 100-CPU race finishes, frame time measured. done 10.11.2026 (M5 Pro, 18 cores): `--race-players=100 --lockstep` finishes at race frame 5,932, all 100 racers with real times (1:28.867 to 1:42.875), 0 lag; `--heap-check` reports nothing; 4 of 4 runs give result bef33e7d8845128c, now `oracle-baseline-100.json`. Frame time, headless, 600-frame segments: best 11.1 ms, per-run means 14.3 to 25.3 ms at 100 karts (load average 7 to 18 from other jobs, so upper bounds); 12 and 24 karts sit on a flat 8.3 ms floor that looks like pacing, not engine cost (not investigated) |
| M4 | Relay netcode, 2 clients | two local clients race through our server |
| M5 | 100 clients | 100 headless clients on one server finish a race |
| M6 | UI | lobby, rankings, minimap and results for 100 |
| M7 | Retro Rewind | Pulsar code ported to the N-player layout |

Base game first, Retro Rewind last: Pulsar's hooks sit on top of the same
structs, so porting them before the engine layout settles means doing it twice.

## Known limits / still owed

- Retro Rewind's `Code.pul` has not been scanned.
- Bitmask and trailing-field displacement scans do not exist yet.
- Performance at 100 karts is measured only as headless frame time on a
  loaded machine (M3 row); where the time goes (AI, collision, rendering) is
  not profiled. Slow frames do not change an offline race (measured, above),
  but online play syncs on time, and a slow client there is unmeasured.
- 100 karts are tested on one course (0x08). The start grid's side-by-side
  blocks and the respawn spread depend on each course's checkpoints and
  respawn points.
- Lockstep gates only VI retraces. The time base, alarms and audio still
  follow the wall clock; nothing in the offline race was seen to depend on
  them, which is a measurement on one course, not a proof. A per-read counter
  clock did not remove the 24-racer run-to-run splits (the cause was heap
  layout, below).
- The oracle covers one course with items on. Its sensitivity is shown only
  indirectly (the fresh-NAND path changed both hashes); no deliberate
  one-racer perturbation has been run yet.
- Overflow player slots (`runtime/include/guest_abi/player_slots.h`) cover the
  0xF0 sites and the RaceConfig functions that walk a fixed 12 (construct,
  copy, count, initRace). Other fixed-12 RaceConfig walks (copyPrevPositions,
  points update, resetPlayers, initPlayers, awards) do not carry players 12+
  yet; GP, online and awards are untested past 12.
- The heap layout differs run to run: the race scene pads two allocations by
  a clock-seeded random size (0x8051AC40, `(rand(8) + 1) * 32` bytes each),
  and loading moves more. So any read past the end of a heap array past 12
  shows up as nondeterminism, not as a crash. That is how the AI per-position
  table overrun was found (`runtime/src/hle/ai_rank_params.cpp`); the N=24
  oracle baseline (`docs/mkw100/oracle-baseline-24.json`) is the check for the
  next one. One 24-racer launch exited 1 within a second with no log kept; it
  did not recur.
- Start grid past 12 karts (`runtime/src/hle/start_grid.cpp`): the 12-kart
  grid repeated in blocks of 12, side by side where the checkpoint quads have
  room and in block rows behind (course 0x08, 100 karts: 3 rows of 4). A design
  choice; the game has no grid data for 13+. The quads are flat, so a gap or
  level change inside one is not seen.
- DriverMgr: racePositions of players 12+ are not kept (read by
  CalcFinishType online only, so online players 12+ get their finish type from
  the Raceinfo position alone; online past 12 is untested). Past 12 players
  CalcFinishType (0x8078CFA4) runs natively: the game's threshold table
  (0x808D1820) has 12 rows, so the 12-player share of GOOD finishers is kept,
  ceil(table[11] * N / 12) (24: 2nd-10th GOOD, 11th-24th BAD; the game read
  past the table and gave BAD from 2nd). From 80 players its ctor's position
  loop overwrites the top-5 timers; their bytes are saved before the loop and
  put back after it.
- Gameplay and visual caps past 12, by design until replaced natively: item
  probabilities spread the 12 file rows (position i gets row i * 12 / N), and
  so do the CPU per-position driving chances (0x8073AC88);
  name balloons show players 0..11; the POW hits players 0..11 (by index, not
  rank); the GOOD finish type goes to the 12-player share of finishers;
  AI::Manager +0x98 is a `% 12` round robin; 0x8073C394 clamps AI ranks
  to 0..11. Effect draw loops may still walk 12 groups (not checked). When
  the race's scene light pool runs out (2 per kart; 48 karts fit, 64 did not)
  karts share lights round robin (`runtime/src/hle/light_pool.cpp`), so a
  shared light follows whichever owner updates it last. Respawn points spread
  karts by a 12-entry table (0x8088FA08); players past 12 take entry
  id % 12 (`runtime/src/hle/respawn_point.cpp`), so with 100 up to 9 share a
  spot.
- `array_sites.py` follows callees 2 calls deep; an array reached deeper is
  missed (reported as unproven, not rewritten). The translator's
  `ArrayOverflowSites` has no unit tests yet; it is covered only by the races.
- `slot_sites.py` does not follow `bctr` jump tables, so a site reached only
  through one could be proven from too few definitions. None is known.
- The translator tests ran on the .NET 10 runtime (roll-forward); the
  translator itself runs on .NET 8.0.33.
- No remote for this branch yet; `upstream` is patchzyy's repo and is never
  pushed to.
