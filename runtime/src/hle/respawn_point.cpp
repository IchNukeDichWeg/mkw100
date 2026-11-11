// mkw100: respawn positions for players past the 12th.
//
// KMP::Holder<JGPT>::GetPosition (0x8051896C: holder, Vec3* dest, player id) spreads the karts
// that respawn at one point: it adds the point's x and z axes scaled by two s8 factors from a
// static table of 12 pairs, [player id * 2] at 0x8088FA08 in StaticR. A player id past 11 reads
// past the table (100 karts, M5 Pro: racers 12, 52 and 16 others were put back over the course's
// edge, fell, and respawned in a loop until the race ended without them).
//
// Players past the 12th take the pair of player id % 12, so with 100 players up to 9 karts share
// one respawn spot and two that respawn at one point at once start overlapped. Up to 12
// players the game's function runs unchanged.

#include "abi_bridge.h"
#include "guest_function_imports.h"

#include <cstdint>

namespace {

constexpr uint32_t kRespawnPlayers = 12;  // pairs in the spread table

} // namespace

extern "C" void JGPT_GetPosition_8051896c(CpuContext* ctx) {
    const uint32_t player = ctx->gpr[5] & 0xFF;
    if (player != 0xFF && player >= kRespawnPlayers) {
        ctx->gpr[5] = player % kRespawnPlayers;
    }
    MKW_GUEST_NEXT(8051896C)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x8051896C, JGPT_GetPosition_8051896c, "JGPT_GetPosition_8051896c");
