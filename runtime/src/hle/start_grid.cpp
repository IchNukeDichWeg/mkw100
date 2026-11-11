// mkw100: start grid positions for more than 12 karts.
//
// KMP::Holder<KTPT>::CalcCoordinates (0x80514B24: holder, Vec3* pos, Vec3* rot, rank 1-based,
// player count) places a kart on the grid from two static byte tables [count - 1][12] in StaticR
// (0x8088F8E8: lateral column per rank, its first entry the field's width; +0x90: row per rank).
// Up to 12 karts the grid is 3 per row over 4 rows, rank 1 in front, columns -10 -2 6 -6 2 10
// repeating. With more karts the tables are read past their end (24 karts: racers 12, 14, 16, 18
// and 22 started thousands of units off the course, never moved and got no finish time).
//
// For more than 12 karts the 12-kart grid repeats in blocks of 12: rank r takes the 12-kart slot
// of rank (r - 1) % 12 + 1 in block (r - 1) / 12, and the game's own function computes the slot.
// Blocks sit in block rows, each one block depth behind the last (4 kart rows, twice the step
// from rank 1 to rank 7, slope included), and side by side within a block row, one block width
// apart (the 12-kart grid's 6 columns, so the column spacing carries on). Block 0 is the 12-kart
// grid; later blocks take the next placement, row by row and outwards from the middle, alternating
// sides, whose 12 slots all lie inside the course's checkpoint quads (CKPT) with a margin each
// side. Stacked straight back, 100 karts filled 9 block rows, the last 46,000 units behind the
// start and past course 0x08's start straight onto two narrow split paths; the race took 7,326
// frames, the slowest kart 2:04.7. Side by side they take 3 block rows of 4 on that straight,
// 16,000 units back at most: 5,932 frames, slowest 1:42.9 (M5 Pro, `--lockstep`).
// Checkpoint quads are flat, so a gap or level change inside one is not seen; a course whose
// checkpoints leave no room falls back to straight block rows. Up to 12 karts the game's function
// runs unchanged.

#include "abi_bridge.h"
#include "guest_function_imports.h"
#include "memory.h"
#include "boot_race.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

constexpr uint32_t kGridKarts = 12;
constexpr uint32_t kGridCalcCoordinates = 0x80514B24;
constexpr uint32_t kGridFrontRank = 1;     // column -10, row 0 of the 12-kart grid
constexpr uint32_t kGridThirdRowRank = 7;  // column -10, row 2
constexpr uint32_t kGridColumns = 6;       // columns -10 -6 -2 2 6 10
constexpr uint32_t kGridFrame = 0x20;      // guest stack bytes for one Vec3 output
// Blocks tried per block row: the middle one and up to 4 each side. Course 0x08 fits 4.
constexpr int kGridMaxSide = 4;
// Block rows tried before falling back to straight rows: 100 karts need 9 blocks.
constexpr uint32_t kGridMaxRows = 24;
// Room kept between a slot and a checkpoint quad's side, about a kart's width.
constexpr float kGridMargin = 300.0f;

// KMP (Pulsar KMP/KMPManager.hpp, KMP/CKPT.hpp)
constexpr uint32_t kKmpManager = 0x809BD6E8;  // KMP::Manager::sInstance
constexpr uint32_t kKmpCkptSection = 0x20;    // Section<CKPT>*: Holder** at +0, u16 count at +4
constexpr uint32_t kKtptRotation = 0xC;       // KTPT: Vec3 position, Vec3 rotation (degrees)
constexpr uint32_t kCkptRight = 0x8;          // CKPT: Vec2 left (x, z), Vec2 right
constexpr uint32_t kCkptNext = 0x13;          // u8, 0xFF on the last checkpoint
constexpr float kGridPi = 3.14159265358979f;

using GridVec = std::array<float, 3>;

GridVec ReadVec(uint32_t address) {
    return {Memory::ReadFloat32(address), Memory::ReadFloat32(address + 4), Memory::ReadFloat32(address + 8)};
}

float Cross(float ax, float az, float bx, float bz, float px, float pz) {
    return (bx - ax) * (pz - az) - (bz - az) * (px - ax);
}

