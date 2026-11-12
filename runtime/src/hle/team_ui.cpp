// mkw100 team colours in the race HUD (teams.h). As Retro Rewind's extended teams do
// (rr-pulsar ExtendedTeamSelectMisc.cpp, GPL-3), existing layout panes are tinted at runtime; no
// assets are added:
//   name balloons: every racer's character name (chara_name) in its team colour, after
//       CtrlRaceNameBalloon::UpdateInfo;
//   minimap: the local player's teammates get their marker's two shadow panes shown in the team
//       colour and are drawn on top, after CtrlRace2DMapCharacter::OnUpdate;
//   results rows: team_color_c (select_base on the local player's row) shown in the team colour,
//       whenever local_vs.cpp fills a row (TintResultRow).
// Name balloons: the tint is applied after the game's update, which overwrites what the balloon's
// animations set. Minimap markers: their colour animation repaints the outline every frame, so
// while CtrlRace2DMapCharacter::InitSelf / CalculatePosition run, AnimationGroup::
// PlayAnimationAtFrameAndDisable only deactivates the group, as RR does at those call sites.

#include "teams.h"

#include "abi_bridge.h"
#include "guest_abi/array_overflow.h"
#include "guest_function_imports.h"
#include "memory.h"

#include <cstdint>

namespace {

constexpr uint32_t kGetPaneByName = 0x805E8368;     // Layout::GetPaneByName(layout, name)
constexpr uint32_t kSetPaneVisibility = 0x8063E54C; // LayoutUIControl::SetPaneVisibility(control, name, bool)
constexpr uint32_t kLayout = 0xA8;                  // LayoutUIControl::layout
constexpr uint32_t kRootPane = 0x170;               // LayoutUIControl::rootPane
constexpr uint32_t kZIndex = 0x84;                  // UIControl::zIdx
constexpr uint32_t kPaneAlpha = 0xB8;               // nw4r::lyt::Pane::alpha
constexpr uint32_t kPaneTransZ = 0x34;              // Pane::trans.z
constexpr uint32_t kGetMaterial = 0x68;             // Pane vtable: GetMaterial()
constexpr uint32_t kTevColours = 0x10;              // Material: GX::ColorS10 tevColours[3]
constexpr uint32_t kTextColour = 0xDC;              // TextBox: ut::Color color1[2]

// Pane names, the game's own strings.
constexpr uint32_t kCharaName = 0x808A9F56;         // "chara_name"
constexpr uint32_t kShadow0 = 0x808A9504;           // "chara_shadow_0_0"
constexpr uint32_t kShadow1 = 0x808A9515;           // "chara_shadow_0_1"

// Pane names the game's own strings do not have, in the fixed runtime table span.
constexpr uint32_t kTeamColourPane = ArrayOverflow::kMenuText + 0x2000;  // "team_color_c"
constexpr uint32_t kSelectBasePane = kTeamColourPane + 0x20;             // "select_base"
constexpr uint32_t kPaneFlag = 0xBB;                                     // Pane::flag, bit 0 visible
constexpr uint32_t kRacedata = 0x809BD728;
constexpr uint32_t kRaceScenarioPlayers = 0x28;                          // Racedata + 0x20 + 8
constexpr uint32_t kPlayerType = 0x10;                                   // 0 = real local

constexpr uint32_t kBalloonPlayer = 4;              // UpdateInfo's player id argument (r4)
constexpr uint32_t kMapPlayer = 0x1B4;              // CtrlRace2DMapCharacter: u8 player id
constexpr uint32_t kMapChara = 0x1B8;               // charaPane, charaShadow0Pane, charaShadow1Pane

uint32_t TeamUiCall(CpuContext* ctx, const CpuContext& saved, uint32_t fn, uint32_t r3, uint32_t r4 = 0, uint32_t r5 = 0) {
    *ctx = saved;
    ctx->gpr[3] = r3; ctx->gpr[4] = r4; ctx->gpr[5] = r5;
    InvokeIndirectCpu(fn, ctx);
    return ctx->gpr[3];
}

bool TeamUiInRam(uint32_t address) { return address >= 0x80000000u && address < 0x98000000u; }

void TeamUiTint(CpuContext* ctx, const CpuContext& saved, uint32_t pane, const Teams::Colour& c, uint32_t first,
                  uint32_t last, int alpha) {
    const uint32_t material = TeamUiCall(ctx, saved, Memory::Read32(Memory::Read32(pane) + kGetMaterial), pane);
    if (!TeamUiInRam(material)) return;
    for (uint32_t i = first; i <= last; ++i) {
        const uint32_t colour = material + kTevColours + i * 8;  // s16 r, g, b, a
        Memory::Write16(colour + 0, c.r);
        Memory::Write16(colour + 2, c.g);
        Memory::Write16(colour + 4, c.b);
        if (alpha >= 0) Memory::Write16(colour + 6, uint16_t(alpha));
    }
}

void WriteName(uint32_t address, const char* text) {
    for (uint32_t i = 0;; ++i) {
        Memory::Write8(address + i, uint8_t(text[i]));
        if (!text[i]) break;
    }
}

} // namespace

