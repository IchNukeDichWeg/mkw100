#pragma once

// mkw100 overflow player slots (docs/mkw100/DESIGN.md, layout decision).
//
// RacedataScenario keeps its 12 inline RacedataPlayer slots (stride 0xF0) because 185 functions
// read the settings behind them at fixed displacements. Players 12..kMaxPlayers-1 live in a guest
// region of their own: player i gets a kWindow-sized window at kOverflowBase + (i - 12) * kWindow,
// laid out like the Racedata object itself, so one slot exists per scenario (race, menu, award)
// at the same offset it would have had in Racedata.
//
// The translator rewrites every `mulli rD, rA, 0xF0` that provably indexes a Racedata player slot
// (projects/mkwii/mkw100/player-slot-sites.txt, scripts/mkw100/slot_sites.py) into
// MKW100_PlayerSlotOffset(rA). The game then adds that to Racedata or a scenario pointer and
// dereferences a slot field: for i < 12 the result is i * 0xF0 exactly (the N=12 race is
// unchanged); for i >= 12 it is the delta that moves Racedata + i * 0xF0 + disp into player i's
// window at the same offset.

#include <cstdint>

namespace PlayerSlots {

inline constexpr uint32_t kInlineSlots = 12;
inline constexpr uint32_t kStride = 0xF0;
// Capacity, not the race size: the race uses as many slots as have a player type set.
// 128: the game keeps racer indices in signed bytes in places (Effects::Sub9d8::Update 0x8067DE14
// sign-extends them and indexes arrays with the result), so racer 128+ reads and writes before
// those arrays. Indices 0..127 stay in range; 134 and 141 racers crashed there, 128 and 130 did not.
inline constexpr uint32_t kMaxPlayers = 128;
// One window per overflow player; must cover Racedata's three scenarios (Racedata + 0x23F0; the
// last player field ends at 0x18F8). 16 KiB windows leave room for every overflow player.
inline constexpr uint32_t kWindow = 0x4000;
// Above the Kamek overlay (0x81800000, 2 MiB), below MEM2; nothing on a Wii maps it.
inline constexpr uint32_t kOverflowBase = 0x81A00000;
inline constexpr uint32_t kOverflowSize = 0x600000;
static_assert((kMaxPlayers - kInlineSlots) * kWindow <= kOverflowSize);
static_assert(kWindow >= 0x23F0);

inline constexpr uint32_t kRacedataGlobal = 0x809BD728;  // System::RaceConfig::spInstance
inline constexpr uint32_t kScenarioOffsets[3] = {0x20, 0xC10, 0x1800};  // race, menu, award
inline constexpr uint32_t kPlayersOffset = 0x8;  // inside a scenario

// Guest address of scenario-relative player `index` (any index) of the scenario at `scenario`.
uint32_t PlayerAddress(uint32_t racedata, uint32_t scenario, uint32_t index);

} // namespace PlayerSlots

// Called by translated code at each rewritten 0xF0 site; see above.
extern "C" uint32_t MKW100_PlayerSlotOffset(uint32_t index);

// Called at each `deref` site instead of computing slot + displacement: `slot` is a Racedata or
// scenario pointer plus a rewritten product for player i (inline or in i's window), and the access
// lands in player i + k's slot (an unrolled loop's later players). Returns the field's address in
// player i + k's slot; for i + k < 12 that is slot + displacement exactly.
extern "C" uint32_t MKW100_PlayerSlotAddress(uint32_t slot, uint32_t displacement);
