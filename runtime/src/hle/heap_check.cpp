// mkw100 `--heap-check` (runtime/include/boot_race.h): finds the subsystem whose work corrupted a
// heap. The race scene sets a heap group id before creating each subsystem
// (ExpHeapGroup::SetHeapsGroupId, RaceScene::createSubsystems), so walking every ExpHeap's used
// list at each switch names the group whose allocations were written past their end.

#include "boot_race.h"

#include "abi_bridge.h"
#include "guest_abi/flat_memory.h"
#include "guest_function_imports.h"
#include "memory.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr uint32_t kExpHeapSignature = 0x45585048;  // 'EXPH', MEMiHeapHead
constexpr uint16_t kUsedSignature = 0x5544;         // 'UD', MEMiExpHeapMBlockHead
constexpr uint32_t kUsedListHead = 0x44;            // MEMVisitAllocatedForExpHeap
constexpr uint32_t kHeapStart = 0x18;
constexpr uint32_t kHeapEnd = 0x1C;
constexpr struct {
    uint32_t base;
    uint32_t size;
} kRanges[] = {{0x80000000, 0x01800000}, {0x90000000, 0x04000000}};

uint32_t Read32(uint32_t address) {
    const uint8_t* p = MKW_FLAT_GUEST_BASE + address;
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

bool InRam(uint32_t address) {
    for (const auto& range : kRanges) {
        if (address >= range.base && address - range.base < range.size - 0x10) {
            return true;
        }
    }
    return false;
}

// First broken used block of the heap at `heap`, or 0. `previous` gets the last good block.
uint32_t BrokenBlock(uint32_t heap, uint32_t& previous) {
    const uint32_t start = Read32(heap + kHeapStart);
    const uint32_t end = Read32(heap + kHeapEnd);
    previous = 0;
    uint32_t block = Read32(heap + kUsedListHead);
    for (int guard = 0; block != 0 && guard < 1000000; ++guard) {
        if (block < start || block >= end || !InRam(block) || (Read32(block) >> 16) != kUsedSignature) {
            return block;
        }
        previous = block;
        block = Read32(block + 0xC);
    }
    return 0;
}

bool g_reported = false;
uint32_t g_group = 0xFFFFFFFF;

void CheckHeaps() {
    static const uint8_t signature[4] = {0x45, 0x58, 0x50, 0x48};
    for (const auto& range : kRanges) {
        const uint8_t* base = MKW_FLAT_GUEST_BASE + range.base;
        for (uint32_t offset = 0; offset + 0x50 < range.size; offset += 4) {
            if (std::memcmp(base + offset, signature, 4) != 0) {
                continue;
            }
            const uint32_t heap = range.base + offset;
            const uint32_t start = Read32(heap + kHeapStart);
            const uint32_t end = Read32(heap + kHeapEnd);
            if (!InRam(start) || end <= start || end - start > range.size) {
                continue;  // the bytes merely look like a signature
            }
            uint32_t previous = 0;
            if (const uint32_t broken = BrokenBlock(heap, previous)) {
                std::fprintf(stderr,
                             "[heap-check] heap %08x [%08x..%08x): used block %08x is broken after %08x (size "
                             "%#x) during heap group %u\n",
                             heap, start, end, broken, previous, previous ? Read32(previous + 4) : 0, g_group);
                // The overrun's bytes: the end of the last good block's payload and the header after it.
                if (InRam(broken - 0x20)) {
                    std::fprintf(stderr, "[heap-check] %08x:", broken - 0x20);
                    for (uint32_t word = 0; word < 0x40; word += 4) {
                        std::fprintf(stderr, " %08x", Read32(broken - 0x20 + word));
                    }
                    std::fprintf(stderr, "\n");
                }
                // Who holds the overrun block: words pointing at its payload, with the vtable-like
                // word 0..0x40 bytes before each (names the owner object).
                // The broken block too: a negative index into it overwrites its header.
                for (const uint32_t payload : {previous != 0 ? previous + 0x10 : 0, broken + 0x10}) {
                    if (payload == 0) {
                        continue;
                    }
                    for (const auto& scan : kRanges) {
                        for (uint32_t at = scan.base; at + 4 < scan.base + scan.size; at += 4) {
                            if (Read32(at) != payload) {
                                continue;
                            }
                            // The used block holding the pointer (same heap), and its first words.
                            uint32_t holder = 0;
                            uint32_t block = Read32(heap + kUsedListHead);
                            for (int guard = 0; block != 0 && block != broken && guard < 1000000; ++guard) {
                                if (at >= block + 0x10 && at < block + 0x10 + Read32(block + 4)) {
                                    holder = block + 0x10;
                                    break;
                                }
                                block = Read32(block + 0xC);
                            }
                            std::fprintf(stderr, "[heap-check] %08x points at payload %08x", at, payload);
                            if (holder != 0) {
                                std::fprintf(stderr, " (holder %08x+%#x, size %#x, words %08x %08x)", holder,
                                             at - holder, Read32(holder - 0xC), Read32(holder), Read32(holder + 4));
                            }
                            std::fprintf(stderr, "\n");
                        }
                    }
                }
                g_reported = true;
            }
        }
    }
}

} // namespace

// ExpHeapGroup::SetHeapsGroupId(scene, group): the work of the previous group just ended.
extern "C" void ExpHeapGroup_SetHeapsGroupId_80009bc0(CpuContext* ctx) {
    if (BootRace::HeapCheck() && !g_reported) {
        CheckHeaps();
        g_group = ctx->gpr[4];
    }
    MKW_GUEST_NEXT(80009BC0)(ctx);
}
REGISTER_NATIVE_WRAPPER_AS(0x80009BC0, ExpHeapGroup_SetHeapsGroupId_80009bc0, "ExpHeapGroup_SetHeapsGroupId_80009bc0");
