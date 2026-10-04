#include "../client/reader.h"
#include "../driver/page_walk.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace {
int failures = 0;
void Check(bool condition, const char* name) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}
struct Tables {
    std::map<uint64_t, uint64_t> entries;
    bool ReadEntry(uint64_t address, uint64_t& value) {
        const auto found = entries.find(address);
        if (found == entries.end()) return false;
        value = found->second;
        return true;
    }
};
Tables NormalTables() {
    // VA 0x40203123: PML4[0], PDPT[1], PD[1], PT[3], offset 0x123.
    return {{{0x1000, 0x2007}, {0x2008, 0x3007},
              {0x3008, 0x4007}, {0x4018, 0xabc007}}};
}

int SelfTest() {
    using namespace cracker::paging;
    auto tables = NormalTables();
    Translation result = {};
    Check(Translate(tables, 0x1000, 0x40203123, result) == WalkStatus::Success &&
          result.physical == 0xabc123 && result.page_bytes == 0x1000, "4 KiB translation");
    Check(Translate(tables, 0x1abc, 0x40203123, result) == WalkStatus::Success &&
          result.physical == 0xabc123, "CR3 PCID bits do not enter the table address");
    tables.entries[0x3008] = 0x02001087;
    Check(Translate(tables, 0x1000, 0x40203123, result) == WalkStatus::Success &&
          result.physical == 0x02003123 && result.page_bytes == 0x200000,
          "2 MiB page: PAT bit is not a physical address bit");
    tables.entries[0x2008] = 0x80001087;
    Check(Translate(tables, 0x1000, 0x40203123, result) == WalkStatus::Success &&
          result.physical == 0x80203123 && result.page_bytes == 0x40000000,
          "1 GiB page and offset");
    tables = NormalTables();
    tables.entries[0x4018] &= ~1ull;
    Check(Translate(tables, 0x1000, 0x40203123, result) == WalkStatus::NotPresent &&
          !result.physical && !result.page_bytes, "non-present leaf clears output");
    tables.entries.erase(0x4018);
    Check(Translate(tables, 0x1000, 0x40203123, result) == WalkStatus::ReadFailed,
          "physical read failure is distinct from a non-present page");
    tables.entries[0x4018] = 7;
    Check(Translate(tables, 0x1000, 0x40203123, result) == WalkStatus::Success &&
          result.physical == 0x123, "physical frame zero is representable");
    Check(Translate(tables, 0x1000, 0x0000800040203123ull, result) ==
          WalkStatus::UnsupportedAddress, "reject non-canonical address");
    Check(Translate(tables, 0, 0x40203123, result) == WalkStatus::NotPresent,
          "empty page-table root");
    tables = NormalTables();
    tables.entries[0x1800] = 0x2007;
    Check(Translate(tables, 0x1000, 0xffff800040203123ull, result) == WalkStatus::Success &&
          result.physical == 0xabc123, "canonical upper address translation");
    tables.entries[0x1000] |= 0x80;
    Check(Translate(tables, 0x1000, 0x40203123, result) == WalkStatus::UnsupportedAddress,
          "unsupported PML4 large-page bit");
    tables = NormalTables();
    tables.entries[0x4018] = 0x0001000000abc007ull;
    Check(Translate(tables, 0x1000, 0x40203123, result) == WalkStatus::Success &&
          result.physical == 0x0001000000abc123ull, "physical addresses retain high frame bits");

    cracker::Reader reader;
    unsigned char byte = 0;
    const auto read = reader.Read(0, &byte, 1);
    Check(read.error == ERROR_INVALID_HANDLE && read.bytes == 0,
          "SDK rejects reads before Open without contacting a driver");
    cracker::DeviceInfo info = {};
    Check(reader.Query(info) == ERROR_INVALID_HANDLE, "SDK rejects Query before Open");
    reader.Close();
    reader.Close();
    Check(true, "Close is idempotent");
    std::printf("Offline result: %d failure(s). No driver was loaded or contacted.\n", failures);
    return failures ? 1 : 0;
}

