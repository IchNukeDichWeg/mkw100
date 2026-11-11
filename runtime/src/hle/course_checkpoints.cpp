// mkw100: checkpoint search for players 16+.
//
// CourseMap's checkpoint search (0x80511500, from RaceManagerPlayer::calcCheckPoint and
// 0x80534260) marks the checkpoints it has visited in a u16 at checkpoint +0x18 with bit
// `1 << playerIndex`, and its recursive helper (0x80511110) stops at a checkpoint whose bit is set.
// For player 16+ the bit falls off the halfword, nothing is ever marked, and the recursion runs
// until the host stack overflows (seen at 24 players). The player index is used for nothing but
// that bit, and the search clears every checkpoint's mask on entry, so within one search any bit
// is as good as another: fold the index into 0..15. Players 0..15 keep their own bit (the N=12
// race is unchanged).

#include "abi_bridge.h"
#include "guest_function_imports.h"

#include <cstdint>

namespace {
constexpr uint32_t kVisitedMaskBits = 16;
} // namespace

extern "C" void CourseMap_FindSector_80511500(CpuContext* ctx) {
    const uint32_t player = ctx->gpr[4];
    ctx->gpr[4] = player % kVisitedMaskBits;
    MKW_GUEST_NEXT(80511500)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x80511500, CourseMap_FindSector_80511500, "CourseMap_FindSector_80511500");
