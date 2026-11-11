// mkw100: the HUD's name balloons in races of more than 12 players.
//
// Each race HUD owns a RaceBalloons object (ctor 0x807F1C58, 0xC4 bytes): up to 3 name balloon
// controls, the players they show (+0x14), `Vec3 positions[12]` at +0x20 and more state after it
// (+0xB0, the item balloon at +0xC0). RaceBalloons::Update (0x807F132C) walks every race player
// (`i < RaceConfig race player count`), stores each one's position in positions[i] and picks the
// nearest ones in view for the balloons. Players 12+ write past the array into the next heap
// block (24 players: the first name balloon control, then a call through float bytes from
// ControlGroup::Draw).
//
// The array cannot grow in place (the fields after it are the class's own), so with more than 12
// players Update runs with the race player count it reads lowered to 12 and restored right after:
// name balloons can then only show players 0..11. Visual only; a native Update with positions for
// every player is the upgrade path. Update's callees (local player lookup, Raceinfo flags, kart
// position, camera projection) do not read the race player count. Up to 12 players nothing changes.

#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "memory.h"

#include <cstdint>

namespace {

constexpr uint32_t kBalloonsRacedataGlobal = 0x809BD728;  // RaceConfig::spInstance
constexpr uint32_t kBalloonsRaceCount = 0x24;             // u8, race scenario player count
constexpr uint32_t kBalloonPositions = 12;                // RaceBalloons positions[12]

} // namespace

extern "C" void RaceBalloons_Update_807f132c(CpuContext* ctx) {
    const uint32_t racedata = Memory::Read32(kBalloonsRacedataGlobal);
    const uint8_t count = racedata == 0 ? 0 : Memory::Read8(racedata + kBalloonsRaceCount);
    if (count <= kBalloonPositions) {
        MKW_GUEST_NEXT(807F132C)(ctx);
        return;
    }
    Memory::Write8(racedata + kBalloonsRaceCount, kBalloonPositions);
    MKW_GUEST_NEXT(807F132C)(ctx);
    Memory::Write8(racedata + kBalloonsRaceCount, count);
}
REGISTER_NATIVE_WRAPPER_AS(0x807F132C, RaceBalloons_Update_807f132c, "RaceBalloons_Update_807f132c");
