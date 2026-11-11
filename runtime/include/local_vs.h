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
}
