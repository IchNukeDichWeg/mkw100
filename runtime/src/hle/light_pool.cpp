// mkw100: scene lights for races of more than 12 players.
//
// The race's LightMgrHolder (Pulsar 3D/Scn/Light.hpp) builds its pool of Light objects once,
// LoadBLIGHT (0x80561988): one per light object of the scene's EGG::LightMgr past the first 16,
// each with light set id index + 8 as a signed byte, so the pool cannot pass 120. Every kart takes
// two (Kart::Player::Init, 0x8058E22C, through ScnMgr::AcquireLight 0x80565630), and items take
// more: 48 karts fit, 64 got a null Light and called through it.
//
// With more than 12 players an AcquireLight that finds the pool empty hands out a light already in
// use, round robin over the acquired list, so two owners share it: whichever updates it last that
// frame sets its position and colour, a visual limit only. Shared handouts are counted per light,
// and a ReleaseLight (0x80561404, item code only) of a shared light drops one share instead of
// putting a light that is still in use back on the free list. Up to 12 players nothing here
// changes the game's behaviour.

#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "memory.h"

#include <cstdint>
#include <unordered_map>

namespace {

constexpr uint32_t kLightRacedataGlobal = 0x809BD728;  // RaceConfig::spInstance
constexpr uint32_t kLightPlayerCount = 0x24;           // u8, race scenario player count
constexpr uint32_t kLightInlinePlayers = 12;
// LightMgrHolder: nw4r::ut::List acquiredLightsList at 0x0 (head, tail, u16 count, u16 link offset)
constexpr uint32_t kAcquiredHead = 0x0;
constexpr uint32_t kAcquiredCount = 0x8;
constexpr uint32_t kAcquiredLinkOffset = 0xA;
constexpr uint32_t kLinkNext = 0x4;  // nw4r::ut::Link {prev, next}

// Extra owners of each shared light, for the current holder; one holder per scene.
std::unordered_map<uint32_t, uint32_t> g_sharedLights;
uint32_t g_nextShared = 0;

bool ManyPlayers() {
    const uint32_t racedata = Memory::Read32(kLightRacedataGlobal);
    return racedata != 0 && Memory::Read8(racedata + kLightPlayerCount) > kLightInlinePlayers;
}

} // namespace

// LightMgrHolder::__ct (race and menu): a new pool, so no light is shared yet.
extern "C" void LightMgrHolder_Ctor_80561688(CpuContext* ctx) {
    g_sharedLights.clear();
    g_nextShared = 0;
    MKW_GUEST_NEXT(80561688)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x80561688, LightMgrHolder_Ctor_80561688, "LightMgrHolder_Ctor_80561688");

// LightMgrHolder::AcquireLight(this): the first free Light, or null.
extern "C" void LightMgrHolder_AcquireLight_80561398(CpuContext* ctx) {
    const uint32_t holder = ctx->gpr[3];
    MKW_GUEST_NEXT(80561398)(ctx);
    if (ctx->gpr[3] != 0 || !ManyPlayers()) {
        return;
    }
    const uint32_t count = Memory::Read16(holder + kAcquiredCount);
    if (count == 0) {
        return;
    }
    const uint32_t offset = Memory::Read16(holder + kAcquiredLinkOffset);
    uint32_t light = Memory::Read32(holder + kAcquiredHead);
    for (uint32_t step = g_nextShared++ % count; step > 0 && light != 0; --step) {
        light = Memory::Read32(light + offset + kLinkNext);
    }
    if (light != 0) {
        ++g_sharedLights[light];
    }
    ctx->gpr[3] = light;
}
REGISTER_NATIVE_WRAPPER_AS(0x80561398, LightMgrHolder_AcquireLight_80561398, "LightMgrHolder_AcquireLight_80561398");

// LightMgrHolder::ReleaseLight(this, light)
extern "C" void LightMgrHolder_ReleaseLight_80561404(CpuContext* ctx) {
    const auto shared = g_sharedLights.find(ctx->gpr[4]);
    if (shared != g_sharedLights.end()) {
        if (--shared->second == 0) {
            g_sharedLights.erase(shared);
        }
        return;
    }
    MKW_GUEST_NEXT(80561404)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x80561404, LightMgrHolder_ReleaseLight_80561404, "LightMgrHolder_ReleaseLight_80561404");
