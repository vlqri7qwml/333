#include "backend.h"
#include "page_walk.h"

namespace cracker::kernel {
static bool IsRam(const Platform* platform, ULONG64 address, SIZE_T bytes) {
    for (auto range = platform->ranges;
         range->BaseAddress.QuadPart || range->NumberOfBytes.QuadPart; ++range) {
        const auto start = static_cast<ULONG64>(range->BaseAddress.QuadPart);
        const auto length = static_cast<ULONG64>(range->NumberOfBytes.QuadPart);
        if (address >= start && address - start < length &&
            bytes <= length - (address - start)) return true;
    }
    return false;
}

static bool ReadPhysical(Backend* backend, ULONG64 address, PVOID output, SIZE_T bytes) {
    const auto offset = static_cast<SIZE_T>(address & (PAGE_SIZE - 1));
    if (bytes > PAGE_SIZE - offset || !IsRam(backend->platform, address, bytes))
        return false;
    bool success = false;
    // A private window per session, serialized by the caller. Keep each raised
    // IRQL interval to one page; each use and restore invalidates the local TLB.
    const KIRQL previous = KeRaiseIrqlToDpcLevel();
    __try {
        __try {
            const ULONG64 entry = (address & paging::kFrameMask) |
                                  0x8000000000000003ull; // WB, supervisor, NX.
            InterlockedExchange64(backend->pte, static_cast<LONG64>(entry));
            __invlpg(backend->window);
            RtlCopyMemory(output, backend->window + offset, bytes);
            success = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            success = false;
        }
    } __finally {
        InterlockedExchange64(backend->pte, static_cast<LONG64>(backend->original_pte));
        __invlpg(backend->window);
        KeLowerIrql(previous);
    }
    return success;
}

struct EntryReader {
    Backend* backend;
    bool ReadEntry(uint64_t address, uint64_t& value) {
        return ReadPhysical(backend, address, &value, sizeof(value));
    }
};

ReadResult ReadTarget(Backend* backend, PEPROCESS process,
                      ULONG64 address, ULONG bytes, PUCHAR destination) {
    ReadResult result = {STATUS_SUCCESS, 0};
    if (PsGetProcessExitStatus(process) != STATUS_PENDING)
        return {STATUS_PROCESS_IS_TERMINATING, 0};
    auto mdl = IoAllocateMdl(reinterpret_cast<PVOID>(address), bytes, FALSE, FALSE, nullptr);
    if (!mdl) return {STATUS_INSUFFICIENT_RESOURCES, 0};
    KAPC_STATE attach = {};
    bool attached = false, locked = false;
    __try {
        __try {
            KeStackAttachProcess(process, &attach);
            attached = true;
            // Pin the authorized user range so physical pages cannot be reused
            // while reading.
            MmProbeAndLockPages(mdl, UserMode, IoReadAccess);
            locked = true;
            const ULONG64 cr3 = __readcr3();
            const auto pfns = MmGetMdlPfnArray(mdl);
            EntryReader reader = {backend};
            while (result.bytes < bytes) {
                const ULONG64 current = address + result.bytes;
                paging::Translation translated = {};
                if (paging::Translate(reader, cr3, current, translated) !=
                    paging::WalkStatus::Success) {
                    result.status = STATUS_PARTIAL_COPY;
                    break;
                }
                const ULONG index = static_cast<ULONG>(
                    ((address & (PAGE_SIZE - 1)) + result.bytes) >> PAGE_SHIFT);
                const ULONG64 expected = (static_cast<ULONG64>(pfns[index]) << PAGE_SHIFT) |
                                         (current & (PAGE_SIZE - 1));
                // A live VA may be remapped after pinning; don't read a new PFN.
                if (translated.physical != expected) {
                    result.status = STATUS_RETRY;
                    break;
                }
                const ULONG page_left = PAGE_SIZE - static_cast<ULONG>(current & (PAGE_SIZE - 1));
                const ULONG remaining = bytes - result.bytes;
                const ULONG count = remaining < page_left ? remaining : page_left;
                if (!ReadPhysical(backend, translated.physical,
                                  destination + result.bytes, count)) {
                    result.status = STATUS_PARTIAL_COPY;
                    break;
                }
                result.bytes += count;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            result.status = GetExceptionCode();
        }
    } __finally {
        if (locked) MmUnlockPages(mdl);
        if (attached) KeUnstackDetachProcess(&attach);
        IoFreeMdl(mdl);
    }
    return result;
}
} // namespace cracker::kernel
