// Local offline VS integration. Guest memory is only touched on the game thread.
#include "local_vs.h"
#include "abi_bridge.h"
#include "boot_race.h"
#include "guest_function_imports.h"
#include "guest_abi/player_slots.h"
#include "memory.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include <imgui.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <vector>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace local_vs_detail {
std::atomic<int> desiredCount{100};
std::atomic<bool> unlocks{true};
std::atomic<bool> diversity{true};
std::atomic<bool> thirtyFps{false};
std::atomic<bool> instantBoxes{true};
std::atomic<uint32_t> hudRank{0};
std::atomic<int64_t> hudTime{0};
std::atomic<uint32_t> simulationRate{0};
int64_t NowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
uint32_t seriesCount = 0;
struct Entry { uint32_t id, character, place, earned, total; };
std::mutex standingsMutex;
std::vector<Entry> standings;
constexpr std::array<uint32_t,24> weights{{1,0,2,2,0,0,0,1,0,2,1,2,0,0,0,1,1,1,1,2,1,2,2,2}};
constexpr std::array<const char*,24> names{{"Mario","Baby Peach","Waluigi","Bowser","Baby Daisy","Dry Bones","Baby Mario","Luigi","Toad","Donkey Kong","Yoshi","Wario","Baby Luigi","Toadette","Koopa Troopa","Daisy","Peach","Birdo","Diddy Kong","King Boo","Bowser Jr.","Dry Bowser","Funky Kong","Rosalina"}};
uint32_t Player(uint32_t scenario, uint32_t id) {
    return PlayerSlots::PlayerAddress(Memory::Read32(MKW_GADDR(809BD728)), scenario, id);
}
uint32_t RaceCount() {
    const auto rd = Memory::Read32(MKW_GADDR(809BD728));
    return rd ? Memory::Read8(rd + 0x24) : 0;
}
bool ExpandedOffline() {
    const auto rd = Memory::Read32(MKW_GADDR(809BD728));
    return rd && RaceCount() > 12 && RaceCount() <= PlayerSlots::kMaxPlayers &&
           Memory::Read32(rd + 0x20 + 0xB50) == 1 && Memory::Read32(rd + 0x20 + 0xB54) == 0;
}
}