extern "C" uint32_t MKW100_PlayerSlotOffset(uint32_t index);

void Teams::TintResultRow(CpuContext* ctx, uint32_t control, uint32_t player) {
    if (!Teams::Active()) return;
    const uint32_t team = Teams::TeamOf(player);
    if (team >= Teams::kMaxTeams) return;
    static bool named = false;
    if (!named) {
        named = true;
        WriteName(kTeamColourPane, "team_color_c");
        WriteName(kSelectBasePane, "select_base");
    }
    const CpuContext saved = *ctx;
    const uint32_t rd = Memory::Read32(kRacedata);
    const bool local = Memory::Read32(rd + MKW100_PlayerSlotOffset(player) + kRaceScenarioPlayers + kPlayerType) == 0;
    const uint32_t pane = TeamUiCall(ctx, saved, kGetPaneByName, control + kLayout, local ? kSelectBasePane : kTeamColourPane);
    if (TeamUiInRam(pane)) {
        Memory::Write8(pane + kPaneAlpha, local ? 255 : 200);
        Memory::Write8(pane + kPaneFlag, Memory::Read8(pane + kPaneFlag) | 1);
        TeamUiTint(ctx, saved, pane, Teams::ColourOf(team), 0, 1, 255);
    }
    *ctx = saved;
}

// CtrlRaceNameBalloon::UpdateInfo(this, playerId)
extern "C" void TeamUi_BalloonUpdateInfo_807f0044(CpuContext* ctx) {
    const uint32_t balloon = ctx->gpr[3];
    const uint32_t player = ctx->gpr[kBalloonPlayer] & 0xFF;
    MKW_GUEST_NEXT(807F0044)(ctx);
    if (!Teams::Active()) return;
    const uint32_t team = Teams::TeamOf(player);
    if (team >= Teams::kMaxTeams) return;
    const Teams::Colour& c = Teams::ColourOf(team);
    const CpuContext saved = *ctx;
    const uint32_t name = TeamUiCall(ctx, saved, kGetPaneByName, balloon + kLayout, kCharaName);
    if (TeamUiInRam(name)) {
        Memory::Write8(name + kTextColour + 0, c.r);  // color1[0], top
        Memory::Write8(name + kTextColour + 1, c.g);
        Memory::Write8(name + kTextColour + 2, c.b);
        Memory::Write8(name + kTextColour + 3, 255);
        Memory::Write8(name + kTextColour + 4, c.r);  // color1[1], bottom
        Memory::Write8(name + kTextColour + 5, c.g);
        Memory::Write8(name + kTextColour + 6, c.b);
        Memory::Write8(name + kTextColour + 7, 255);
        // The text material's TEV colour 1 to white, as RR does, so the vertex colour shows.
        const uint32_t material = TeamUiCall(ctx, saved, Memory::Read32(Memory::Read32(name) + kGetMaterial), name);
        if (TeamUiInRam(material)) {
            for (uint32_t k = 0; k < 3; ++k) Memory::Write16(material + kTevColours + 8 + k * 2, 255);
        }
    }
    *ctx = saved;
}
REGISTER_NATIVE_WRAPPER_AS(0x807F0044, TeamUi_BalloonUpdateInfo_807f0044, "TeamUi_BalloonUpdateInfo_807f0044");

namespace {
bool gInMapMarker = false;  // game thread only
}

