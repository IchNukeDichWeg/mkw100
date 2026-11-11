// mkw100: Retro Rewind kart archives and character skins past 12 racers.
//
// Retro Rewind ships up to 100 alternative skins per character (Character/AllKart
// "<kart>-<char>-<n>-allkart*.szs", Character/Driver "<char>-<n>.brres"). Its Code.pul scans which
// exist into a [24][101] flag table (0x81877CF8, built at 0x81802628) and lets each of the 4 local
// players pick one in the character menu, a [4][24] byte table (0x81878670).
//
// Its kart archive code gets the racer from an ArchiveMgr element address: (element - ArchiveMgr -
// 8) / 0x1C in the kart path builder 0x818026FC, and the same for the arrays at 0x8 and 0x158 in
// the stubs 0x81832DFC and 0x81832E24 in front of 0x81832CC4. mkw100 moves elements 12+ of those
// arrays out of line (array_overflow.h, class archive_manager), so with 13+ racers that division
// gave a garbage index and 0x81832CC4 read far out of bounds (24 racers crashed loading the karts).
// Here the index comes from ArrayOverflow::ElementIndex instead.
//
// The path builder reads row <racer> of the [4][24] table, which past row 3 was outside the table.
// For racers past 3 the call borrows row 3, zeroed (the stock look), and restores it after.
// Retro Rewind product only: the base game never calls these addresses.

#include "abi_bridge.h"
#include "guest_abi/array_overflow.h"
#include "guest_function_imports.h"
#include "memory.h"

#include <cstdint>

namespace {

constexpr uint32_t kArchiveClass = 9;              // archive_manager in array_overflow.h
constexpr uint32_t kArchiveStride = 0x1C;          // ArchivesHolder
constexpr uint32_t kArchiveModels = 0x8;           // kartModelsHolders[12]
constexpr uint32_t kArchiveModels2 = 0x158;        // kartModelsHolders2[12]
constexpr uint32_t kSkinChosen = 0x81878670;       // u8 [4][24]: local player's skin per character
constexpr uint32_t kSkinCharacters = 24;
constexpr uint32_t kSkinRows = 4;
constexpr uint32_t kSkinBorrowRow = 3;

uint32_t ArchiveRacer(uint32_t element, uint32_t origin) {
    return ArrayOverflow::ElementIndex(ArrayOverflow::Class(kArchiveClass), element, origin, kArchiveStride);
}

// The stubs: r30 ArchiveMgr, r31 the element; r10 the racer for 0x81832CC4.
void ArchiveStub(CpuContext* ctx, uint32_t origin) {
    const uint32_t element = ctx->gpr[31];
    ctx->gpr[11] = element - ctx->gpr[30];
    ctx->gpr[0] = ctx->gpr[11] - origin;
    const uint32_t racer = ArchiveRacer(element, origin);
    ctx->gpr[10] = racer != ArrayOverflow::kNoArray ? racer : ctx->gpr[0] / kArchiveStride;
    InvokeIndirectCpu(0x81832CC4, ctx);  // module code, not in the base map
}

} // namespace

extern "C" void RR_ArchiveModels_81832dfc(CpuContext* ctx) { ArchiveStub(ctx, kArchiveModels); }
REGISTER_NATIVE_REPLACEMENT_AS(0x81832DFC, RR_ArchiveModels_81832dfc, "RR_ArchiveModels_81832dfc");

extern "C" void RR_ArchiveModels2_81832e24(CpuContext* ctx) { ArchiveStub(ctx, kArchiveModels2); }
REGISTER_NATIVE_REPLACEMENT_AS(0x81832E24, RR_ArchiveModels2_81832e24, "RR_ArchiveModels2_81832e24");

// The kart path builder; r7 is the racer's kartModelsHolders element.
extern "C" void RR_KartSkinPath_818026fc(CpuContext* ctx) {
    const uint32_t racer = ArchiveRacer(ctx->gpr[7], kArchiveModels);
    if (racer == ArrayOverflow::kNoArray || racer < kSkinRows) {
        MKW_GUEST_NEXT(818026FC)(ctx);
        return;
    }
    const uint32_t borrow = kSkinChosen + kSkinBorrowRow * kSkinCharacters;
    uint8_t saved[kSkinCharacters];
    for (uint32_t c = 0; c < kSkinCharacters; ++c) {
        saved[c] = Memory::Read8(borrow + c);
        Memory::Write8(borrow + c, 0);
    }
    const uint32_t archive = Memory::Read32(ArrayOverflow::Class(kArchiveClass).instanceGlobal);
    ctx->gpr[7] = archive + kArchiveModels + kSkinBorrowRow * kArchiveStride;
    MKW_GUEST_NEXT(818026FC)(ctx);
    for (uint32_t c = 0; c < kSkinCharacters; ++c) Memory::Write8(borrow + c, saved[c]);
}
REGISTER_NATIVE_WRAPPER_AS(0x818026FC, RR_KartSkinPath_818026fc, "RR_KartSkinPath_818026fc");
