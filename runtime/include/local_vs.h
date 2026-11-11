#pragma once
#include <cstdint>
struct CpuContext;
namespace LocalVs {
void Initialize();
void DrawSettings();
void DrawHud();
void TestItems(CpuContext* ctx, uint32_t raceinfo);
void PrepareRace(uint32_t racedata);
bool UnlocksEnabled();
// Racers for the next single-player VS series, 1..PlayerSlots::kMaxPlayers (F10 and the VS rules page).
int DesiredCount();
void SetDesiredCount(int count);
// Expanded (13+) offline VS balancing, F10 and Config.toml [local_vs] scoring / item_distribution.
enum class Scoring : uint32_t { ClassicSpread, ClassicTop12, Linear };
enum class ItemDistribution : uint32_t { Even, Generous, Tight, ClassicTop11 };
ItemDistribution CurrentItemDistribution();
// Points for finishing `place` (1-based) of `count` racers under the current scoring.
uint32_t PointsFor(uint32_t place, uint32_t count);
// The 0-based ItemSlot.bin row (of `rows`) that race position `position` (0-based) of `count` draws from.
uint32_t ItemRowFor(uint32_t position, uint32_t count, uint32_t rows);
}
