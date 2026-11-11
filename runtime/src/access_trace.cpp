#include "guest_abi/access_trace.h"

#include "guest_abi/flat_memory.h"

#include <array>
#include <cstdio>
#include <map>
#include <mutex>
#include <tuple>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace AccessTrace {

uint32_t g_lo[kMaxRanges] = {};
uint32_t g_hi[kMaxRanges] = {};
int g_rangeCount = 1;  // range 0 (SetRange) is empty until set
uint32_t g_tick = 0;

namespace {

struct WatchedObject {
    const char* name;
    uint32_t global;  // guest address of the singleton's instance pointer
    int32_t member;   // >= 0: the object is the pointer at instance + member instead
    uint32_t size;
};

// mkw100 analysis targets: singletons (and objects they own) whose per-player arrays the N-player
// rewrite relocates. Sizes are the allocations in AI::Manager's ctor (0x80738ED4).
constexpr WatchedObject kWatchedObjects[] = {
    {"ai_manager", 0x809C2BE8, -1, 0x9C},    // AI::Manager: AI* [12] at 0x24 and 0x54
    {"ai_rank", 0x809C2BE8, 0x84, 0x194},    // AI::Manager::mpRankManager (Enemy::AIRankManager)
    {"ai_sub88", 0x809C2BE8, 0x88, 0x38},
    {"ai_sub90", 0x809C2BE8, 0x90, 0x34},
    {"ai_sub94", 0x809C2BE8, 0x94, 0xD0},
    {"driver_manager", 0x809C2F38, -1, 0x1F0},  // Race::DriverManager: Driver* [12] at 0x10 and more
};
static_assert(1 + std::size(kWatchedObjects) <= kMaxRanges);
const char* RangeName(int range) { return range == 0 ? "racedata" : kWatchedObjects[range - 1].name; }

// Return addresses of the three innermost callers. On the checked memory path (16 KB host pages)
// the first one is inside Memory::Read*/Write*, so the report walks out to the translated function.
constexpr int kFrames = 3;

struct Key {
    int range;
    std::array<uintptr_t, kFrames> pcs;
    uint32_t offset;
    uint32_t size;
    bool isWrite;
    bool operator<(const Key& other) const {
        return std::tie(range, pcs, offset, size, isWrite) <
               std::tie(other.range, other.pcs, other.offset, other.size, other.isWrite);
    }
};

std::mutex g_mutex;
std::map<Key, uint64_t> g_hits;

uintptr_t ImageSlide() {
#if defined(__APPLE__)
    return static_cast<uintptr_t>(_dyld_get_image_vmaddr_slide(0));
#else
    return 0;
#endif
}

} // namespace

void SetRange(uint32_t lo, uint32_t hi) {
    if (!kEnabled) {
        return;
    }
    g_lo[0] = lo;
    g_hi[0] = hi;
    std::printf("[access-trace] watching %08x..%08x\n", lo, hi);
    RefreshObjects();
}

void RefreshObjects() {
    if (!kEnabled) {
        return;
    }
    // Plain host reads of guest globals: Memory::Read32 would re-enter the traced access path.
    const auto read32 = [](uint32_t address) {
        const uint8_t* p = MKW_FLAT_GUEST_BASE + address;
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    };
    for (size_t i = 0; i < std::size(kWatchedObjects); ++i) {
        uint32_t instance = read32(kWatchedObjects[i].global);
        if (instance != 0 && kWatchedObjects[i].member >= 0) {
            instance = read32(instance + kWatchedObjects[i].member);
        }
        g_lo[i + 1] = instance;
        g_hi[i + 1] = instance ? instance + kWatchedObjects[i].size : instance;
    }
    g_rangeCount = static_cast<int>(1 + std::size(kWatchedObjects));
}

void Record(int range, uint32_t address, uint32_t size, bool isWrite) {
    // Walks the arm64 frame records ({previous fp, lr}); Apple's ABI keeps frame pointers.
    // Unslid, so `atos -o WiiCompiled <pc>` names the function without a load address.
    std::array<uintptr_t, kFrames> pcs{};
    auto* frame = static_cast<uintptr_t*>(__builtin_frame_address(0));
    for (int i = 0; i < kFrames && frame; ++i) {
        pcs[i] = frame[1] ? frame[1] - ImageSlide() : 0;
        auto* previous = reinterpret_cast<uintptr_t*>(frame[0]);
        if (previous <= frame) break;
        frame = previous;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_hits[{range, pcs, address - g_lo[range], size, isWrite}];
}

void Dump() {
    if (!kEnabled) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    std::printf("[access-trace] %zu distinct accesses\n", g_hits.size());
    for (const auto& [key, count] : g_hits) {
        std::printf("[access-trace] range %s pc %llx,%llx,%llx offset %x size %u %s count %llu\n",
                    RangeName(key.range), static_cast<unsigned long long>(key.pcs[0]), static_cast<unsigned long long>(key.pcs[1]),
                    static_cast<unsigned long long>(key.pcs[2]), key.offset, key.size, key.isWrite ? "W" : "R",
                    static_cast<unsigned long long>(count));
    }
    std::fflush(stdout);
}

} // namespace AccessTrace
