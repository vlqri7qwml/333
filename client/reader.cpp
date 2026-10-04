#include "reader.h"
#include <algorithm>
#include <cstring>
#include <new>
#include <vector>

namespace cracker {
DWORD Reader::Open(DWORD process_id) {
    Close();
    const HANDLE process = OpenProcess(PROCESS_VM_READ, FALSE, process_id);
    if (!process) return GetLastError();
    device_ = CreateFileW(kDevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (device_ == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        CloseHandle(process);
        return error;
    }
    const BindRequest request = {{kProtocolVersion, sizeof(BindRequest)},
                                  reinterpret_cast<uint64_t>(process)};
    DWORD returned = 0;
    const BOOL success = DeviceIoControl(device_, kBindTarget,
        const_cast<BindRequest*>(&request), sizeof(request), nullptr, 0, &returned, nullptr);
    const DWORD error = success ? ERROR_SUCCESS : GetLastError();
    CloseHandle(process);
    if (!success) Close();
    return error;
}

void Reader::Close() {
    if (device_ != INVALID_HANDLE_VALUE) CloseHandle(device_);
    device_ = INVALID_HANDLE_VALUE;
}

DWORD Reader::Query(DeviceInfo& info) const {
    info = {};
    if (device_ == INVALID_HANDLE_VALUE) return ERROR_INVALID_HANDLE;
    Header request = {kProtocolVersion, sizeof(Header)};
    DWORD returned = 0;
    if (!DeviceIoControl(device_, kQueryInfo, &request, sizeof(request), &info,
                         sizeof(info), &returned, nullptr)) return GetLastError();
    if (returned != sizeof(info) || info.version != kProtocolVersion ||
        info.backend != kBackendPte || info.max_read_bytes != kMaxReadBytes)
        return ERROR_REVISION_MISMATCH;
    return ERROR_SUCCESS;
}

ReadResult Reader::Read(uint64_t address, void* destination, size_t bytes) const {
    ReadResult result = {ERROR_SUCCESS, 0, 0};
    if (device_ == INVALID_HANDLE_VALUE) return {ERROR_INVALID_HANDLE, 0, 0};
    if (!bytes) return result;
    if (!destination || bytes - 1 > UINT64_MAX - address)
        return {ERROR_INVALID_PARAMETER, 0, 0};
    std::vector<unsigned char> response;
    try {
        response.resize(sizeof(ReadReply) + (std::min)(bytes, size_t{kMaxReadBytes}));
    } catch (const std::bad_alloc&) {
        return {ERROR_NOT_ENOUGH_MEMORY, 0, 0};
    }
    while (result.bytes < bytes) {
        const auto count = static_cast<uint32_t>((std::min)(bytes - result.bytes,
                                                           size_t{kMaxReadBytes}));
        ReadRequest request = {{kProtocolVersion, sizeof(ReadRequest)},
                                address + result.bytes, count, 0};
        DWORD returned = 0;
        if (!DeviceIoControl(device_, kReadMemory, &request, sizeof(request),
                response.data(), static_cast<DWORD>(sizeof(ReadReply) + count),
                &returned, nullptr)) {
            result.error = GetLastError();
            break;
        }
        ReadReply reply = {};
        if (returned < sizeof(reply)) {
            result.error = ERROR_INVALID_DATA;
            break;
        }
        std::memcpy(&reply, response.data(), sizeof(reply));
        if (reply.version != kProtocolVersion || reply.reserved || reply.bytes > count ||
            returned != sizeof(reply) + reply.bytes || (reply.status == 0 && reply.bytes != count)) {
            result.error = ERROR_INVALID_DATA;
            break;
        }
        std::memcpy(static_cast<unsigned char*>(destination) + result.bytes,
                    response.data() + sizeof(reply), reply.bytes);
        result.bytes += reply.bytes;
        result.status = reply.status;
        if (reply.status != 0) break;
    }
    return result;
}
} // namespace cracker
