#pragma once
#include <stdint.h>

namespace cracker {
constexpr uint32_t kProtocolVersion = 1;
constexpr uint32_t kMaxReadBytes = 64 * 1024;
constexpr wchar_t kDevicePath[] = L"\\\\.\\CrackerReader";

// Private device type, FILE_READ_ACCESS, METHOD_BUFFERED.
constexpr uint32_t Ioctl(uint32_t function) {
    return (0x8000u << 16) | (1u << 14) | (function << 2);
}
constexpr uint32_t kBindTarget = Ioctl(0x800);
constexpr uint32_t kReadMemory = Ioctl(0x801);
constexpr uint32_t kQueryInfo = Ioctl(0x802);
constexpr uint32_t kBackendPte = 1;

struct Header {
    uint32_t version;
    uint32_t size;
};
struct BindRequest {
    Header header;
    uint64_t process_handle; // Caller-owned handle with PROCESS_VM_READ access.
};
struct ReadRequest {
    Header header;
    uint64_t address; // Target user virtual address; translated by the PTE backend.
    uint32_t bytes;
    uint32_t reserved;
};
struct ReadReply {
    uint32_t version;
    int32_t status; // NTSTATUS of the memory operation, independent of transport.
    uint32_t bytes;
    uint32_t reserved;
    // Exactly 'bytes' initialized bytes follow this header.
};
struct DeviceInfo {
    uint32_t version;
    uint32_t backend;
    uint32_t max_read_bytes;
    uint32_t reserved;
    uint64_t process_id;
};
static_assert(sizeof(Header) == 8, "Protocol layout");
static_assert(sizeof(BindRequest) == 16, "Protocol layout");
static_assert(sizeof(ReadRequest) == 24, "Protocol layout");
static_assert(sizeof(ReadReply) == 16, "Protocol layout");
static_assert(sizeof(DeviceInfo) == 24, "Protocol layout");
} // namespace cracker
