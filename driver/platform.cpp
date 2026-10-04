#include "backend.h"
#include "page_walk.h"

namespace cracker::kernel {
NTSTATUS InitializePlatform(Platform* platform) {
    *platform = {};
    // This backend supports four-level x64 paging.
    if (__readcr4() & (1ull << 12)) return STATUS_NOT_SUPPORTED;
    const ULONG64 root = __readcr3() & paging::kFrameMask;
    auto table = static_cast<ULONG64*>(ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                                     PAGE_SIZE, kPoolTag));
    if (!table) return STATUS_INSUFFICIENT_RESOURCES;
    MM_COPY_ADDRESS source = {};
    source.PhysicalAddress.QuadPart = static_cast<LONGLONG>(root);
    SIZE_T copied = 0;
    NTSTATUS status = MmCopyMemory(table, source, PAGE_SIZE,
                                   MM_COPY_MEMORY_PHYSICAL, &copied);
    if (NT_SUCCESS(status) && copied == PAGE_SIZE) {
        status = STATUS_NOT_SUPPORTED;
        // Locate the kernel page-table self-reference used to address PTEs.
        for (ULONG slot = 256; slot < 512; ++slot) {
            if ((table[slot] & 1) && (table[slot] & paging::kFrameMask) == root) {
                platform->pte_base = 0xffff000000000000ull |
                                     (static_cast<ULONG64>(slot) << 39);
                status = STATUS_SUCCESS;
                break;
            }
        }
    } else if (NT_SUCCESS(status)) {
        status = STATUS_PARTIAL_COPY;
    }
    ExFreePoolWithTag(table, kPoolTag);
    if (!NT_SUCCESS(status)) return status;
    platform->ranges = MmGetPhysicalMemoryRanges();
    if (!platform->ranges) return STATUS_INSUFFICIENT_RESOURCES;
    return STATUS_SUCCESS;
}

void ShutdownPlatform(Platform* platform) {
    if (platform->ranges) ExFreePool(platform->ranges);
    *platform = {};
}

NTSTATUS InitializeBackend(Backend* backend, const Platform* platform) {
    *backend = {};
    PHYSICAL_ADDRESS low = {}, high = {}, boundary = {};
    high.QuadPart = MAXLONGLONG;
    backend->window = static_cast<PUCHAR>(MmAllocateContiguousMemorySpecifyCache(
        PAGE_SIZE, low, high, boundary, MmCached));
    if (!backend->window) return STATUS_INSUFFICIENT_RESOURCES;
    backend->platform = platform;
    backend->pte = reinterpret_cast<volatile LONG64*>(platform->pte_base +
        ((reinterpret_cast<ULONG64>(backend->window) >> 9) & 0x7ffffffff8ull));
    NTSTATUS status = STATUS_SUCCESS;
    __try {
        backend->original_pte = static_cast<ULONG64>(*backend->pte);
        const auto physical = MmGetPhysicalAddress(backend->window);
        if (!(backend->original_pte & 1) ||
            (backend->original_pte & paging::kFrameMask) !=
            static_cast<ULONG64>(physical.QuadPart)) status = STATUS_NOT_SUPPORTED;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        status = STATUS_NOT_SUPPORTED;
    }
    if (!NT_SUCCESS(status)) ShutdownBackend(backend);
    return status;
}

void ShutdownBackend(Backend* backend) {
    // Every physical read restores the PTE before returning.
    if (backend->window) MmFreeContiguousMemory(backend->window);
    *backend = {};
}
} // namespace cracker::kernel
