#pragma once
#include <stdint.h>

namespace cracker::paging {
constexpr uint64_t kPageBytes = 0x1000;
constexpr uint64_t kFrameMask = 0x000ffffffffff000ull;
enum class WalkStatus { Success, NotPresent, ReadFailed, UnsupportedAddress };
struct Translation {
    uint64_t physical;
    uint64_t page_bytes;
};

// Four-level x64 translation. The reader is supplied by the PTE backend in
// kernel mode and a synthetic page table in offline tests.
// This module has no allocation, imports, process state or Windows dependency.
template <typename Reader>
WalkStatus Translate(Reader& reader, uint64_t cr3, uint64_t address,
                     Translation& result) {
    result = {};
    const uint64_t high = address >> 47;
    if (high != 0 && high != 0x1ffff) return WalkStatus::UnsupportedAddress;
    uint64_t table = cr3 & kFrameMask;
    if (!table) return WalkStatus::NotPresent;
    const unsigned shifts[] = {39, 30, 21, 12};
    for (unsigned level = 0; level < 4; ++level) {
        uint64_t entry = 0;
        if (!reader.ReadEntry(table + ((address >> shifts[level]) & 0x1ff) * 8,
                              entry)) return WalkStatus::ReadFailed;
        if (!(entry & 1)) return WalkStatus::NotPresent;
        if ((level == 1 || level == 2) && (entry & 0x80)) {
            const uint64_t page_bytes = 1ull << shifts[level];
            result = {(entry & kFrameMask & ~(page_bytes - 1)) |
                      (address & (page_bytes - 1)), page_bytes};
            return WalkStatus::Success;
        }
        if (level == 0 && (entry & 0x80)) return WalkStatus::UnsupportedAddress;
        table = entry & kFrameMask;
    }
    result = {table | (address & (kPageBytes - 1)), kPageBytes};
    return WalkStatus::Success;
}
} // namespace cracker::paging