// Whether (x, z) lies in a checkpoint quad: checkpoint i's left and right points and its next one's.
bool OnCourse(uint32_t section, float x, float z) {
    if (section == 0) {
        return false;
    }
    const uint32_t count = Memory::Read16(section + 4);
    const uint32_t holders = Memory::Read32(section);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t raw = Memory::Read32(Memory::Read32(holders + i * 4));
        uint32_t next = Memory::Read8(raw + kCkptNext);
        next = next >= count ? 0 : next;
        const uint32_t to = Memory::Read32(Memory::Read32(holders + next * 4));
        const float qx[4] = {Memory::ReadFloat32(raw), Memory::ReadFloat32(raw + kCkptRight),
                             Memory::ReadFloat32(to + kCkptRight), Memory::ReadFloat32(to)};
        const float qz[4] = {Memory::ReadFloat32(raw + 4), Memory::ReadFloat32(raw + kCkptRight + 4),
                             Memory::ReadFloat32(to + kCkptRight + 4), Memory::ReadFloat32(to + 4)};
        bool negative = false;
        bool positive = false;
        for (int e = 0; e < 4; ++e) {
            const float side = Cross(qx[e], qz[e], qx[(e + 1) % 4], qz[(e + 1) % 4], x, z);
            negative = negative || side < 0.0f;
            positive = positive || side > 0.0f;
        }
        if (!(negative && positive)) {
            return true;
        }
    }
    return false;
}

struct Grid {
    std::array<GridVec, kGridKarts> slots;  // the 12-kart grid, rank 1 first
    GridVec rowShift;                       // one block row back
    GridVec sideShift;                      // one block width across
    float sideX;                            // unit vector across the start heading
    float sideZ;
};

// Interactive grids follow the course's linked CPU driving line backwards.
// Checkpoint regions are not road boundaries: the benchmark's wide block grid
// can legitimately fit its checkpoint test while placing a kart on grass.
bool PlaceRoadGrid(uint32_t ktpt, uint32_t rank, uint32_t position, uint32_t rotation) {
    const uint32_t manager = Memory::Read32(kKmpManager);
    const uint32_t section = manager ? Memory::Read32(manager + 0x10) : 0;
    if (!section) return false;
    const uint32_t count = Memory::Read16(section + 4), holders = Memory::Read32(section);
    if (!count || count > 255) return false;
    auto holderAt = [&](uint32_t i) { return Memory::Read32(holders + 4*i); };
    auto pointAt = [&](uint32_t i) { return ReadVec(Memory::Read32(holderAt(i)+4)); };
    auto previous = [&](uint32_t i) {
        const uint32_t h=holderAt(i);
        return Memory::Read8(h+0x10) ? uint32_t(Memory::Read8(Memory::Read32(h+8))) : count;
    };
    const auto start = ReadVec(ktpt);
    float best = 1.0e30f, fraction = 0;
    uint32_t end = count;
    for (uint32_t i=0;i<count;++i) {
        const auto prev=previous(i); if (prev>=count) continue;
        const auto a=pointAt(prev), b=pointAt(i);
        const float dx=b[0]-a[0], dz=b[2]-a[2], length2=dx*dx+dz*dz;
        if (length2<1) continue;
        const float t=std::clamp(((start[0]-a[0])*dx+(start[2]-a[2])*dz)/length2,0.0f,1.0f);
        const float ex=a[0]+t*dx-start[0], ez=a[2]+t*dz-start[2];
        const float error=ex*ex+ez*ez;
        if (error<best) { best=error; fraction=t; end=i; }
    }
    if (end==count) return false;
    float remaining = 700.0f + float((rank-1)/3)*350.0f;
    for (uint32_t steps=0;steps<count;++steps) {
        const auto prev=previous(end); if (prev>=count) return false;
        const auto a=pointAt(prev), b=pointAt(end);
        const float dx=b[0]-a[0], dz=b[2]-a[2], length=std::sqrt(dx*dx+dz*dz);
        if (length>1 && remaining<=length*fraction) {
            const float t=fraction-remaining/length;
            const float lateral=(int((rank-1)%3)-1)*300.0f;
            Memory::WriteFloat32(position,a[0]+t*dx+lateral*dz/length);
            Memory::WriteFloat32(position+4,a[1]+t*(b[1]-a[1])+50.0f);
            Memory::WriteFloat32(position+8,a[2]+t*dz-lateral*dx/length);
            if (rotation) {
                Memory::WriteFloat32(rotation,0);
                Memory::WriteFloat32(rotation+4,std::atan2(dx,dz)*180.0f/kGridPi);
                Memory::WriteFloat32(rotation+8,0);
            }
            return true;
        }
        remaining-=length*fraction; end=prev; fraction=1;
    }
    return false;
}

bool BlockFits(uint32_t section, const Grid& grid, uint32_t row, int side) {
    for (const GridVec& slot : grid.slots) {
        const float x = slot[0] + grid.rowShift[0] * row + grid.sideShift[0] * side;
        const float z = slot[2] + grid.rowShift[2] * row + grid.sideShift[2] * side;
        if (!OnCourse(section, x - grid.sideX * kGridMargin, z - grid.sideZ * kGridMargin) ||
            !OnCourse(section, x + grid.sideX * kGridMargin, z + grid.sideZ * kGridMargin)) {
            return false;
        }
    }
    return true;
}

