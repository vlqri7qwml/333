#pragma once
#include <ntifs.h>
#include <intrin.h>

namespace cracker::kernel {
constexpr ULONG kPoolTag = 'rRcC';
struct Platform {
    ULONG64 pte_base;
    PPHYSICAL_MEMORY_RANGE ranges;
};
struct Backend {
    PUCHAR window;
    volatile LONG64* pte;
    ULONG64 original_pte;
    const Platform* platform;
};
struct ReadResult {
    NTSTATUS status;
    ULONG bytes;
};

NTSTATUS InitializePlatform(Platform* platform);
void ShutdownPlatform(Platform* platform);
NTSTATUS InitializeBackend(Backend* backend, const Platform* platform);
void ShutdownBackend(Backend* backend);
// Caller serializes this backend and holds a reference to process.
ReadResult ReadTarget(Backend* backend, PEPROCESS process,
                      ULONG64 address, ULONG bytes, PUCHAR destination);
} // namespace cracker::kernel
