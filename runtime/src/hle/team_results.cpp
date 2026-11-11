// mkw100: the game's own Team VS results (red vs blue) in races of more than 12.
//
// The team standings page (TeamVSLeaderboardTotal, page 0x32) shows a CtrlRaceResultTeam column
// per team with six inline player slots and six rows (+0x174 players[6], +0x1F0 items[6]): six
// members a team at 12 racers. Its InitSelf (0x807F69E4) puts every member of its team into the
// next slot without a bound and adds their points to the team score (+0xB74); at 24 racers the
// seventh member onward overwrote the rows, and the page crashed calling one (Caller LR 0x805C28A8,
// found by the --race-teams --race-auto results harness).
//
// Past 12 racers each column lists its team's six best (series total, menu scenario +0xDA): while
// InitSelf runs, every other member's team is set to 2, which neither column matches, and put
// back after; the column's team score is then set to the whole team's total. Up to 12 racers
// nothing changes.

#include "abi_bridge.h"
#include "guest_abi/player_slots.h"
#include "guest_function_imports.h"
#include "memory.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kTeamRacedata = 0x809BD728;
constexpr uint32_t kTeamRaceCount = 0x24;            // u8, race scenario player count
constexpr uint32_t kTeamRaceTeam = 0x20 + 0x8 + 0xCC;   // race scenario player: team
constexpr uint32_t kTeamMenuTeam = 0xC10 + 0x8 + 0xCC;  // menu scenario player: team
constexpr uint32_t kTeamMenuTotal = 0xC10 + 0x8 + 0xDA; // menu scenario player: series total (u16)
constexpr uint32_t kColumnTeam = 0x1EC;
constexpr uint32_t kColumnScore = 0xB74;
constexpr uint32_t kColumnRows = 6;
constexpr uint32_t kNoColumn = 2;

} // namespace

extern "C" uint32_t MKW100_PlayerSlotOffset(uint32_t index);

// CtrlRaceResultTeam::InitSelf(this)
extern "C" void TeamResults_ColumnInit_807f69e4(CpuContext* ctx) {
    const uint32_t column = ctx->gpr[3];
    const uint32_t rd = Memory::Read32(kTeamRacedata);
    const uint32_t count = rd ? Memory::Read8(rd + kTeamRaceCount) : 0;
    if (count <= 12 || count > PlayerSlots::kMaxPlayers) {
        MKW_GUEST_NEXT(807F69E4)(ctx);
        return;
    }
    // Each team's members by series total, best first; the rest are hidden from this column.
    std::vector<std::pair<uint32_t, uint32_t>> members[2];  // (total, player)
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t slot = rd + MKW100_PlayerSlotOffset(i);
        const uint32_t team = Memory::Read32(slot + kTeamRaceTeam);
        if (team < 2) members[team].emplace_back(Memory::Read16(slot + kTeamMenuTotal), i);
    }
    struct Saved { uint32_t player, race, menu; };
    std::vector<Saved> hidden;
    uint32_t totals[2] = {0, 0};
    for (uint32_t team = 0; team < 2; ++team) {
        auto& list = members[team];
        std::stable_sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (uint32_t k = 0; k < list.size(); ++k) {
            totals[team] += list[k].first;
            if (k < kColumnRows) continue;
            const uint32_t slot = rd + MKW100_PlayerSlotOffset(list[k].second);
            hidden.push_back({slot, Memory::Read32(slot + kTeamRaceTeam), Memory::Read32(slot + kTeamMenuTeam)});
            Memory::Write32(slot + kTeamRaceTeam, kNoColumn);
            Memory::Write32(slot + kTeamMenuTeam, kNoColumn);
        }
    }
    try {
        MKW_GUEST_NEXT(807F69E4)(ctx);
    } catch (...) {
        for (const auto& s : hidden) { Memory::Write32(s.player + kTeamRaceTeam, s.race); Memory::Write32(s.player + kTeamMenuTeam, s.menu); }
        throw;
    }
    for (const auto& s : hidden) { Memory::Write32(s.player + kTeamRaceTeam, s.race); Memory::Write32(s.player + kTeamMenuTeam, s.menu); }
    const uint32_t team = Memory::Read32(column + kColumnTeam);
    if (team < 2) Memory::Write32(column + kColumnScore, totals[team]);
}
REGISTER_NATIVE_WRAPPER_AS(0x807F69E4, TeamResults_ColumnInit_807f69e4, "TeamResults_ColumnInit_807f69e4");
