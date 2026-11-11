// mkw100: the POW block in races of more than 12 players.
//
// Item::PowMgr (0x28 bytes, Item::PowMgr::CreateInstance 0x807B1BC0, global 0x809C3660) keeps
// `u8 hit[12]` at +9 (which players the POW affects), a state byte at +0x15 and the local
// screens' Item::PowScreen*[4] at +0x18. DeployPow (0x807B1DB4) sets hit[i] for every item player
// (count: the byte at 0x809C38B8, read once on entry), CreateShockwave (0x807B20BC) and
// AnimateModel (0x807B1EEC) read them in loops over the same count. With 24 players hit[15..] land
// on the screen pointers (a call through them from AnimateModel).
//
// Expanded solo VS stores the additional eligibility flags on the host and passes
// a widened temporary view to CreateShockwave. Its damage/dodge logic stays stock.
// Local-screen animation still uses the original twelve-slot view (local P1 is
// slot zero). Other modes retain the earlier safety clamp. <=12 is unchanged.

#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "memory.h"

#include <cstdint>
#include <array>
#include "guest_abi/player_slots.h"
#include "runtime_log.h"

namespace {

constexpr uint32_t kItemPlayerCount = 0x809C38B8;  // u8, Item::Manager's player count source
constexpr uint8_t kPowHitSlots = 12;
std::array<uint8_t, PlayerSlots::kMaxPlayers> expandedHits{};
bool ExpandedSoloVs() {
    const uint32_t rd=Memory::Read32(MKW_GADDR(809BD728));
    return rd && Memory::Read8(rd+0x24)>12 && Memory::Read8(rd+0x24)<=expandedHits.size() &&
        Memory::Read32(rd+0xB70)==1 && Memory::Read32(rd+0xB74)==0 && !(Memory::Read32(rd+0xB90)&2);
}

void WithClampedCount(CpuContext* ctx, void (*next)(CpuContext*)) {
    const uint8_t count = Memory::Read8(kItemPlayerCount);
    if (count <= kPowHitSlots) {
        next(ctx);
        return;
    }
    Memory::Write8(kItemPlayerCount, kPowHitSlots);
    next(ctx);
    Memory::Write8(kItemPlayerCount, count);
}

} // namespace

extern "C" void PowMgr_DeployPow_807b1db4(CpuContext* ctx) {
    const uint32_t self=ctx->gpr[3], owner=ctx->gpr[4];
    const bool expand=ExpandedSoloVs() && static_cast<int32_t>(Memory::Read32(self))<=0;
    WithClampedCount(ctx, MKW_GUEST_NEXT(807B1DB4));
    if (expand) {
        const uint32_t count=Memory::Read8(kItemPlayerCount);
        const uint32_t info=Memory::Read32(MKW_GADDR(809BD730));
        const uint32_t players=Memory::Read32(info+0xC);
        const uint32_t rank=Memory::Read8(Memory::Read32(players+4*owner)+0x20);
        for (uint32_t i=0;i<count;++i) {
            expandedHits[i]= i<12 ? Memory::Read8(self+9+i) :
                (i!=owner && Memory::Read8(Memory::Read32(players+4*i)+0x20)<=rank);
        }
        uint32_t targets=0, extraTargets=0;
        for (uint32_t i=0;i<count;++i) { targets+=expandedHits[i]!=0; extraTargets+=(i>=12 && expandedHits[i]!=0); }
        RT_LOGF("local-vs", "POW deployment: owner=%u racers=%u targets=%u extraTargets=%u\n",owner,count,targets,extraTargets);
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x807B1DB4, PowMgr_DeployPow_807b1db4, "PowMgr_DeployPow_807b1db4");

extern "C" void PowMgr_AnimateModel_807b1eec(CpuContext* ctx) {
    WithClampedCount(ctx, MKW_GUEST_NEXT(807B1EEC));
}
REGISTER_NATIVE_WRAPPER_AS(0x807B1EEC, PowMgr_AnimateModel_807b1eec, "PowMgr_AnimateModel_807b1eec");

extern "C" void PowMgr_CreateShockwave_807b20bc(CpuContext* ctx) {
    if (ExpandedSoloVs()) {
        // This function reads only owner (+8) and hit flags (+9+i) from PowMgr.
        // A temporary widened view avoids overwriting its live state/screens.
        // Keep the caller linkage area separate from that view.
        const uint32_t self=ctx->gpr[3], sp=ctx->gpr[1];
        constexpr uint32_t reserve=((32+9+PlayerSlots::kMaxPlayers+15)/16)*16;
        const uint32_t view=sp-reserve+32;
        Memory::Write8(view+8,Memory::Read8(self+8));
        for (uint32_t i=0;i<Memory::Read8(kItemPlayerCount);++i) Memory::Write8(view+9+i,expandedHits[i]);
        ctx->gpr[1]=sp-reserve; ctx->gpr[3]=view;
        try { MKW_GUEST_NEXT(807B20BC)(ctx); }
        catch (...) { ctx->gpr[1]=sp; throw; }
        ctx->gpr[1]=sp;
        return;
    }
    WithClampedCount(ctx, MKW_GUEST_NEXT(807B20BC));
}
REGISTER_NATIVE_WRAPPER_AS(0x807B20BC, PowMgr_CreateShockwave_807b20bc, "PowMgr_CreateShockwave_807b20bc");
