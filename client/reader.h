#pragma once
#include <Windows.h>
#include <stddef.h>
#include "../shared/protocol.h"

namespace cracker {
struct ReadResult {
    DWORD error;
    int32_t status;
    size_t bytes;
    bool complete(size_t requested) const {
        return error == ERROR_SUCCESS && status == 0 && bytes == requested;
    }
};

// Reads may run concurrently. Open/Close require external synchronization.
class Reader {
public:
    Reader() = default;
    ~Reader() { Close(); }
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    DWORD Open(DWORD process_id);
    void Close();
    DWORD Query(DeviceInfo& info) const;
    ReadResult Read(uint64_t address, void* destination, size_t bytes) const;
private:
    HANDLE device_ = INVALID_HANDLE_VALUE;
};
} // namespace cracker
