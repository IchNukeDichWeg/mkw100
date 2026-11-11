#pragma once

#include <dolphin/pad.h>

namespace AutoDriver {
// `--race-auto`: replace GameCube port 1's status for this frame with the scripted driver's input
// (runtime/src/hle/auto_driver.cpp). Does nothing in any other mode. Game thread only.
void FillPad(PADStatus& status);
} // namespace AutoDriver