// Block `block` (1 or more): its row and side among the placements that fit, or straight rows.
void PlaceBlock(const Grid& grid, uint32_t block, uint32_t& row, int& side) {
    const uint32_t manager = Memory::Read32(kKmpManager);
    const uint32_t section = manager == 0 ? 0 : Memory::Read32(manager + kKmpCkptSection);
    uint32_t found = 0;
    for (uint32_t r = 0; r < kGridMaxRows; ++r) {
        for (int k = 0; k <= 2 * kGridMaxSide; ++k) {
            const int s = (k + 1) / 2 * (k % 2 == 1 ? 1 : -1);  // 0, 1, -1, 2, -2, ...
            if ((r == 0 && s == 0) || !BlockFits(section, grid, r, s)) {
                continue;
            }
            if (++found == block) {
                row = r;
                side = s;
                return;
            }
        }
    }
    row = block;
    side = 0;
}

} // namespace

extern "C" void KTPT_CalcCoordinates_80514b24(CpuContext* ctx) {
    const uint32_t count = ctx->gpr[7] & 0xFF;
    const uint32_t rank = ctx->gpr[6] & 0xFF;
    if (count <= kGridKarts || rank == 0) {
        MKW_GUEST_NEXT(80514B24)(ctx);
        return;
    }
    const uint32_t holder = ctx->gpr[3];
    const uint32_t position = ctx->gpr[4];
    const uint32_t rotation = ctx->gpr[5];
    const uint32_t block = (rank - 1) / kGridKarts;
    const uint32_t slot = (rank - 1) % kGridKarts;
    // The kart's own column and row within the 12-kart grid, rotation included.
    ctx->gpr[6] = slot + 1;
    ctx->gpr[7] = kGridKarts;
    const CpuContext entry = *ctx;
    MKW_GUEST_NEXT(80514B24)(ctx);
    if (!BootRace::CpuDriven() && position &&
        PlaceRoadGrid(Memory::Read32(holder), rank, position, rotation)) return;
    if (block == 0 || position == 0) {
        return;
    }
    const CpuContext after = *ctx;
    // Every slot of the 12-kart grid, written to a frame below the caller's stack.
    Grid grid{};
    *ctx = entry;
    ctx->gpr[1] -= kGridFrame;
    const uint32_t out = ctx->gpr[1] + 0x10;
    const CpuContext probe = *ctx;
    for (uint32_t i = 0; i < kGridKarts; ++i) {
        *ctx = probe;
        ctx->gpr[4] = out;
        ctx->gpr[5] = 0;
        ctx->gpr[6] = i + 1;
        InvokeIndirectCpu(kGridCalcCoordinates, ctx);
        grid.slots[i] = ReadVec(out);
    }
    *ctx = after;

    const uint32_t ktpt = Memory::Read32(holder);
    const float heading = Memory::ReadFloat32(ktpt + kKtptRotation + 4) * kGridPi / 180.0f;
    const float fx = std::sin(heading);
    const float fz = std::cos(heading);
    grid.sideX = fz;
    grid.sideZ = -fx;
    float least = 0.0f;
    float most = 0.0f;
    for (uint32_t i = 0; i < kGridKarts; ++i) {
        const float across = grid.slots[i][0] * grid.sideX + grid.slots[i][2] * grid.sideZ;
        least = i == 0 || across < least ? across : least;
        most = i == 0 || across > most ? across : most;
    }
    const float width = (most - least) * kGridColumns / (kGridColumns - 1);
    const GridVec& front = grid.slots[kGridFrontRank - 1];
    const GridVec& third = grid.slots[kGridThirdRowRank - 1];
    for (int k = 0; k < 3; ++k) {
        grid.rowShift[k] = 2.0f * (third[k] - front[k]);
    }
    grid.sideShift = {grid.sideX * width, 0.0f, grid.sideZ * width};

    uint32_t row = 0;
    int side = 0;
    PlaceBlock(grid, block, row, side);
    const GridVec& own = grid.slots[slot];
    for (uint32_t k = 0; k < 3; ++k) {
        Memory::WriteFloat32(position + k * 4,
                             own[k] + grid.rowShift[k] * static_cast<float>(row) + grid.sideShift[k] * static_cast<float>(side));
    }
}
REGISTER_NATIVE_WRAPPER_AS(0x80514B24, KTPT_CalcCoordinates_80514b24, "KTPT_CalcCoordinates_80514b24");
