// mkw100: item limits for races of more than 12 players.
//
// Each item object kind has a limit, the most of it that may be in play at once
// (Item::ObjProperties[15] at 0x809C2F48, stride 0x74, u32 limit at +0x4). ComputeCapacities
// (0x80790FB8) turns the limits into pool capacities (+0xC, +0x10), and Item::Manager's
// ObjHolders allocate their pools from them when the manager is constructed (0x807992D8). An item
// pulled with no room left in its pool is replaced by a mushroom (Item::Manager::
// IsThereCapacityForItem 0x80799BE8). The stock limits are sized for 12 players, so with 100
// most pulls of stars, megas and shells came out as mushrooms.
//
// For more than 12 players each limit is scaled by players / 12, rounded up, before the
// capacities are computed from it. Items that hit the whole field (blue shell,
// lightning, blooper, POW) keep their stock limit, and the cooldown that blocks pulling them
// again after one was pulled (300 frames, Item::ItemSlotData::CanItemNotBeObtained 0x807BB380)
// is scaled by the same factor: at 100 players 2,500 frames, about 42 seconds. A gameplay choice:
// the game has no data for 13+ players. Up to 12 players everything runs unchanged.

#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "memory.h"
#include "runtime_log.h"

#include <cstdint>

namespace {

// Toggle for the whole change; off runs the game's own limits and cooldown at every player count.
constexpr bool kScaleItemLimits = true;
constexpr uint32_t kItemStockPlayers = 12;
constexpr uint32_t kItemRacedata = 0x809BD728;      // Racedata*, u8 player count at +0x24
constexpr uint32_t kItemObjProperties = 0x809C2F48; // Item::ObjProperties[15]
constexpr uint32_t kItemObjStride = 0x74;
constexpr uint32_t kItemObjLimit = 0x4;
constexpr uint32_t kItemObjKinds = 15;
constexpr uint32_t kItemCooldownFrames = 300;       // the game's own constant in 0x807BB380

// ItemObjId: 5 blue shell, 6 lightning, 10 blooper, 11 POW. Their cooldown timers are
// ItemSlotData +0x34 +0x38 +0x3C +0x40, the order 0x807BB380 reads them in.
constexpr bool IsFieldItem(uint32_t id) { return id == 5 || id == 6 || id == 10 || id == 11; }

uint32_t ItemPlayerCount() {
    const uint32_t racedata = Memory::Read32(kItemRacedata);
    return racedata == 0 ? 0 : Memory::Read8(racedata + 0x24);
}

} // namespace

// Item::ObjProperties::ComputeCapacities. The Item::Manager constructor (0x807992D8) copies every
// race's properties fresh from StaticR (0x80790AFC) and then calls this, so the limits read here
// are always the stock ones.
extern "C" void ItemObjProperties_ComputeCapacities_80790fb8(CpuContext* ctx) {
    const uint32_t players = ItemPlayerCount();
    if (kScaleItemLimits && players > kItemStockPlayers) {
        for (uint32_t i = 0; i < kItemObjKinds; ++i) {
            const uint32_t address = kItemObjProperties + i * kItemObjStride + kItemObjLimit;
            const uint32_t stock = Memory::Read32(address);
            if (!IsFieldItem(i)) {
                Memory::Write32(address, (stock * players + kItemStockPlayers - 1) / kItemStockPlayers);
            }
            RT_LOGF("item-limits", "%u players: item object %u limit %u -> %u\n", players, i, stock, Memory::Read32(address));
        }
    }
    MKW_GUEST_NEXT(80790FB8)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x80790FB8, ItemObjProperties_ComputeCapacities_80790fb8, "ItemObjProperties_ComputeCapacities_80790fb8");

// bool ItemSlotData::CanItemNotBeObtained(this, ItemObjId id, bool useCooldown): true while the
// item's timer is below the cooldown. Only the cooldown length changes past 12 players.
extern "C" void ItemSlot_CanItemNotBeObtained_807bb380(CpuContext* ctx) {
    const uint32_t players = ItemPlayerCount();
    const uint32_t id = ctx->gpr[4];
    if (!kScaleItemLimits || players <= kItemStockPlayers || (ctx->gpr[5] & 0xFF) == 0 || !IsFieldItem(id)) {
        MKW_GUEST_NEXT(807BB380)(ctx);
        return;
    }
    const uint32_t timer = id == 6 ? 0x34 : id == 5 ? 0x38 : id == 10 ? 0x3C : 0x40;
    const int32_t frames = static_cast<int32_t>(Memory::Read32(ctx->gpr[3] + timer));
    const int32_t cooldown = static_cast<int32_t>(kItemCooldownFrames * players / kItemStockPlayers);
    ctx->gpr[3] = frames < cooldown ? 1 : 0;
}
REGISTER_NATIVE_WRAPPER_AS(0x807BB380, ItemSlot_CanItemNotBeObtained_807bb380, "ItemSlot_CanItemNotBeObtained_807bb380");
