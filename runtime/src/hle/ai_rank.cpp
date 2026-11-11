// mkw100: the AI rank manager's re-ranking (0x80742584 CPUs, 0x807426E4 real players) for races
// of more than 12.
//
// AIRankManager (class ai_rank, runtime/include/guest_abi/array_overflow.h) holds {AI*, AI*}[12]
// for the CPUs at +0xE8 (count +0x178) and {AI*, AI*}[4] for the real players at +0x148 (count
// +0x17C). Each function buckets its AIs by rank (AI +0x14, 1-based) into a stack
// `AI* byRank[12]`, then walks the 12 buckets (unrolled 2 x 6) and lists the AIs found in rank
// order in the elements' second word; the CPU version also renumbers their ranks 1..n. A rank of
// 13+ lands on the caller's saved registers (24 CPUs: AI::Manager::Update's `this` became an AI
// pointer, then a call through 0).
//
// While every count and rank fits 12 the game's functions run unchanged; otherwise these do the
// same with buckets for every possible rank, CPU elements 12+ in the class's shadow windows.

#include "guest_abi/array_overflow.h"

#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "memory.h"

#include <array>
#include <cstdint>

namespace {

constexpr uint32_t kAiRankClass = 2;
constexpr uint32_t kRankArray = 0xE8;     // {AI* ai; AI* byRank;}[12]
constexpr uint32_t kRankStride = 8;
constexpr uint32_t kRankCpuCount = 0x178;
constexpr uint32_t kRealArray = 0x148;    // {AI* ai; AI* byRank;}[4]
constexpr uint32_t kRankRealCount = 0x17C;
constexpr uint32_t kRealSlots = 4;
constexpr uint32_t kAiRank = 0x14;        // s32, 1-based

uint32_t RankElement(const ArrayOverflow::ArrayClass& c, uint32_t self, uint32_t index) {
    if (index < ArrayOverflow::kInlineSlots) {
        return self + kRankArray + index * kRankStride;
    }
    return ArrayOverflow::Window(c, index) + kRankArray;
}

// The AIs of one list bucketed by rank, in rank order; false when the game's own 12 buckets would
// do (every count and rank within 12), so the caller runs the game's function.
using Ranked = std::array<uint32_t, ArrayOverflow::kMaxPlayers>;
template <typename Element>
bool Bucket(uint32_t count, Element element, Ranked& byRank) {
    bool fits = count <= ArrayOverflow::kInlineSlots;
    for (uint32_t i = 0; i < count && i < byRank.size(); ++i) {
        const uint32_t ai = Memory::Read32(element(i));
        const int32_t rank = static_cast<int32_t>(Memory::Read32(ai + kAiRank));
        fits = fits && rank <= static_cast<int32_t>(ArrayOverflow::kInlineSlots);
        // The game skips negative ranks; rank 0 would write byRank[-1] (never seen), skip it too.
        if (rank >= 1 && rank <= static_cast<int32_t>(byRank.size())) {
            byRank[rank - 1] = ai;
        }
    }
    return !fits;
}

} // namespace

extern "C" void AIRankManager_RerankCpus_80742584(CpuContext* ctx) {
    const uint32_t self = ctx->gpr[3];
    const uint32_t count = Memory::Read32(self + kRankCpuCount);
    const ArrayOverflow::ArrayClass& c = ArrayOverflow::Class(kAiRankClass);
    Ranked byRank{};
    if (static_cast<int32_t>(count) <= 0 ||
        !Bucket(count, [&](uint32_t i) { return RankElement(c, self, i); }, byRank)) {
        MKW_GUEST_NEXT(80742584)(ctx);
        return;
    }
    uint32_t next = 0;
    for (const uint32_t ai : byRank) {
        if (ai == 0) {
            continue;
        }
        Memory::Write32(ai + kAiRank, next + 1);
        Memory::Write32(RankElement(c, self, next) + 4, ai);
        ++next;
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x80742584, AIRankManager_RerankCpus_80742584, "AIRankManager_RerankCpus_80742584");

extern "C" void AIRankManager_RerankReal_807426e4(CpuContext* ctx) {
    const uint32_t self = ctx->gpr[3];
    const uint32_t count = Memory::Read32(self + kRankRealCount);
    Ranked byRank{};
    if (static_cast<int32_t>(count) <= 0 || count > kRealSlots ||
        !Bucket(count, [&](uint32_t i) { return self + kRealArray + i * kRankStride; }, byRank)) {
        MKW_GUEST_NEXT(807426E4)(ctx);
        return;
    }
    uint32_t next = 0;
    for (const uint32_t ai : byRank) {
        if (ai != 0) {
            Memory::Write32(self + kRealArray + next++ * kRankStride + 4, ai);
        }
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x807426E4, AIRankManager_RerankReal_807426e4, "AIRankManager_RerankReal_807426e4");
