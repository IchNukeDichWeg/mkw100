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

std::atomic<uint32_t> g_count{2};
std::atomic<uint32_t> g_teamSize{0};
std::atomic<bool> g_byAverage{true};
std::atomic<uint32_t> g_inPlay{0};
std::atomic<bool> g_teamVsRace{false};  // OnInitRace: this race is a Team VS race past 12
std::mutex g_lock;
std::vector<uint8_t> g_teamOf;                  // per racer
std::array<uint8_t, Teams::kMaxTeams> g_colour{};  // team -> colour index
std::vector<Teams::Standing> g_standings;

} // namespace

namespace local_vs_detail {
uint32_t RaceCount();  // local_vs.cpp: the race scenario's player count
}

namespace {

uint32_t Clamp(uint32_t teams) { return std::clamp(teams, 2u, Teams::kMaxTeams); }

constexpr uint32_t kTeamsRacedata = 0x809BD728;     // RaceConfig::spInstance
constexpr uint32_t kTeamsRaceModeFlags = 0x20 + 0xB70;  // race scenario settings modeFlags
constexpr uint32_t kTeamsRaceNumber = 0x20 + 0xB6C;     // race scenario: race of the series (u8)
constexpr uint32_t kTeamsFlag = 2;

} // namespace

namespace Teams {

void Initialize(uint32_t teamsFromConfig) {
    const auto& c = RuntimeConfigFile::Get();
    g_count = Clamp(teamsFromConfig);
    g_teamSize = std::min<uint32_t>(c.localVsTeamSize.value_or(0), PlayerSlots::kMaxPlayers);
    g_byAverage = c.localVsTeamScoring.value_or("average") != "total";
}

uint32_t TeamSize() { return g_teamSize.load(); }
void SetTeamSize(uint32_t racers) {
    g_teamSize = std::min<uint32_t>(racers, PlayerSlots::kMaxPlayers);
    RuntimeConfigFile::WriteSetting("local_vs", "team_size", std::to_string(g_teamSize.load()));
}
bool ScoreByAverage() { return g_byAverage.load(); }
void SetScoreByAverage(bool average) {
    g_byAverage = average;
    RuntimeConfigFile::WriteSetting("local_vs", "team_scoring", RuntimeConfigFile::FormatString(average ? "average" : "total"));
}
uint32_t InPlay() { return g_inPlay.load(); }
uint32_t Standing::Score() const { return g_byAverage.load() ? uint32_t(average + 0.5) : total; }

void OnInitRace(uint32_t racedata) {
    g_teamVsRace = false;
    if (racedata == 0) return;
    const uint32_t flags = Memory::Read32(racedata + kTeamsRaceModeFlags);
    if ((flags & kTeamsFlag) == 0 || local_vs_detail::RaceCount() <= 12) return;
    // Plain VS from here (karts, HUD, item hits); team identity is ours. The menu scenario keeps
    // the flag, so every race of the series comes back through here.
    Memory::Write32(racedata + kTeamsRaceModeFlags, flags & ~kTeamsFlag);
    g_teamVsRace = true;
    bool fresh = Memory::Read8(racedata + kTeamsRaceNumber) == 0;
    {
        std::lock_guard guard(g_lock);
        fresh = fresh || g_teamOf.size() != local_vs_detail::RaceCount();
    }
    if (fresh) {
        NewSeries(local_vs_detail::RaceCount(), BootRace::Scripted());
        Log("series", {});
    }
}

uint32_t Count() { return g_count.load(); }

void SetCount(uint32_t teams) {
    g_count = Clamp(teams);
    RuntimeConfigFile::WriteSetting("local_vs", "teams", std::to_string(g_count.load()));
}

void NewSeries(uint32_t racers, bool repeatable) {
    const uint32_t size = TeamSize();
    const uint32_t teams = size ? Clamp((racers + size - 1) / size) : Count();
    g_inPlay = racers > teams ? teams : 0;
    std::lock_guard guard(g_lock);
    g_standings.clear();
    g_teamOf.assign(racers, uint8_t(kNoTeam));
    if (racers <= teams) return;
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
    if (!g_teamVsRace.load() || local_vs_detail::RaceCount() <= 12) return false;
    std::lock_guard guard(g_lock);
    return g_inPlay.load() >= 2 && g_teamOf.size() > g_inPlay.load();
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
    const bool byAverage = g_byAverage.load();
    std::stable_sort(result.begin(), result.end(), [byAverage](const Standing& a, const Standing& b) {
        return byAverage ? a.average > b.average : a.total > b.total;
    });
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
    if (!g_teamVsRace.load()) return;
    if (points.empty()) {
        std::array<uint32_t, kMaxTeams> sizes{};
        uint32_t racers = 0;
        {
            std::lock_guard guard(g_lock);
            racers = uint32_t(g_teamOf.size());
            for (uint8_t t : g_teamOf) if (t < kMaxTeams) ++sizes[t];
        }
        std::string line;
        for (uint32_t t = 0; t < InPlay(); ++t) line += std::string(" ") + ColourOf(t).name + "=" + std::to_string(sizes[t]);
        RT_LOGF("teams", "%s %u racers in %u teams:%s (racer 0 is %s)\n", what, racers, InPlay(), line.c_str(),
                ColourOf(TeamOf(0)).name);
        return;
    }
    for (const auto& s : Rank(points)) {
        RT_LOGF("teams", "%s %s: %u members, %u points, average %.2f\n", what, ColourOf(s.team).name, s.members, s.total,
                s.average);
    }
}

} // namespace Teams