int DeviceTest() {
    constexpr size_t size = cracker::kMaxReadBytes * 2 + 4096;
    auto source = static_cast<unsigned char*>(VirtualAlloc(nullptr, size,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!source) { std::printf("VirtualAlloc error: %lu\n", GetLastError()); return 1; }
    for (size_t i = 0; i < size; ++i) source[i] = static_cast<unsigned char>((i * 37) ^ (i >> 8));
    cracker::Reader reader;
    const DWORD opened = reader.Open(GetCurrentProcessId());
    if (opened) {
        std::printf("Open error: %lu. This test needs the locally installed driver.\n", opened);
        VirtualFree(source, 0, MEM_RELEASE);
        return 1;
    }
    cracker::DeviceInfo info = {};
    Check(reader.Query(info) == ERROR_SUCCESS && info.process_id == GetCurrentProcessId(),
          "bound target and PTE backend reported by the driver");
    std::vector<unsigned char> destination(size, 0);
    auto read = reader.Read(reinterpret_cast<uint64_t>(source), destination.data(), size);
    Check(read.complete(size) && !std::memcmp(source, destination.data(), size),
          "multi-chunk read matches every byte");
    read = reader.Read(reinterpret_cast<uint64_t>(source + 4093), destination.data(), 8199);
    Check(read.complete(8199) && !std::memcmp(source + 4093, destination.data(), 8199),
          "unaligned cross-page read");
    read = reader.Read(0, nullptr, 0);
    Check(read.complete(0), "zero-length read");
    read = reader.Read(0, destination.data(), 16);
    Check(!read.complete(16) && read.bytes == 0, "unmapped address reports failure");
    DWORD previous = 0;
    if (VirtualProtect(source + cracker::kMaxReadBytes, 4096, PAGE_NOACCESS, &previous)) {
        std::fill(destination.begin(), destination.end(), static_cast<unsigned char>(0xcd));
        read = reader.Read(reinterpret_cast<uint64_t>(source), destination.data(), size);
        Check(!read.complete(size) && read.bytes == cracker::kMaxReadBytes &&
              !std::memcmp(source, destination.data(), read.bytes) &&
              destination[read.bytes] == 0xcd,
              "later chunk failure preserves the exact valid prefix");
        DWORD ignored = 0;
        Check(VirtualProtect(source + cracker::kMaxReadBytes, 4096, previous, &ignored) != FALSE,
              "restore source page protection");
    } else {
        Check(false, "prepare inaccessible-page test");
    }
    const auto start = std::chrono::steady_clock::now();
    bool matches = true;
    for (unsigned i = 0; i < 100; ++i) {
        read = reader.Read(reinterpret_cast<uint64_t>(source), destination.data(), size);
        if (!read.complete(size) || std::memcmp(source, destination.data(), size)) {
            matches = false;
            break;
        }
    }
    Check(matches, "100 repeated reads with full data comparison");
    const auto elapsed = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    std::printf("Repeated-read time (including comparison): %.3f ms\n", elapsed);
    bool reopened = true;
    for (unsigned i = 0; i < 16; ++i) {
        reader.Close();
        if (reader.Open(GetCurrentProcessId()) != ERROR_SUCCESS) { reopened = false; break; }
        read = reader.Read(reinterpret_cast<uint64_t>(source), destination.data(), 16);
        if (!read.complete(16) || std::memcmp(source, destination.data(), 16)) {
            reopened = false; break;
        }
    }
    Check(reopened, "16 close/reopen/read cycles");
    reader.Close();
    VirtualFree(source, 0, MEM_RELEASE);
    return failures ? 1 : 0;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 1 || (argc == 2 && !std::strcmp(argv[1], "--self-test"))) return SelfTest();
    if (argc == 2 && !std::strcmp(argv[1], "--device-test")) return DeviceTest();
    std::printf("CrackerVerify --self-test     Offline protocol/page-table tests (default)\n"
                "CrackerVerify --device-test   Read known data in this process via the installed driver\n");
    return argc == 2 && !std::strcmp(argv[1], "--help") ? 0 : 2;
}