// AnimationGroup::PlayAnimationAtFrameAndDisable(group, id, frame)
extern "C" void TeamUi_PlayAnimationAndDisable_8063cb20(CpuContext* ctx) {
    if (gInMapMarker) {
        Memory::Write8(ctx->gpr[3] + 0x35, 0);  // isActive = false
        return;
    }
    MKW_GUEST_NEXT(8063CB20)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x8063CB20, TeamUi_PlayAnimationAndDisable_8063cb20, "TeamUi_PlayAnimationAndDisable_8063cb20");

// CtrlRace2DMapCharacter::InitSelf(this) and ::CalculatePosition(this, pos, dest, r6)
extern "C" void TeamUi_MapCharacterInit_807eaecc(CpuContext* ctx) {
    gInMapMarker = Teams::Active();
    MKW_GUEST_NEXT(807EAECC)(ctx);
    gInMapMarker = false;
}
REGISTER_NATIVE_WRAPPER_AS(0x807EAECC, TeamUi_MapCharacterInit_807eaecc, "TeamUi_MapCharacterInit_807eaecc");
extern "C" void TeamUi_MapCharacterPosition_807eb3cc(CpuContext* ctx) {
    gInMapMarker = Teams::Active();
    MKW_GUEST_NEXT(807EB3CC)(ctx);
    gInMapMarker = false;
}
REGISTER_NATIVE_WRAPPER_AS(0x807EB3CC, TeamUi_MapCharacterPosition_807eb3cc, "TeamUi_MapCharacterPosition_807eb3cc");

// CtrlRace2DMapCharacter::OnUpdate(this)
extern "C" void TeamUi_MapCharacterUpdate_807eb250(CpuContext* ctx) {
    const uint32_t marker = ctx->gpr[3];
    MKW_GUEST_NEXT(807EB250)(ctx);
    if (!Teams::Active()) return;
    const uint32_t player = Memory::Read8(marker + kMapPlayer);
    const uint32_t team = Teams::TeamOf(player);
    if (team >= Teams::kMaxTeams || team != Teams::TeamOf(0)) return;  // the local player's team only
    const Teams::Colour& c = Teams::ColourOf(team);
    const CpuContext saved = *ctx;
    TeamUiCall(ctx, saved, kSetPaneVisibility, marker, kShadow0, 1);
    TeamUiCall(ctx, saved, kSetPaneVisibility, marker, kShadow1, 1);
    for (uint32_t i = 0; i < 3; ++i) {
        const uint32_t pane = Memory::Read32(marker + kMapChara + 4 * i);
        if (!TeamUiInRam(pane)) continue;
        Memory::Write8(pane + kPaneAlpha, 255);
        if (i > 0) TeamUiTint(ctx, saved, pane, c, 0, 1, 255);
    }
    Memory::Write32(marker + kZIndex, 20);  // over the other markers
    const uint32_t root = Memory::Read32(marker + kRootPane);
    if (TeamUiInRam(root)) Memory::WriteFloat32(root + kPaneTransZ, 20.0f);
    *ctx = saved;
}
REGISTER_NATIVE_WRAPPER_AS(0x807EB250, TeamUi_MapCharacterUpdate_807eb250, "TeamUi_MapCharacterUpdate_807eb250");

// AnimationGroup::PlayAnimationAtFrame(group, id, frame) from the results rows (CtrlRaceResult):
// their colour animation repaints the row every frame over the team tint, so with teams it is
// switched off where the row is loaded and skipped where the row is filled, as Retro Rewind's
// extended teams do (ExtendedTeamSelectMisc.cpp, CtrlRaceResult_InitPatchAnimation /
// CalcSkipAnimation, same call sites).
extern "C" void TeamUi_PlayAnimation_8063c91c(CpuContext* ctx) {
    const uint32_t lr = ctx->lr;
    const bool load = lr == 0x807F4F78 || lr == 0x807F4F90;               // CtrlRaceResult::Load
    const bool fill = lr == 0x807F631C || lr == 0x807F63A8 || lr == 0x807F6414;  // CtrlRaceResult::Fill
    if ((load || fill) && Teams::Active()) {
        Memory::Write8(ctx->gpr[3] + 0x35, 0);  // isActive = false (Load may run before teams are dealt)
        return;
    }
    MKW_GUEST_NEXT(8063C91C)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x8063C91C, TeamUi_PlayAnimation_8063c91c, "TeamUi_PlayAnimation_8063c91c");
