#pragma once

#include <cstdint>
#include <utility>
#include <vector>

// mkw100 teams for offline VS of more than 12 racers (runtime/src/hle/teams.cpp): 2..8 teams of
// near-equal size (differing by at most one), kept for a series, scored by the average points per
// member so uneven sizes stay fair. Team colours tint existing UI (name balloons, minimap, result
// rows) as Retro Rewind's extended teams do; the game's own red/blue team mode stays off.
namespace Teams {

inline constexpr uint32_t kMaxTeams = 8;
inline constexpr uint32_t kNoTeam = 0xFF;

struct Colour {
    uint8_t r, g, b;
    const char* name;
};

void Initialize(uint32_t teamsFromConfig);
// Configured team count: 0 = off, else 2..kMaxTeams.
uint32_t Count();
void SetCount(uint32_t teams);  // F10 / VS rules row; writes Config.toml

// A new series of `racers`: shuffle racers into Count() teams and pick the colour order.
// `repeatable` uses a fixed seed (test modes).
void NewSeries(uint32_t racers, bool repeatable);
// Teams in play for this race (Count() >= 2 and more racers than teams).
bool Active();
// The team of racer `player`, kNoTeam when teams are off. Assigns lazily (headless races start
// without the menus' series setup).
uint32_t TeamOf(uint32_t player);
const Colour& ColourOf(uint32_t team);

struct Standing {
    uint32_t team;
    uint32_t members;
    uint32_t total;      // members' points added up
    double average;      // total / members: the team's score
};
// Team standings from (racer, points) pairs, best average first.
std::vector<Standing> Rank(const std::vector<std::pair<uint32_t, uint32_t>>& points);
// Last computed series standings (for the F10 table and the results page).
void SetStandings(std::vector<Standing> standings);
std::vector<Standing> LastStandings();
// "[teams] ..." lines: assignment and the standings of `points`.
void Log(const char* what, const std::vector<std::pair<uint32_t, uint32_t>>& points);

} // namespace Teams
