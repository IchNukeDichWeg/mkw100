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
}
