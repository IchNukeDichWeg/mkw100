// mkw100: room for the race scene's objects in races of more than 12 players.
//
// GameScene keeps its objects (karts, controls, pages, G3D scene groups) in structsMem1 (+0xC88),
// an ExpHeap of 7 MB that ends at the top of MEM1, and the rest in structsMem2 (+0xC8C). Each
// racer takes about 85 KB of structsMem1 (free at race start, M5 Pro: 3.4 MB with 24 racers,
// 1.4 MB with 48), and with 78 it ran out (0x488 bytes left) while ScnMgr::CreateScnGroupEx
// built its groups, which then called through a null group. MEM1 cannot grow (the player slot
// overflow and the shadow windows sit above it), while structsMem2 still had 12 MB free.
//
// With more than 12 players an allocation that fails in the current scene's structsMem1 is made
// in its structsMem2 instead. This is done at MEMAllocFromExpHeapEx, the call under both
// EGG::ExpHeap::alloc and the MEMAllocator that G3D allocates through. A block's free or resize
// can still name structsMem1 (MEMFreeToAllocator with the scene's allocator), so
// MEMFreeToExpHeap and MEMResizeForMBlockExpHeap are pointed at structsMem2 when the block lies
// outside the heap they were given and inside it. Up to 12 players nothing here changes the game's
// behaviour.

#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "memory.h"

#include <cstdint>

namespace {

constexpr uint32_t kRaceHeapRacedataGlobal = 0x809BD728;  // RaceConfig::spInstance
constexpr uint32_t kRaceHeapPlayerCount = 0x24;           // u8, race scenario player count
constexpr uint32_t kRaceHeapInlinePlayers = 12;
constexpr uint32_t kRkSystem = 0x802A4080;                // EGG::System object
constexpr uint32_t kSceneManager = 0x54;
constexpr uint32_t kCurrentScene = 0xC;
constexpr uint32_t kSceneId = 0x28;
constexpr uint32_t kStructsMem1 = 0xC88;   // GameScene, EGG::ExpHeap*
constexpr uint32_t kStructsMem2 = 0xC8C;
constexpr uint32_t kRvlHeap = 0x10;        // EGG::Heap: MEMiHeapHead*
constexpr uint32_t kHeapStart = 0x18;      // MEMiHeapHead
constexpr uint32_t kHeapEnd = 0x1C;

// The pair the last fallback used: frees and resizes of its blocks may still name structsMem1.
uint32_t g_mem1 = 0;
uint32_t g_mem2 = 0;

// GameScene::GetCurrent (0x8051BED0): the current scene unless its id is 0 (root) or 5.
uint32_t CurrentGameScene() {
    const uint32_t manager = Memory::Read32(kRkSystem + kSceneManager);
    const uint32_t scene = manager == 0 ? 0 : Memory::Read32(manager + kCurrentScene);
    if (scene == 0) {
        return 0;
    }
    const uint32_t id = Memory::Read32(scene + kSceneId);
    return id == 0 || id == 5 ? 0 : scene;
}

bool Inside(uint32_t heap, uint32_t address) {
    return address >= Memory::Read32(heap + kHeapStart) && address < Memory::Read32(heap + kHeapEnd);
}

// The heap that really holds `block`, for a call that named `heap`.
uint32_t OwnerHeap(uint32_t heap, uint32_t block) {
    if (g_mem1 != 0 && heap == g_mem1 && !Inside(heap, block) && Inside(g_mem2, block)) {
        return g_mem2;
    }
    return heap;
}

} // namespace

// MEMAllocFromExpHeapEx(heap, size, alignment)
extern "C" void MEM_AllocFromExpHeapEx_80198d88(CpuContext* ctx) {
    const uint32_t heap = ctx->gpr[3];
    const uint32_t size = ctx->gpr[4];
    const uint32_t alignment = ctx->gpr[5];
    MKW_GUEST_NEXT(80198D88)(ctx);
    if (ctx->gpr[3] != 0) {
        return;
    }
    const uint32_t racedata = Memory::Read32(kRaceHeapRacedataGlobal);
    if (racedata == 0 || Memory::Read8(racedata + kRaceHeapPlayerCount) <= kRaceHeapInlinePlayers) {
        return;
    }
    const uint32_t scene = CurrentGameScene();
    const uint32_t mem1 = scene == 0 ? 0 : Memory::Read32(scene + kStructsMem1);
    const uint32_t mem2 = scene == 0 ? 0 : Memory::Read32(scene + kStructsMem2);
    if (mem1 == 0 || mem2 == 0 || heap != Memory::Read32(mem1 + kRvlHeap)) {
        return;
    }
    g_mem1 = heap;
    g_mem2 = Memory::Read32(mem2 + kRvlHeap);
    ctx->gpr[3] = g_mem2;
    ctx->gpr[4] = size;
    ctx->gpr[5] = alignment;
    MKW_GUEST_NEXT(80198D88)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x80198D88, MEM_AllocFromExpHeapEx_80198d88, "MEM_AllocFromExpHeapEx_80198d88");

// MEMFreeToExpHeap(heap, block)
extern "C" void MEM_FreeToExpHeap_80199038(CpuContext* ctx) {
    ctx->gpr[3] = OwnerHeap(ctx->gpr[3], ctx->gpr[4]);
    MKW_GUEST_NEXT(80199038)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x80199038, MEM_FreeToExpHeap_80199038, "MEM_FreeToExpHeap_80199038");

// MEMResizeForMBlockExpHeap(heap, block, size)
extern "C" void MEM_ResizeForMBlockExpHeap_80198e38(CpuContext* ctx) {
    ctx->gpr[3] = OwnerHeap(ctx->gpr[3], ctx->gpr[4]);
    MKW_GUEST_NEXT(80198E38)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x80198E38, MEM_ResizeForMBlockExpHeap_80198e38, "MEM_ResizeForMBlockExpHeap_80198e38");
