#include "backend.h"
#include "../shared/protocol.h"
#include <wdmsec.h>

using namespace cracker;
using namespace cracker::kernel;

namespace {
Platform g_platform = {};
constexpr ACCESS_MASK kProcessReadAccess = 0x0010; // PROCESS_VM_READ (Win32).
UNICODE_STRING g_link = RTL_CONSTANT_STRING(L"\\DosDevices\\CrackerReader");
const GUID kDeviceClass = {0x79f01a53, 0x9bd5, 0x4672,
                           {0x99, 0x6f, 0x78, 0x9c, 0xdd, 0x7a, 0x8d, 0x60}};
struct Session {
    FAST_MUTEX mutex;
    Backend backend;
    PEPROCESS process;
    bool closed;
};

NTSTATUS Complete(PIRP irp, NTSTATUS status, ULONG_PTR bytes = 0) {
    irp->IoStatus.Status = status;
    irp->IoStatus.Information = bytes;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

bool ValidHeader(const Header& header, ULONG bytes) {
    return header.version == kProtocolVersion && header.size == bytes;
}

void Cleanup(Session* session) {
    ExAcquireFastMutex(&session->mutex);
    if (!session->closed) {
        session->closed = true;
        if (session->process) ObDereferenceObject(session->process);
        session->process = nullptr;
        ShutdownBackend(&session->backend);
    }
    ExReleaseFastMutex(&session->mutex);
}

NTSTATUS Unsupported(PDEVICE_OBJECT, PIRP irp) {
    return Complete(irp, STATUS_INVALID_DEVICE_REQUEST);
}

NTSTATUS CreateClose(PDEVICE_OBJECT, PIRP irp) {
    const auto stack = IoGetCurrentIrpStackLocation(irp);
    auto session = static_cast<Session*>(stack->FileObject->FsContext);
    if (stack->MajorFunction == IRP_MJ_CREATE) {
        if (stack->FileObject->FileName.Length) return Complete(irp, STATUS_OBJECT_NAME_INVALID);
        session = static_cast<Session*>(ExAllocatePool2(POOL_FLAG_NON_PAGED,
                                                        sizeof(Session), kPoolTag));
        if (!session) return Complete(irp, STATUS_INSUFFICIENT_RESOURCES);
        ExInitializeFastMutex(&session->mutex);
        const NTSTATUS status = InitializeBackend(&session->backend, &g_platform);
        if (!NT_SUCCESS(status)) {
            ExFreePoolWithTag(session, kPoolTag);
            return Complete(irp, status);
        }
        stack->FileObject->FsContext = session;
    } else if (session) {
        Cleanup(session);
        if (stack->MajorFunction == IRP_MJ_CLOSE) {
            stack->FileObject->FsContext = nullptr;
            ExFreePoolWithTag(session, kPoolTag);
        }
    }
    return Complete(irp, STATUS_SUCCESS);
}

NTSTATUS DeviceControl(PDEVICE_OBJECT, PIRP irp) {
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return Complete(irp, STATUS_INVALID_DEVICE_STATE);
    const auto stack = IoGetCurrentIrpStackLocation(irp);
    auto session = static_cast<Session*>(stack->FileObject->FsContext);
    if (!session) return Complete(irp, STATUS_INVALID_HANDLE);
    const ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
    const ULONG input_bytes = stack->Parameters.DeviceIoControl.InputBufferLength;
    const ULONG output_bytes = stack->Parameters.DeviceIoControl.OutputBufferLength;
    const auto buffer = static_cast<PUCHAR>(irp->AssociatedIrp.SystemBuffer);
    if (!buffer || input_bytes < sizeof(Header)) return Complete(irp, STATUS_BUFFER_TOO_SMALL);
    const Header header = *reinterpret_cast<const Header*>(buffer);
    if (!ValidHeader(header, input_bytes)) return Complete(irp, STATUS_REVISION_MISMATCH);

    if (code == kBindTarget) {
        if (input_bytes != sizeof(BindRequest)) return Complete(irp, STATUS_INVALID_PARAMETER);
        const auto request = *reinterpret_cast<const BindRequest*>(buffer);
        PEPROCESS process = nullptr;
        // A user handle is the authorization boundary. Never substitute a PID
        // lookup or KernelMode here; normal process read permissions apply.
        NTSTATUS status = ObReferenceObjectByHandle(
            reinterpret_cast<HANDLE>(request.process_handle), kProcessReadAccess,
            *PsProcessType, UserMode, reinterpret_cast<PVOID*>(&process), nullptr);
        if (!NT_SUCCESS(status)) return Complete(irp, status);
        ExAcquireFastMutex(&session->mutex);
        if (session->closed) {
            status = STATUS_FILE_CLOSED;
        } else {
            if (session->process) ObDereferenceObject(session->process);
            session->process = process;
            process = nullptr;
        }
        ExReleaseFastMutex(&session->mutex);
        if (process) ObDereferenceObject(process);
        return Complete(irp, status);
    }
    if (code == kQueryInfo) {
        if (input_bytes != sizeof(Header) || output_bytes < sizeof(DeviceInfo))
            return Complete(irp, STATUS_BUFFER_TOO_SMALL);
        DeviceInfo info = {kProtocolVersion, kBackendPte, kMaxReadBytes, 0, 0};
        ExAcquireFastMutex(&session->mutex);
        const NTSTATUS status = session->closed ? STATUS_FILE_CLOSED : STATUS_SUCCESS;
        if (session->process) info.process_id = reinterpret_cast<uint64_t>(PsGetProcessId(session->process));
        ExReleaseFastMutex(&session->mutex);
        if (!NT_SUCCESS(status)) return Complete(irp, status);
        RtlCopyMemory(buffer, &info, sizeof(info));
        return Complete(irp, STATUS_SUCCESS, sizeof(info));
    }
    if (code == kReadMemory) {
        if (input_bytes != sizeof(ReadRequest)) return Complete(irp, STATUS_INVALID_PARAMETER);
        const auto request = *reinterpret_cast<const ReadRequest*>(buffer);
        const auto highest = reinterpret_cast<ULONG64>(MmHighestUserAddress);
        if (!request.bytes || request.bytes > kMaxReadBytes || request.reserved ||
            request.address > highest || request.bytes - 1 > highest - request.address)
            return Complete(irp, STATUS_INVALID_PARAMETER);
        if (output_bytes < sizeof(ReadReply) + request.bytes)
            return Complete(irp, STATUS_BUFFER_TOO_SMALL);
        ReadResult result = {STATUS_INVALID_DEVICE_STATE, 0};
        ExAcquireFastMutex(&session->mutex);
        if (session->closed) result.status = STATUS_FILE_CLOSED;
        else if (session->process) result = ReadTarget(&session->backend, session->process,
            request.address, request.bytes, buffer + sizeof(ReadReply));
        ExReleaseFastMutex(&session->mutex);
        const ReadReply reply = {kProtocolVersion, result.status, result.bytes, 0};
        RtlCopyMemory(buffer, &reply, sizeof(reply));
        // Memory-operation failures still return their header and valid prefix.
        return Complete(irp, STATUS_SUCCESS, sizeof(reply) + result.bytes);
    }
    return Complete(irp, STATUS_INVALID_DEVICE_REQUEST);
}

void Unload(PDRIVER_OBJECT driver) {
    IoDeleteSymbolicLink(&g_link);
    IoDeleteDevice(driver->DeviceObject);
    ShutdownPlatform(&g_platform);
}
} // namespace

extern "C" DRIVER_INITIALIZE DriverEntry;
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING) {
    NTSTATUS status = InitializePlatform(&g_platform);
    if (!NT_SUCCESS(status)) {
        ShutdownPlatform(&g_platform);
        return status;
    }
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\Device\\CrackerReader");
    PDEVICE_OBJECT device = nullptr;
    status = IoCreateDeviceSecure(driver, 0, &name, 0x8000, FILE_DEVICE_SECURE_OPEN,
        FALSE, &SDDL_DEVOBJ_SYS_ALL_ADM_ALL, &kDeviceClass, &device);
    if (!NT_SUCCESS(status)) {
        ShutdownPlatform(&g_platform);
        return status;
    }
    for (ULONG i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; ++i) driver->MajorFunction[i] = Unsupported;
    driver->MajorFunction[IRP_MJ_CREATE] = CreateClose;
    driver->MajorFunction[IRP_MJ_CLEANUP] = CreateClose;
    driver->MajorFunction[IRP_MJ_CLOSE] = CreateClose;
    driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DeviceControl;
    driver->DriverUnload = Unload;
    status = IoCreateSymbolicLink(&g_link, &name);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(device);
        ShutdownPlatform(&g_platform);
        return status;
    }
    device->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}
