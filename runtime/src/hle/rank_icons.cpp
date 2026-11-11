// mkw100: position icons for 13th..128th on the race HUD's position counter (CtrlRaceRankNum).
//
// The game's counter only has pictures for 1st..12th. rank_icons/rank013.tpl .. rank128.tpl next
// to the executable (runtime/assets/rank_icons: 13-24 by hammerbro101, 25-128 generated; see the
// README there) are loaded on first use into a fixed guest span, one slot per place, and the
// counter's picture pane gets the place's texture through nw4r::lyt::TexMap::ReplaceImage, the
// way Retro Rewind swaps character icons. The pane's own texture is saved first and put back for
// places 1..12. When the files are missing the HUD overlay's vector digits are used instead.

#include "rank_icons.h"

#include "abi_bridge.h"
#include "guest_abi/array_overflow.h"
#include "guest_abi/player_slots.h"
#include "memory.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <array>
#include <cstdio>
#include <fstream>
#include <vector>

namespace {

constexpr uint32_t kFirstPlace = 13;
constexpr uint32_t kLastPlace = PlayerSlots::kMaxPlayers;
constexpr uint32_t kSlotBytes = 0x4100;  // a 64x64 RGBA8 TPL is 0x4040 bytes
constexpr uint32_t kGetMaterial = 0x68;  // nw4r::lyt::Pane vtable
constexpr uint32_t kGetTexMapAry = 0x80080990;
constexpr uint32_t kReplaceImage = 0x80082550;
constexpr uint32_t kTexMapBytes = 0x1C;
static_assert((kLastPlace - kFirstPlace + 1) * kSlotBytes <= ArrayOverflow::kRankIconsSize);

std::array<int8_t, kLastPlace + 1> g_state{};  // 0 unknown, 1 loaded, -1 missing
bool g_anyMissingLogged = false;

uint32_t Slot(uint32_t place) { return ArrayOverflow::kRankIcons + (place - kFirstPlace) * kSlotBytes; }

bool Load(uint32_t place) {
    if (g_state[place] != 0) return g_state[place] > 0;
    g_state[place] = -1;
    const auto dir = RuntimeConfigFile::ExecutableDirectory();
    if (!dir) return false;
    char name[32];
    std::snprintf(name, sizeof name, "rank%03u.tpl", place);
    std::ifstream file(*dir / "rank_icons" / name, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (bytes.size() < 0x40 || bytes.size() > kSlotBytes) {
        if (!g_anyMissingLogged) {
            g_anyMissingLogged = true;
            RT_LOGF("rank-icons", "rank_icons/%s missing or invalid (%zu bytes); using the overlay counter\n", name,
                    bytes.size());
        }
        return false;
    }
    const uint32_t slot = Slot(place);
    for (uint32_t i = 0; i < bytes.size(); ++i) Memory::Write8(slot + i, uint8_t(bytes[i]));
    g_state[place] = 1;
    return true;
}

} // namespace

namespace RankIcons {

bool Available(uint32_t place) {
    return place >= kFirstPlace && place <= kLastPlace && Load(place);
}

namespace {
// CtrlRaceRankNum's layout (Race_E.szs game_image_position.brlyt): three pictures switched together
// by the texture pattern animation (group 0, eRank): shadow, picture, and the highlight shown while
// the counter pops (group 1, eRankTransition, left running).
constexpr uint32_t kLayout = 0xA8;
constexpr uint32_t kAnimator = 0x98;           // UIAnimator: AnimationGroup* (0x44 each)
constexpr uint32_t kGroupActive = 0x35;
constexpr uint32_t kGetPaneByName = 0x805E8368;
constexpr uint32_t kPositionSha = ArrayOverflow::kMenuText + 0x2080;  // "position_sha", not a game string
constexpr std::array<uint32_t, 3> kPanes{{kPositionSha, 0x808AA470 /* "position" */, 0x808AA179 /* "position_l_00" */}};

uint32_t g_control = 0;                         // the counter the saved textures belong to
bool g_saved = false;
std::array<std::array<uint8_t, kTexMapBytes>, 3> g_stock{};

uint32_t Call(CpuContext* ctx, const CpuContext& entry, uint32_t fn, uint32_t r3, uint32_t r4 = 0, uint32_t r5 = 0) {
    *ctx = entry;
    ctx->gpr[3] = r3; ctx->gpr[4] = r4; ctx->gpr[5] = r5;
    InvokeIndirectCpu(fn, ctx);
    return ctx->gpr[3];
}

uint32_t TexMap(CpuContext* ctx, const CpuContext& entry, uint32_t control, uint32_t name) {
    const uint32_t pane = Call(ctx, entry, kGetPaneByName, control + kLayout, name);
    if (pane < 0x80000000u) return 0;
    const uint32_t material = Call(ctx, entry, Memory::Read32(Memory::Read32(pane) + kGetMaterial), pane);
    if (material < 0x80000000u) return 0;
    const uint32_t texMap = Call(ctx, entry, kGetTexMapAry, material);
    return texMap < 0x80000000u ? 0 : texMap;
}
} // namespace

bool Show(CpuContext* ctx, uint32_t control, uint32_t place) {
    static bool named = false;
    if (!named) {
        named = true;
        const char* text = "position_sha";
        for (uint32_t i = 0;; ++i) { Memory::Write8(kPositionSha + i, uint8_t(text[i])); if (!text[i]) break; }
    }
    if (control != g_control) { g_control = control; g_saved = false; }  // a new race's counter
    const uint32_t animator = Memory::Read32(control + kAnimator);
    const bool icon = place >= kFirstPlace && Available(place);
    if (place >= kFirstPlace && !icon) return false;
    const CpuContext entry = *ctx;
    for (uint32_t i = 0; i < kPanes.size(); ++i) {
        const uint32_t texMap = TexMap(ctx, entry, control, kPanes[i]);
        if (texMap == 0) continue;
        if (icon) {
            if (!g_saved) for (uint32_t b = 0; b < kTexMapBytes; ++b) g_stock[i][b] = Memory::Read8(texMap + b);
            Call(ctx, entry, kReplaceImage, texMap, Slot(place), 0);  // TexMap::ReplaceImage(tpl, 0)
        } else if (g_saved) {
            for (uint32_t b = 0; b < kTexMapBytes; ++b) Memory::Write8(texMap + b, g_stock[i][b]);
        }
    }
    *ctx = entry;
    if (icon) g_saved = true;
    else g_saved = false;
    if (animator >= 0x80000000u) Memory::Write8(animator + kGroupActive, icon ? 0 : 1);  // group 0: eRank
    return true;
}

} // namespace RankIcons
