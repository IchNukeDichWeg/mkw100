#pragma once

// mkw100 guest access tracer: an analysis build that records which host code (translated guest
// functions, symbolicated afterwards) reads or writes a watched guest address range. Used to
// find every access to a struct whose layout the N-player rewrite has to change, including
// the ones a static scan misses.
//
// Off unless the build is configured with -DMKW100_ACCESS_TRACE=ON (runtime/CMakeLists.txt);
// then every guest load/store pays one unsigned range compare. Normal builds compile it away.

#include <cstdint>

namespace AccessTrace {

#if defined(MKW100_ACCESS_TRACE) && MKW100_ACCESS_TRACE
inline constexpr bool kEnabled = true;
#else
inline constexpr bool kEnabled = false;
#endif

// Watched ranges [lo, hi): range 0 is fixed by SetRange (the Racedata scenarios); the others are
// singletons found through their global pointer (kWatchedObjects in access_trace.cpp), re-read every
// kRefreshInterval accesses because those objects are created and destroyed with each scene.
inline constexpr int kMaxRanges = 8;
extern uint32_t g_lo[kMaxRanges];
extern uint32_t g_hi[kMaxRanges];
extern int g_rangeCount;
extern uint32_t g_tick;

void SetRange(uint32_t lo, uint32_t hi);
void RefreshObjects();
// Records one access inside range `range`. Not inlined, so the frame walk starts in the accessor.
[[gnu::noinline]] void Record(int range, uint32_t address, uint32_t size, bool isWrite);
// Prints one "[access-trace]" line per distinct (range, caller stack, offset, size, kind) to stdout.
void Dump();

} // namespace AccessTrace

#if defined(MKW100_ACCESS_TRACE) && MKW100_ACCESS_TRACE
#define MKW_TRACE_GUEST_ACCESS(address, size, isWrite)                                           \
    do {                                                                                         \
        const uint32_t mkwTraceAddress = (address);                                              \
        if ((++::AccessTrace::g_tick & 0xFFFu) == 0) [[unlikely]] ::AccessTrace::RefreshObjects(); \
        for (int mkwRange = 0; mkwRange < ::AccessTrace::g_rangeCount; ++mkwRange) {             \
            if (mkwTraceAddress - ::AccessTrace::g_lo[mkwRange] <                                \
                ::AccessTrace::g_hi[mkwRange] - ::AccessTrace::g_lo[mkwRange]) [[unlikely]] {    \
                ::AccessTrace::Record(mkwRange, mkwTraceAddress, (size), (isWrite));             \
                break;                                                                           \
            }                                                                                    \
        }                                                                                        \
    } while (0)
#else
#define MKW_TRACE_GUEST_ACCESS(address, size, isWrite) ((void)0)
#endif
