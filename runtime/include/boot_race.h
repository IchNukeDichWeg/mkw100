#pragma once

#include <string_view>

// `--race[=<course id>]`: boot straight into a 1P VS race instead of the title screen.
// `--race-cpu`: the same, with the local racer driven by the CPU.
// `--race-players=N`: race with N racers (default 12); 13+ use the overflow player slots.
namespace BootRace {
// False when `arg` is not a boot-race option; throws on a malformed one.
bool ParseArgument(std::string_view arg);
bool Requested();
// `--race-cpu`: every racer is CPU driven with fixed RNG seeds, so the race is repeatable.
bool CpuDriven();
// `--lockstep` (with --race-cpu): from the first race frame on, VI retraces follow produced frames
// instead of the wall clock (VI_HLE_EnableLockstep), so a slow host cannot change the race.
bool Lockstep();
// `--heap-check`: walk every ExpHeap whenever the game switches heap group ids (between the
// race scene's subsystem creations) and name the group whose work broke a heap
// (runtime/src/hle/heap_check.cpp). A diagnostic for N-player overruns; slow.
bool HeapCheck();
} // namespace BootRace