namespace LocalVs {
void Initialize() {
    const auto& c = RuntimeConfigFile::Get();
    local_vs_detail::desiredCount = std::clamp(int(c.localVsPlayers.value_or(100)), 1, int(PlayerSlots::kMaxPlayers));
    local_vs_detail::unlocks = c.localVsUnlocks.value_or(true);
    local_vs_detail::diversity = c.localVsDiversity.value_or(true);
    local_vs_detail::thirtyFps = c.localVsThirtyFps.value_or(false);
    local_vs_detail::instantBoxes = c.localVsInstantBoxes.value_or(true);
}
bool UnlocksEnabled() { return !BootRace::CpuDriven() && local_vs_detail::unlocks.load(); }
void DrawSettings() {
    using namespace local_vs_detail;
    int count = desiredCount.load();
    ImGui::SetNextItemWidth(240);
    if (ImGui::SliderInt("Racers", &count, 1, PlayerSlots::kMaxPlayers, "%d", ImGuiSliderFlags_AlwaysClamp)) {
        desiredCount = count;
        RuntimeConfigFile::WriteSetting("local_vs", "players", std::to_string(count));
    }
    ImGui::TextUnformatted("Applies when starting a new single-player VS series.");
    bool varied = diversity.load();
    if (ImGui::Checkbox("Varied CPU characters, karts and bikes", &varied)) {
        diversity = varied;
        RuntimeConfigFile::WriteSetting("local_vs", "diverse_roster", varied ? "true" : "false");
    }
    bool all = unlocks.load();
    if (ImGui::Checkbox("All characters, vehicles and cups available", &all)) {
        unlocks = all;
        RuntimeConfigFile::WriteSetting("local_vs", "unlock_all", all ? "true" : "false");
    }
    ImGui::TextUnformatted("Reopen character selection after changing unlocks.");
    bool half = thirtyFps.load();
    if (ImGui::Checkbox("30 FPS rendering (experimental, next race)", &half)) {
        thirtyFps = half;
        RuntimeConfigFile::WriteSetting("local_vs", "render_30fps", half ? "true" : "false");
    }
    bool boxes = instantBoxes.load();
    if (ImGui::Checkbox("Instant item boxes in expanded races", &boxes)) {
        instantBoxes = boxes;
        RuntimeConfigFile::WriteSetting("local_vs", "instant_boxes", boxes ? "true" : "false");
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Expanded scoring: N..1, plus +3 for first and +1 for second.");
    std::vector<Entry> copy;
    { std::lock_guard lock(standingsMutex); copy = standings; }
    if (!copy.empty() && ImGui::BeginTable("Full race results", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(520,320))) {
        ImGui::TableSetupColumn("Place"); ImGui::TableSetupColumn("Racer");
        ImGui::TableSetupColumn("Earned"); ImGui::TableSetupColumn("Total"); ImGui::TableHeadersRow();
        for (const auto& e : copy) {
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Text("%u", e.place);
            ImGui::TableNextColumn(); ImGui::Text("%s #%u%s", e.character < names.size() ? names[e.character] : "Mii", e.id+1, e.id == 0 ? " (you)" : "");
            ImGui::TableNextColumn(); ImGui::Text("+%u", e.earned);
            ImGui::TableNextColumn(); ImGui::Text("%u", e.total);
        }
        ImGui::EndTable();
    }
}

// The stock position counter is a layout animation with twelve pictures, and the game's
// pictures may not be redistributed, so places past 12 are drawn here: slanted seven-segment
// digits with an outline, gold for the podium, shading to blue towards last. Everything is
// sized from the window height, so it stays sharp at any resolution.
namespace {
// Segments a b c d e f g per digit, the usual seven-segment layout.
constexpr std::array<uint8_t,10> kSegments{{0x3F,0x06,0x5B,0x4F,0x66,0x6D,0x7D,0x07,0x7F,0x6F}};

void DrawSegmentDigit(ImDrawList* list, ImVec2 origin, float height, uint32_t digit, ImU32 fill, ImU32 outline) {
    const float w = height * 0.5f, t = height * 0.14f, slant = height * 0.12f;
    // Segment rectangles in a unit box (x right, y down, 0..1), slanted afterwards.
    struct Seg { float x0, y0, x1, y1; };
    const float h = 0.5f, tw = t / w, th = t / height;
    const Seg segs[7] = {{tw,0,1-tw,th},{1-tw,th,1,h-th/2},{1-tw,h+th/2,1,1-th},{tw,1-th,1-tw,1},
                         {0,h+th/2,tw,1-th},{0,th,tw,h-th/2},{tw,h-th/2,1-tw,h+th/2}};
    auto point = [&](float x, float y) { return ImVec2(origin.x + x*w + (1-y)*slant, origin.y + y*height); };
    for (int pass = 0; pass < 2; ++pass) {
        const float grow = pass == 0 ? t * 0.35f : 0.0f;
        for (int i = 0; i < 7; ++i) {
            if (!(kSegments[digit] >> i & 1)) continue;
            const Seg& g = segs[i];
            const float gx = grow / w, gy = grow / height;
            const ImVec2 q[4] = {point(g.x0-gx,g.y0-gy), point(g.x1+gx,g.y0-gy), point(g.x1+gx,g.y1+gy), point(g.x0-gx,g.y1+gy)};
            list->AddConvexPolyFilled(q, 4, pass == 0 ? outline : fill);
        }
    }
}

float DrawSegmentNumber(ImDrawList* list, ImVec2 origin, float height, uint32_t value, ImU32 fill, ImU32 outline) {
    char text[8];
    std::snprintf(text, sizeof text, "%u", value);
    for (const char* c = text; *c; ++c) {
        DrawSegmentDigit(list, origin, height, uint32_t(*c - '0'), fill, outline);
        origin.x += height * 0.72f;
    }
    return origin.x;
}
}

void DrawHud() {
    using namespace local_vs_detail;
    const uint32_t packed=hudRank.load();
    if (!packed || NowMs()-hudTime.load()>250) return;
    const uint32_t rank = packed & 255, count = packed >> 8;
    const auto* vp=ImGui::GetMainViewport();
    ImDrawList* list = ImGui::GetForegroundDrawList();
    const float big = vp->Size.y * 0.13f, small = big * 0.38f;
    // Gold for 1st to 3rd, then from white-blue to deep blue towards last.
    const float f = count > 1 ? float(rank - 1) / float(count - 1) : 0.0f;
    const ImU32 fill = rank <= 3 ? IM_COL32(255, 205, 40, 255)
        : IM_COL32(int(230 - 190*f), int(240 - 150*f), 255, 255);
    const ImU32 outline = IM_COL32(15, 15, 30, 235);
    char total[8];
    std::snprintf(total, sizeof total, "%u", count);
    const float width = big * 0.72f * float(rank >= 100 ? 3 : rank >= 10 ? 2 : 1) +
                        small * 0.72f * float(std::strlen(total) + 1);
    ImVec2 at(vp->Pos.x + vp->Size.x * 0.96f - width, vp->Pos.y + vp->Size.y * 0.80f);
    const float end = DrawSegmentNumber(list, at, big, rank, fill, outline);
    // "/N" in small digits on the baseline.
    const ImVec2 slash0(end + small*0.25f, at.y + big), slash1(end + small*0.65f, at.y + big - small);
    list->AddLine(slash0, slash1, outline, small * 0.22f);
    list->AddLine(slash0, slash1, IM_COL32(235,235,235,255), small * 0.12f);
    DrawSegmentNumber(list, ImVec2(end + small*0.75f, at.y + big - small), small, count, IM_COL32(235,235,235,255), outline);
    // Game speed stays in a small corner readout.
    const uint32_t rate = simulationRate.load();
    if (rate) {
        char speed[32];
        std::snprintf(speed, sizeof speed, "Game speed %u%%", rate*100/60);
        list->AddText(ImVec2(vp->Pos.x + vp->Size.x*0.96f - ImGui::CalcTextSize(speed).x, at.y + big + small*0.3f), IM_COL32(235,235,235,200), speed);
    }
}

// Opt-in integration exercise using real race objects, never enabled by a launcher.
void TestItems(CpuContext* ctx, uint32_t info) {
    static uint32_t observedInfo=0, lastFrame=0;
    static int64_t lastTime=0;
    const int64_t now=local_vs_detail::NowMs();
    const uint32_t frame=Memory::Read32(info+0x20);
    if (observedInfo!=info || frame<lastFrame || (Memory::Read32(0x809C1874)&1)) {
        observedInfo=info; lastFrame=frame; lastTime=now;
    } else if (now-lastTime>=1000) {
        local_vs_detail::simulationRate=uint32_t((frame-lastFrame)*1000/(now-lastTime));
        lastFrame=frame; lastTime=now;
    }
    static const bool enabled=std::getenv("MKW100_TEST_ITEMS")!=nullptr;
    static bool fired=false;
    if (!enabled || !BootRace::CpuDriven() || fired || !local_vs_detail::ExpandedOffline() ||
        Memory::Read32(info+0x28)!=2 || Memory::Read32(info+0x20)<600) return;
    fired=true;
    const CpuContext saved=*ctx;
    const uint32_t count=local_vs_detail::RaceCount();
    ctx->gpr[3]=count-1;
    MKW_GUEST_FUNC(807B7C20)(ctx);
    *ctx=saved;
    const uint32_t manager=Memory::Read32(0x809C3660); // PAL PowMgr singleton
    const uint32_t players=Memory::Read32(info+0xC);
    uint32_t powOwner=12, worstPlace=0;
    for (uint32_t i=12;i<count;++i) {
        const uint32_t place=Memory::Read8(Memory::Read32(players+4*i)+0x20);
        if (place>worstPlace) { worstPlace=place; powOwner=i; }
    }
    ctx->gpr[3]=manager; ctx->gpr[4]=powOwner;
    InvokeIndirectCpu(MKW_GADDR(807B1DB4),ctx); // exercise the patched entry, not the base translation
    *ctx=saved;
    RT_LOGF("local-vs", "mechanics test injected lightning from slot %u and POW from slot %u (place %u)\n",count-1,powOwner,worstPlace);
}

void PrepareRace(uint32_t rd) {
    using namespace local_vs_detail;
    if (BootRace::Requested()) return; // keep the deterministic direct-boot instrument separate
    const uint32_t menu = rd + 0xC10;
    RT_LOGF("local-vs", "prepare: mode=%u type=%u flags=0x%X p0=%u p1=%u raceNumber=%u\n",
        Memory::Read32(menu+0xB50),Memory::Read32(menu+0xB54),Memory::Read32(menu+0xB70),
        Memory::Read32(Player(menu,0)+0x10),Memory::Read32(Player(menu,1)+0x10),Memory::Read8(menu+0xB6C));
    const uint32_t gameType=Memory::Read32(menu+0xB54);
    const uint32_t firstType=Memory::Read32(Player(menu,0)+0x10);
    const bool preview=gameType==5;
    bool solo = firstType == 0 || (preview && firstType == 1);
    for (uint32_t i=1;i<12;++i) solo = solo && Memory::Read32(Player(menu,i)+0x10) != 0;
    const bool eligible = solo && Memory::Read32(menu+0xB50)==1 && (gameType==0 || preview) &&
                          (Memory::Read32(menu+0xB70)&2)==0; // ordinary offline VS, no teams
    if (!eligible) {
        for (uint32_t i=12;i<PlayerSlots::kMaxPlayers;++i) Memory::Write32(Player(menu,i)+0x10,5);
        seriesCount=0;
        return;
    }
    const bool first = Memory::Read8(menu+0xB6C)==0 || seriesCount==0;
    if (first) {
        seriesCount=desiredCount.load();
        std::lock_guard lock(standingsMutex); standings.clear();
    }
    const uint32_t humanCharacter=Memory::Read32(Player(menu,0)+0xC);
    for (uint32_t i=0;i<PlayerSlots::kMaxPlayers;++i) {
        const uint32_t p=Player(menu,i);
        Memory::Write32(p+0x10, i>=seriesCount ? 5 : i==0 ? firstType : 1);
        if (i && i<seriesCount && (diversity.load() || i>=12)) {
            const uint32_t character=(humanCharacter+ i)%24;
            const uint32_t vehicle=weights[character]+3*((i/3)%12);
            Memory::Write32(p+0xC,character); Memory::Write32(p+8,vehicle);
        }
        if (first) {
            Memory::Write16(p+0xD8,0); Memory::Write16(p+0xDA,0);
            Memory::Write8(p+0xE1, i<seriesCount ? seriesCount-i : 0);
            Memory::Write8(p+0xE2, i<seriesCount ? seriesCount-i : 0);
        }
    }
    RT_LOGF("local-vs", "menu race: course=0x%02X racers=%u raceNumber=%u humanCharacter=%u\n",
            Memory::Read32(menu+0xB48),seriesCount,Memory::Read8(menu+0xB6C),humanCharacter);
}
}

// The vanilla function has a 12-entry stack array and indexes pointsRoom[12][12].
// Never call it for expanded offline VS, even to repair the result afterward.
extern "C" void LocalVs_UpdatePoints_8052e950(CpuContext* ctx) {
    using namespace local_vs_detail;
    if (!ExpandedOffline()) { MKW_GUEST_NEXT(8052E950)(ctx); return; }
    const uint32_t menu=ctx->gpr[3], rd=Memory::Read32(MKW_GADDR(809BD728));
    const uint32_t info=Memory::Read32(MKW_GADDR(809BD730));
    const uint32_t infos=Memory::Read32(info+0xC), count=RaceCount();
    std::vector<Entry> rows;
    for (uint32_t i=0;i<count;++i) {
        const uint32_t p=Player(menu,i), raceP=Player(rd+0x20,i);
        const uint32_t place=Memory::Read8(Memory::Read32(infos+4*i)+0x20);
        const uint32_t bonus=place==1 ? 3 : place==2 ? 1 : 0;
        const uint32_t earned=(place>=1 && place<=count) ? count-place+1+bonus : 0;
        const uint32_t previous=Memory::Read16(raceP+0xD8);
        const uint32_t total=std::min(65535u,previous+earned);
        Memory::Write16(p+0xD8,previous); Memory::Write16(p+0xDA,total);
        Memory::Write8(p+0xE2,place);
        rows.push_back({i,Memory::Read32(raceP+0xC),place,earned,total});
    }
    std::stable_sort(rows.begin(),rows.end(),[](const Entry& a,const Entry& b){return a.place<b.place;});
    { std::lock_guard lock(standingsMutex); standings=std::move(rows); }
    RT_LOGF("local-vs", "scored %u racers without vanilla 12-player table\n",count);
}
REGISTER_NATIVE_WRAPPER_AS(0x8052E950, LocalVs_UpdatePoints_8052e950, "LocalVs_UpdatePoints_8052e950");

// Only twelve native layouts exist. Full results remain available in F10 > Local VS.
extern "C" void LocalVs_RowCount_8085c4f4(CpuContext* ctx) {
    MKW_GUEST_NEXT(8085C4F4)(ctx);
    ctx->gpr[3]=std::min(ctx->gpr[3],12u);
}
REGISTER_NATIVE_WRAPPER_AS(0x8085C4F4, LocalVs_RowCount_8085c4f4, "LocalVs_RowCount_8085c4f4");

// The original total page writes count entries even when GetRowCount is smaller.
extern "C" void LocalVs_TotalRows_8085cae8(CpuContext* ctx) {
    using namespace local_vs_detail;
    if (!ExpandedOffline()) { MKW_GUEST_NEXT(8085CAE8)(ctx); return; }
    const uint32_t page=ctx->gpr[3];
    std::vector<Entry> rows;
    { std::lock_guard lock(standingsMutex); rows=standings; }
    std::stable_sort(rows.begin(),rows.end(),[](const Entry&a,const Entry&b){return a.total>b.total;});
    const CpuContext saved=*ctx;
    for (uint32_t i=0;i<std::min<uint32_t>(12,rows.size());++i) {
        const uint32_t control=Memory::Read32(Memory::Read32(page+0x1A0)+4*i);
        const uint32_t entry=Memory::Read32(page+0x1A4)+12*i;
        Memory::Write32(entry,rows[i].total); Memory::Write32(entry+4,rows[i].earned); Memory::Write8(entry+8,rows[i].id);
        *ctx=saved; ctx->gpr[3]=control; ctx->gpr[4]=i+1; ctx->gpr[5]=rows[i].id; MKW_GUEST_FUNC(807F5FEC)(ctx);
        *ctx=saved; ctx->gpr[3]=control; ctx->gpr[4]=rows[i].total; ctx->gpr[5]=1313; MKW_GUEST_FUNC(807F56D4)(ctx);
    }
    *ctx=saved;
}
REGISTER_NATIVE_WRAPPER_AS(0x8085CAE8, LocalVs_TotalRows_8085cae8, "LocalVs_TotalRows_8085cae8");

extern "C" void LocalVs_CharacterUnlocked_8054fed8(CpuContext* ctx) {
    if (LocalVs::UnlocksEnabled()) ctx->gpr[3]=1; else MKW_GUEST_NEXT(8054FED8)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x8054FED8, LocalVs_CharacterUnlocked_8054fed8, "LocalVs_CharacterUnlocked_8054fed8");
extern "C" void LocalVs_KartUnlocked_805500cc(CpuContext* ctx) {
    if (LocalVs::UnlocksEnabled()) ctx->gpr[3]=1; else MKW_GUEST_NEXT(805500CC)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x805500CC, LocalVs_KartUnlocked_805500cc, "LocalVs_KartUnlocked_805500cc");
extern "C" void LocalVs_CupUnlocked_8054fc44(CpuContext* ctx) {
    if (LocalVs::UnlocksEnabled()) ctx->gpr[3]=1; else MKW_GUEST_NEXT(8054FC44)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x8054FC44, LocalVs_CupUnlocked_8054fc44, "LocalVs_CupUnlocked_8054fc44");

// The stock rank animation only contains twelve pictures. Publish a numeric HUD
// on the game thread; the overlay never reads guest objects that may be destroyed.
extern "C" void LocalVs_Rank_807f4a08(CpuContext* ctx) {
    const uint32_t self=ctx->gpr[3];
    MKW_GUEST_NEXT(807F4A08)(ctx);
    if (!local_vs_detail::ExpandedOffline()) return;
    const uint32_t id=Memory::Read32(self+0x19C);
    if (id!=0) return; // current menu integration supports one local player
    const uint32_t info=Memory::Read32(MKW_GADDR(809BD730));
    const uint32_t player=Memory::Read32(Memory::Read32(info+0xC)+4*id);
    const uint32_t rank=Memory::Read8(player+0x20);
    if (!rank || rank>local_vs_detail::RaceCount()) return;
    Memory::Write8(self+0x80,1);
    local_vs_detail::hudRank=rank | (local_vs_detail::RaceCount()<<8);
    local_vs_detail::hudTime=local_vs_detail::NowMs();
}
REGISTER_NATIVE_WRAPPER_AS(0x807F4A08, LocalVs_Rank_807f4a08, "LocalVs_Rank_807f4a08");

// Equivalent to preventing the inactive-state store in Anarion's instant-box
// patch (PAL 80828EDC). Keep the original collision, roulette and sound behavior.
// Reference: https://mariokartwii.com/showthread.php?tid=2206 (Unnamed, credits Anarion).
extern "C" void LocalVs_ItemBox_80828cb8(CpuContext* ctx) {
    const uint32_t self=ctx->gpr[3];
    const bool instant=local_vs_detail::ExpandedOffline() && local_vs_detail::instantBoxes.load();
    const uint32_t active=Memory::Read32(self+0xB0);
    MKW_GUEST_NEXT(80828CB8)(ctx);
    if (instant && active==1) Memory::Write32(self+0xB0,1);
}
REGISTER_NATIVE_WRAPPER_AS(0x80828CB8, LocalVs_ItemBox_80828cb8, "LocalVs_ItemBox_80828cb8");

// The normal effect function retains star/bullet/other immunity handling.
// Only replace the rank-to-duration lookup, whose stock guard rejects rank >12.
extern "C" void LocalVs_Lightning_80580438(CpuContext* ctx) {
    if (!local_vs_detail::ExpandedOffline()) { MKW_GUEST_NEXT(80580438)(ctx); return; }
    const uint32_t self=ctx->gpr[3];
    const uint32_t id=Memory::Read8(Memory::Read32(Memory::Read32(self))+0x10);
    const uint32_t count=local_vs_detail::RaceCount();
    if (id>=count) { ctx->gpr[3]=0; return; }
    const uint32_t info=Memory::Read32(MKW_GADDR(809BD730));
    const uint32_t place=Memory::Read8(Memory::Read32(Memory::Read32(info+0xC)+4*id)+0x20);
    if (!place || place>count) { ctx->gpr[3]=0; return; }
    const uint32_t row=(place-1)*11/(count-1);
    const uint32_t frames=Memory::Read16(0x80891A20u+2*row)+72; // PAL duration table
    ctx->gpr[4]=frames; ctx->gpr[5]=0; ctx->gpr[6]=0;
    MKW_GUEST_FUNC(80580778)(ctx);
    RT_LOGF("local-vs", "lightning: slot=%u place=%u duration=%u applied=%u\n",id,place,frames,ctx->gpr[3]);
}
REGISTER_NATIVE_WRAPPER_AS(0x80580438, LocalVs_Lightning_80580438, "LocalVs_Lightning_80580438");

// RKSystem's existing half-rate path alternates a simulation-only iteration
// with a rendered iteration. Menus and the deterministic CPU oracle stay default.
extern "C" void LocalVs_Framerate_8051af84(CpuContext* ctx) {
    const bool test30=BootRace::CpuDriven() && std::getenv("MKW100_TEST_30FPS")!=nullptr;
    if (local_vs_detail::ExpandedOffline() && (test30 || (!BootRace::CpuDriven() && local_vs_detail::thirtyFps.load()))) {
        ctx->gpr[4]=1;
        RT_LOGF("local-vs", "alternate-frame rendering enabled\n");
    }
    MKW_GUEST_NEXT(8051AF84)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x8051AF84, LocalVs_Framerate_8051af84, "LocalVs_Framerate_8051af84");
