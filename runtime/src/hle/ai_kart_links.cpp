// mkw100: kart links of the AI per-player objects 12+ (class ai_sub90).
//
// AI::Manager +0x90 holds a 12-slot table of small per-player objects (0x10 bytes: vtable, Kart
// object, player index, a difficulty value). 0x80739EE0 creates one per player (its stores into
// the table are rewritten for 12+), but the table's init (0x80739F4C, a virtual) walks a fixed 12:
// it links each object to KartObjectManager::object(index) and sets the difficulty value
// (25/50/75 for AI::Manager::GetDifficulty 0/1/2). Objects 12+ keep a null kart and the CPU
// driving code reads through it (24 players: func_8073A0E8 via KartObject +0 +4 +8).
//
// After the game's init this does the same for objects 12..players-1 (the shadow windows are not
// cleared between scenes, so only this race's players count). The N=12 race is unchanged.

#include "guest_abi/array_overflow.h"

#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "memory.h"

#include <cstdint>

namespace {

constexpr uint32_t kAiSub90Class = 4;
constexpr uint32_t kLinkTable = 0x4;                   // object*[12] in the table object
constexpr uint32_t kLinkKart = 0x4;
constexpr uint32_t kLinkPlayer = 0x8;
constexpr uint32_t kLinkDifficulty = 0xC;
constexpr uint32_t kKartObjectManager = 0x809C18F8;    // Kart::KartObjectManager::s_instance
constexpr uint32_t kKartObject = 0x80590100;           // KartObjectManager::object(u32)
constexpr uint32_t kLinksAiManager = 0x809C2BE8;       // AI::Manager*
constexpr uint32_t kAiDifficulty = 0x1C;               // AI::Manager::GetDifficulty reads this
constexpr uint32_t kDifficultyValues[3] = {25, 50, 75};
constexpr uint32_t kLinksRacedataGlobal = 0x809BD728;  // RaceConfig::spInstance
constexpr uint32_t kLinksRaceCount = 0x24;             // u8, race scenario player count

} // namespace

extern "C" void AiSub90_Init_80739f4c(CpuContext* ctx) {
    MKW_GUEST_NEXT(80739F4C)(ctx);
    const uint32_t players = Memory::Read8(Memory::Read32(kLinksRacedataGlobal) + kLinksRaceCount);
    const ArrayOverflow::ArrayClass& c = ArrayOverflow::Class(kAiSub90Class);
    const CpuContext saved = *ctx;
    for (uint32_t i = ArrayOverflow::kInlineSlots; i < players && i < ArrayOverflow::kMaxPlayers; ++i) {
        const uint32_t link = Memory::Read32(ArrayOverflow::Window(c, i) + kLinkTable);
        if (link == 0) {
            continue;
        }
        ctx->gpr[3] = Memory::Read32(kKartObjectManager);
        ctx->gpr[4] = Memory::Read32(link + kLinkPlayer);
        InvokeIndirectCpu(kKartObject, ctx);
        Memory::Write32(link + kLinkKart, ctx->gpr[3]);
        const uint32_t difficulty = Memory::Read32(Memory::Read32(kLinksAiManager) + kAiDifficulty);
        if (difficulty < 3) {
            Memory::Write32(link + kLinkDifficulty, kDifficultyValues[difficulty]);
        }
    }
    *ctx = saved;
}
REGISTER_NATIVE_WRAPPER_AS(0x80739F4C, AiSub90_Init_80739f4c, "AiSub90_Init_80739f4c");
