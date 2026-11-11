// mkw100 teams for offline VS of more than 12 racers. See teams.h.
//
// Modelled on Retro Rewind's extended teams (rr-pulsar PulsarEngine/UI/ExtendedTeamSelect, GPL-3,
// with Pulsar and MKW-SP under MIT): racers shuffled into teams at the start of a series, a colour
// per team chosen from a fixed table and randomised per series, the game's red/blue team mode left
// off so the race plays as normal VS, team identity shown by tinting UI that already exists. RR is
// limited to 12 players and 6 colours; this keeps one team per racer index up to
// PlayerSlots::kMaxPlayers, 8 colours, and scores a team by its members' average, not their sum,
// so a team one racer short is not behind.

#include "teams.h"

#include "boot_race.h"
#include "local_vs.h"
#include "guest_abi/player_slots.h"
#include "runtime_config.h"
#include "memory.h"
#include "runtime_log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <numeric>
#include <random>
#include <string>

namespace {

// RR's six (red, orange, yellow, green, blue, purple), then pink and cyan.
constexpr std::array<Teams::Colour, Teams::kMaxTeams> kColours{{
    {255, 40, 40, "Red"},
    {255, 150, 0, "Orange"},
    {255, 225, 0, "Yellow"},
    {40, 200, 40, "Green"},
    {30, 90, 255, "Blue"},
    {160, 60, 220, "Purple"},
    {255, 110, 180, "Pink"},
    {0, 210, 220, "Cyan"},
}};
constexpr Teams::Colour kNoColour{255, 255, 255, "None"};

std::atomic<uint32_t> g_count{0};
std::mutex g_lock;
std::vector<uint8_t> g_teamOf;                  // per racer
std::array<uint8_t, Teams::kMaxTeams> g_colour{};  // team -> colour index
std::vector<Teams::Standing> g_standings;

} // namespace

namespace local_vs_detail {
uint32_t RaceCount();  // local_vs.cpp: the race scenario's player count
}

namespace {

uint32_t Clamp(uint32_t teams) { return teams < 2 ? 0 : std::min(teams, Teams::kMaxTeams); }

} // namespace

namespace Teams {

void Initialize(uint32_t teamsFromConfig) { g_count = Clamp(teamsFromConfig); }

uint32_t Count() { return g_count.load(); }

void SetCount(uint32_t teams) {
    g_count = Clamp(teams);
    RuntimeConfigFile::WriteSetting("local_vs", "teams", std::to_string(g_count.load()));
}

void NewSeries(uint32_t racers, bool repeatable) {
    const uint32_t teams = Count();
    std::lock_guard guard(g_lock);
    g_standings.clear();
    g_teamOf.assign(racers, uint8_t(kNoTeam));
    if (teams < 2 || racers <= teams) return;
    std::mt19937 random(repeatable ? 0x74A1B095u
                                   : uint32_t(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::vector<uint32_t> order(racers);
    std::iota(order.begin(), order.end(), 0u);
    std::shuffle(order.begin(), order.end(), random);
    for (uint32_t i = 0; i < racers; ++i) g_teamOf[order[i]] = uint8_t(i % teams);  // sizes differ by <= 1
    std::iota(g_colour.begin(), g_colour.end(), uint8_t(0));
    if (!repeatable) std::shuffle(g_colour.begin(), g_colour.begin() + teams, random);
}

bool Active() {
    std::lock_guard guard(g_lock);
    // Teams belong to Team VS only (race scenario mode flags, Racedata + 0x20 + 0xB48 + 0x28, bit 2):
    // averaged teams in solo VS read as a bug. Team VS still runs the game's red/blue
    // (team_results.cpp) until its own 2-8 team leaderboard lands, so nothing turns this on yet.
    return false;
}

uint32_t TeamOf(uint32_t player) {
    const uint32_t racers = local_vs_detail::RaceCount();
    bool assign = false;
    {
        std::lock_guard guard(g_lock);
        assign = g_teamOf.size() != racers;
    }
    if (assign) {
        NewSeries(racers, BootRace::Scripted());
        std::vector<std::pair<uint32_t, uint32_t>> none;
        Log("assigned", none);
    }
    std::lock_guard guard(g_lock);
    return player < g_teamOf.size() ? g_teamOf[player] : kNoTeam;
}

const Colour& ColourOf(uint32_t team) {
    if (team >= kMaxTeams) return kNoColour;
    std::lock_guard guard(g_lock);
    return kColours[g_colour[team]];
}

uint32_t ColourIndexOf(uint32_t team) {
    if (team >= kMaxTeams) return kNoTeam;
    std::lock_guard guard(g_lock);
    return g_colour[team];
}

std::vector<Standing> Rank(const std::vector<std::pair<uint32_t, uint32_t>>& points) {
    std::array<Standing, kMaxTeams> sums{};
    for (uint32_t t = 0; t < kMaxTeams; ++t) sums[t].team = t;
    for (const auto& [player, earned] : points) {
        const uint32_t team = TeamOf(player);
        if (team >= kMaxTeams) continue;
        sums[team].members += 1;
        sums[team].total += earned;
    }
    std::vector<Standing> result;
    for (auto& s : sums) {
        if (s.members == 0) continue;
        s.average = double(s.total) / double(s.members);
        result.push_back(s);
    }
    std::stable_sort(result.begin(), result.end(), [](const Standing& a, const Standing& b) { return a.average > b.average; });
    return result;
}

void SetStandings(std::vector<Standing> standings) {
    std::lock_guard guard(g_lock);
    g_standings = std::move(standings);
}

std::vector<Standing> LastStandings() {
    std::lock_guard guard(g_lock);
    return g_standings;
}

void Log(const char* what, const std::vector<std::pair<uint32_t, uint32_t>>& points) {
    if (Count() < 2) return;
    if (points.empty()) {
        std::array<uint32_t, kMaxTeams> sizes{};
        uint32_t racers = 0;
        {
            std::lock_guard guard(g_lock);
            racers = uint32_t(g_teamOf.size());
            for (uint8_t t : g_teamOf) if (t < kMaxTeams) ++sizes[t];
        }
        std::string line;
        for (uint32_t t = 0; t < Count(); ++t) line += std::string(" ") + ColourOf(t).name + "=" + std::to_string(sizes[t]);
        RT_LOGF("teams", "%s %u racers in %u teams:%s (racer 0 is %s)\n", what, racers, Count(), line.c_str(),
                ColourOf(TeamOf(0)).name);
        return;
    }
    for (const auto& s : Rank(points)) {
        RT_LOGF("teams", "%s %s: %u members, %u points, average %.2f\n", what, ColourOf(s.team).name, s.members, s.total,
                s.average);
    }
}

} // namespace Teams
