// mkw100: the CPU driving parameters per race position for races of more than 12 players.
//
// 0x8073AC88 (on the object at AI::Manager +0x8C) returns entry `index` of an 8-byte table of
// per-position chances (drift, mini-turbo, trick, wheelie, ...; first place 100, last 10). Its 8
// callers, all in ParamActionCalculator (0x8073EEE0..0x8073F368), pass the racer's race position
// - 1 and compare a draw of rand(100) against one byte of the entry. The table has 12 entries
// and is followed on the heap by another block, so positions 13+ read the next block's header
// and object: bytes of heap pointers, which change with the heap layout. The game pads two
// race-scene allocations by a clock-seeded random size (0x8051AC40), so 24-CPU `--lockstep`
// races split run to run (traced: CalcGotMT read the low byte of a pointer for position 15).
//
// With more than 12 players the position is mapped onto the 12 entries proportionally,
// index * 12 / count: 1st keeps the leader's entry and last the last entry. Up to 12 players the
// game's function runs unchanged.

#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "memory.h"

#include <cstdint>

namespace {

constexpr uint32_t kRankParamsRacedataGlobal = 0x809BD728;  // RaceConfig::spInstance
constexpr uint32_t kRankParamsPlayerCount = 0x24;           // u8, race scenario player count
constexpr uint32_t kRankParamsEntries = 12;

} // namespace

extern "C" void AIRankParams_Get_8073ac88(CpuContext* ctx) {
    const uint32_t racedata = Memory::Read32(kRankParamsRacedataGlobal);
    const uint32_t count = racedata == 0 ? 0 : Memory::Read8(racedata + kRankParamsPlayerCount);
    const uint32_t index = ctx->gpr[4];
    if (count > kRankParamsEntries && index < count) {
        ctx->gpr[4] = index * kRankParamsEntries / count;
    }
    MKW_GUEST_NEXT(8073AC88)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x8073AC88, AIRankParams_Get_8073ac88, "AIRankParams_Get_8073ac88");
