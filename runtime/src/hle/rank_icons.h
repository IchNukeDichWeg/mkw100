#pragma once

#include <cstdint>

struct CpuContext;

// mkw100 position icons for 13th..128th (runtime/src/hle/rank_icons.cpp).
namespace RankIcons {

// rank_icons/rankNNN.tpl for `place` is present and loaded (13..128).
bool Available(uint32_t place);

// The race HUD position counter `control` (CtrlRaceRankNum) shows `place`: for 13+ its three
// picture panes get the icon and its texture-pattern animation is paused; for 1..12 the stock
// textures and animation are restored. False when 13+ has no icon. Restores `ctx`.
bool Show(CpuContext* ctx, uint32_t control, uint32_t place);

} // namespace RankIcons
