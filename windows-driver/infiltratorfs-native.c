// SPDX-License-Identifier: GPL-3.0-or-later
#include <ntifs.h>
#include <ntdddisk.h>

#include "infiltratorfs-native-protocol.h"

#define INFILFS_NATIVE_TAG 'nSfI'
#define INFILFS_NATIVE_FCB_TAG 'fSfI'
#define INFILFS_NATIVE_CCB_TAG 'cSfI'
#define INFILFS_NATIVE_REQUEST_TAG 'rSfI'
#define INFILFS_NATIVE_BLOCK_SIZE 4096u
#define INFILFS_NATIVE_FORMAT_MAJOR 0u
#define INFILFS_NATIVE_FORMAT_MINOR 18u

typedef struct _INFILFS_NATIVE_SUPERBLOCK_PREFIX {
    UCHAR Magic[8];
    USHORT FormatMajor;
    USHORT FormatMinor;
    USHORT HeaderSize;
    USHORT BlockShift;
    ULONG ChecksumType;
    ULONGLONG Generation;
    ULONGLONG TotalBlocks;
    ULONGLONG FreeBlocks;
    ULONGLONG AllocationRootBlock;
    ULONGLONG AllocationLeafCount;
    ULONGLONG ObjectIndexBlock;
    ULONGLONG RootObjectBlock;
    ULONGLONG CheckpointBlock[3];
} INFILFS_NATIVE_SUPERBLOCK_PREFIX;

typedef struct _INFILFS_NATIVE_VOLUME {
    ULONG Signature;
    PDEVICE_OBJECT DeviceObject;
    PDEVICE_OBJECT TargetDevice;
    PVPB Vpb;
    ULONGLONG VolumeId;
    ULONGLONG SizeBytes;
    ULONG SectorSize;
    BOOLEAN ReadOnly;
    BOOLEAN Locked;
    BOOLEAN Dismounted;
    ERESOURCE Resource;
    FAST_MUTEX FcbLock;
    LIST_ENTRY Fcbs;
    LIST_ENTRY GlobalLink;
} INFILFS_NATIVE_VOLUME;

typedef struct _INFILFS_NATIVE_FCB {
    ULONG Signature;
    struct _INFILFS_NATIVE_VOLUME *Volume;
    LIST_ENTRY VolumeLink;
    FSRTL_ADVANCED_FCB_HEADER Header;
    FAST_MUTEX HeaderMutex;
    SECTION_OBJECT_POINTERS SectionObjectPointers;
    ERESOURCE MainResource;
    ERESOURCE PagingResource;
    SHARE_ACCESS ShareAccess;
    FILE_LOCK FileLock;
    OPLOCK Oplock;
    volatile LONG References;
    ULONGLONG FileId;
    ULONG ObjectType;
    ULONG FileAttributes;
    ULONG LinkCount;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER AccessTime;
    LARGE_INTEGER WriteTime;
    LARGE_INTEGER ChangeTime;
    UNICODE_STRING Path;
    volatile LONG DeletePendingCount;
} INFILFS_NATIVE_FCB;

typedef struct _INFILFS_NATIVE_CCB {
    ULONG Signature;
    ULONG DirectoryIndex;
    BOOLEAN ShareRegistered;
    BOOLEAN DeletePending;
} INFILFS_NATIVE_CCB;

typedef struct _INFILFS_NATIVE_REQUEST_ITEM {
    LIST_ENTRY Link;
    KEVENT CompletionEvent;
    struct infilfs_win_native_request *Request;
    struct infilfs_win_native_response *Response;
    BOOLEAN Active;
    BOOLEAN Completed;
} INFILFS_NATIVE_REQUEST_ITEM;

typedef struct _INFILFS_NATIVE_GLOBAL {
    PDRIVER_OBJECT DriverObject;
    PDEVICE_OBJECT FileSystemDevice;
    PDEVICE_OBJECT ControlDevice;
    UNICODE_STRING ControlDosName;
    FAST_MUTEX QueueLock;
    LIST_ENTRY PendingRequests;
    LIST_ENTRY ActiveRequests;
    KSEMAPHORE PendingSemaphore;
    volatile LONG64 NextRequestId;
    FAST_MUTEX VolumeLock;
    LIST_ENTRY Volumes;
    volatile LONG64 NextVolumeId;
    volatile LONG Unloading;
} INFILFS_NATIVE_GLOBAL;

static INFILFS_NATIVE_GLOBAL g_Infilfs;

static const CACHE_MANAGER_CALLBACKS g_InfilfsCacheCallbacks;

static NTSTATUS InfilfsCompleteIrp(PIRP Irp, NTSTATUS Status, ULONG_PTR Information)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static NTSTATUS InfilfsStatusFromPortable(LONG Status)
{
    switch (Status) {
    case 0: return STATUS_SUCCESS;
    case -2: return STATUS_INVALID_PARAMETER;
    case -3: return STATUS_IO_DEVICE_ERROR;
    case -4: return STATUS_FILE_CORRUPT_ERROR;
    case -5: return STATUS_MEDIA_WRITE_PROTECTED;
    case -6: return STATUS_DISK_FULL;
    case -7: return STATUS_OBJECT_NAME_NOT_FOUND;
    case -8: return STATUS_OBJECT_NAME_COLLISION;
    case -9: return STATUS_NOT_A_DIRECTORY;
    case -10: return STATUS_FILE_IS_A_DIRECTORY;
    case -11: return STATUS_DIRECTORY_NOT_EMPTY;
    case -12: return STATUS_NAME_TOO_LONG;
    case -13: return STATUS_FILE_TOO_LARGE;
    case -14: return STATUS_INTEGER_OVERFLOW;
    case -15: return STATUS_REPARSE_POINT_NOT_RESOLVED;
    case -16: return STATUS_NOT_SUPPORTED;
    case -17: return STATUS_INSUFFICIENT_RESOURCES;
    case -18: return STATUS_CANCELLED;
    case -19: return STATUS_DEVICE_BUSY;
    default: return STATUS_UNSUCCESSFUL;
    }
}

static BOOLEAN InfilfsIsControlDevice(PDEVICE_OBJECT DeviceObject)
{
    return DeviceObject == g_Infilfs.ControlDevice;
}

static BOOLEAN InfilfsIsFileSystemDevice(PDEVICE_OBJECT DeviceObject)
{
    return DeviceObject == g_Infilfs.FileSystemDevice;
}

static INFILFS_NATIVE_VOLUME *InfilfsVolumeFromDevice(PDEVICE_OBJECT DeviceObject)
{
    INFILFS_NATIVE_VOLUME *Volume;
    if (!DeviceObject || InfilfsIsControlDevice(DeviceObject) ||
        InfilfsIsFileSystemDevice(DeviceObject))
        return NULL;
    Volume = (INFILFS_NATIVE_VOLUME *)DeviceObject->DeviceExtension;
    if (!Volume || Volume->Signature != 'vSfI')
        return NULL;
    return Volume;
}

static NTSTATUS InfilfsTargetIo(
    PDEVICE_OBJECT Target, UCHAR Major, ULONGLONG Offset,
    PVOID Buffer, ULONG Length)
{
    KEVENT Event;
    IO_STATUS_BLOCK Iosb;
    LARGE_INTEGER Position;
    PIRP Irp;
    NTSTATUS Status;

    if (!Target || (!Buffer && Length))
        return STATUS_INVALID_PARAMETER;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Position.QuadPart = (LONGLONG)Offset;
    Irp = IoBuildSynchronousFsdRequest(
        Major, Target, Buffer, Length, &Position, &Event, &Iosb);
    if (!Irp)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = IoCallDriver(Target, Irp);
    if (Status == STATUS_PENDING) {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = Iosb.Status;
    }
    if (NT_SUCCESS(Status) && Iosb.Information != Length)
        Status = STATUS_END_OF_FILE;
    return Status;
}

static NTSTATUS InfilfsTargetFlush(PDEVICE_OBJECT Target)
{
    KEVENT Event;
    IO_STATUS_BLOCK Iosb;
    PIRP Irp;
    NTSTATUS Status;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp = IoBuildSynchronousFsdRequest(
        IRP_MJ_FLUSH_BUFFERS, Target, NULL, 0, NULL, &Event, &Iosb);
    if (!Irp)
        return STATUS_INSUFFICIENT_RESOURCES;
    Status = IoCallDriver(Target, Irp);
    if (Status == STATUS_PENDING) {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = Iosb.Status;
    }
    return Status;
}

static INFILFS_NATIVE_VOLUME *InfilfsFindVolume(ULONGLONG VolumeId)
{
    PLIST_ENTRY Entry;
    INFILFS_NATIVE_VOLUME *Found = NULL;

    ExAcquireFastMutex(&g_Infilfs.VolumeLock);
    for (Entry = g_Infilfs.Volumes.Flink;
         Entry != &g_Infilfs.Volumes; Entry = Entry->Flink) {
        INFILFS_NATIVE_VOLUME *Volume =
            CONTAINING_RECORD(Entry, INFILFS_NATIVE_VOLUME, GlobalLink);
        if (Volume->VolumeId == VolumeId && !Volume->Dismounted) {
            Found = Volume;
            break;
        }
    }
    ExReleaseFastMutex(&g_Infilfs.VolumeLock);
    return Found;
}

static NTSTATUS InfilfsCallService(
    INFILFS_NATIVE_VOLUME *Volume,
    struct infilfs_win_native_request *Request,
    struct infilfs_win_native_response *Response)
{
    INFILFS_NATIVE_REQUEST_ITEM *Item;
    LARGE_INTEGER Timeout;
    LARGE_INTEGER ZeroTimeout;
    BOOLEAN ReclaimSemaphore = FALSE;
    BOOLEAN Completed = FALSE;
    NTSTATUS Status;

    if (!Volume || !Request || !Response || g_Infilfs.Unloading)
        return STATUS_DEVICE_NOT_READY;

    Item = ExAllocatePool2(
        POOL_FLAG_PAGED, sizeof(*Item), INFILFS_NATIVE_REQUEST_TAG);
    if (!Item)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(Item, sizeof(*Item));
    InitializeListHead(&Item->Link);
    KeInitializeEvent(&Item->CompletionEvent, NotificationEvent, FALSE);
    Item->Request = Request;
    Item->Response = Response;

    Request->protocol_version = INFILFS_WIN_NATIVE_PROTOCOL_VERSION;
    Request->volume_id = Volume->VolumeId;
    Request->request_id =
        (ULONGLONG)InterlockedIncrement64(&g_Infilfs.NextRequestId);
    Response->protocol_version = INFILFS_WIN_NATIVE_PROTOCOL_VERSION;
    Response->request_id = Request->request_id;

    ExAcquireFastMutex(&g_Infilfs.QueueLock);
    InsertTailList(&g_Infilfs.PendingRequests, &Item->Link);
    ExReleaseFastMutex(&g_Infilfs.QueueLock);
    KeReleaseSemaphore(&g_Infilfs.PendingSemaphore, IO_NO_INCREMENT, 1, FALSE);

    Timeout.QuadPart = -(LONGLONG)30 * 10 * 1000 * 1000;
    Status = KeWaitForSingleObject(
        &Item->CompletionEvent, Executive, KernelMode, FALSE, &Timeout);

    /*
     * QueueLock owns both list membership and the completion transition. The
     * completion path signals the event before dropping this lock, so once we
     * remove the item here no other thread can retain a live kernel pointer to
     * it. This closes the timeout/completion use-after-free race.
     */
    ExAcquireFastMutex(&g_Infilfs.QueueLock);
    if (!IsListEmpty(&Item->Link)) {
        ReclaimSemaphore = Item->Active ? FALSE : TRUE;
        RemoveEntryList(&Item->Link);
    }
    InitializeListHead(&Item->Link);
    Completed = Item->Completed;
    ExReleaseFastMutex(&g_Infilfs.QueueLock);

    /*
     * If a request timed out before a worker dequeued it, consume the matching
     * semaphore credit when it is still present. A worker that already consumed
     * the credit simply wins this zero-time race and observes an empty queue.
     */
    if (ReclaimSemaphore) {
        ZeroTimeout.QuadPart = 0;
        (void)KeWaitForSingleObject(
            &g_Infilfs.PendingSemaphore, Executive, KernelMode,
            FALSE, &ZeroTimeout);
    }

    if (Status == STATUS_TIMEOUT)
        Status = STATUS_IO_TIMEOUT;
    else if (NT_SUCCESS(Status) && !Completed)
        Status = STATUS_DEVICE_NOT_READY;
    else if (NT_SUCCESS(Status))
        Status = InfilfsStatusFromPortable(Response->status);

    ExFreePoolWithTag(Item, INFILFS_NATIVE_REQUEST_TAG);
    return Status;
}

static NTSTATUS InfilfsQueryPortableVolumeState(
    INFILFS_NATIVE_VOLUME *Volume,
    struct infilfs_win_native_volume_state *State)
{
    struct infilfs_win_native_request *Request = NULL;
    struct infilfs_win_native_response *Response = NULL;
    NTSTATUS Status;

    if (!Volume || !State)
        return STATUS_INVALID_PARAMETER;

    Request = ExAllocatePool2(
        POOL_FLAG_PAGED, sizeof(*Request), INFILFS_NATIVE_REQUEST_TAG);
    Response = ExAllocatePool2(
        POOL_FLAG_PAGED, sizeof(*Response), INFILFS_NATIVE_REQUEST_TAG);
    if (!Request || !Response) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto out;
    }

    RtlZeroMemory(Request, sizeof(*Request));
    RtlZeroMemory(Response, sizeof(*Response));
    Request->opcode = INFILFS_WIN_NATIVE_OP_QUERY_VOLUME;
    Status = InfilfsCallService(Volume, Request, Response);
    if (!NT_SUCCESS(Status))
        goto out;
    if (Response->output_bytes != sizeof(*State)) {
        Status = STATUS_DATA_ERROR;
        goto out;
    }
    RtlCopyMemory(State, Response->output, sizeof(*State));
    if (!State->total_blocks ||
        State->free_blocks > State->total_blocks ||
        State->label_bytes > sizeof(State->label))
        Status = STATUS_FILE_CORRUPT_ERROR;

out:
    if (Response)
        ExFreePoolWithTag(Response, INFILFS_NATIVE_REQUEST_TAG);
    if (Request)
        ExFreePoolWithTag(Request, INFILFS_NATIVE_REQUEST_TAG);
    return Status;
}

static ULONG InfilfsVolumeSerial(
    const struct infilfs_win_native_volume_state *State)
{
    ULONG Serial = 2166136261u;
    ULONG Index;

    for (Index = 0; Index < sizeof(State->filesystem_uuid); ++Index)
        Serial = (Serial ^ State->filesystem_uuid[Index]) * 16777619u;
    return Serial ? Serial : 1u;
}

static NTSTATUS InfilfsControlWaitRequest(PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
    INFILFS_NATIVE_REQUEST_ITEM *Item = NULL;
    PLIST_ENTRY Entry;
    LARGE_INTEGER Timeout;
    NTSTATUS Status;

    if (IrpSp->Parameters.DeviceIoControl.OutputBufferLength <
        sizeof(struct infilfs_win_native_request))
        return InfilfsCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
    if (g_Infilfs.Unloading)
        return InfilfsCompleteIrp(Irp, STATUS_DEVICE_NOT_READY, 0);

    /*
     * Keep the private service wait bounded. User-mode CancelIoEx cannot cancel
     * a synchronous kernel semaphore wait after dispatch has entered it, so an
     * infinite wait makes service stop/restart depend on unrelated filesystem
     * traffic. One second keeps idle overhead negligible while bounding stop.
     */
    Timeout.QuadPart = -(LONGLONG)1 * 10 * 1000 * 1000;
    Status = KeWaitForSingleObject(
        &g_Infilfs.PendingSemaphore, Executive, KernelMode, FALSE, &Timeout);
    if (Status == STATUS_TIMEOUT)
        return InfilfsCompleteIrp(Irp, STATUS_DEVICE_NOT_READY, 0);
    if (!NT_SUCCESS(Status))
        return InfilfsCompleteIrp(Irp, Status, 0);

    ExAcquireFastMutex(&g_Infilfs.QueueLock);
    if (!IsListEmpty(&g_Infilfs.PendingRequests)) {
        Entry = RemoveHeadList(&g_Infilfs.PendingRequests);
        Item = CONTAINING_RECORD(
            Entry, INFILFS_NATIVE_REQUEST_ITEM, Link);
        Item->Active = TRUE;
        InsertTailList(&g_Infilfs.ActiveRequests, &Item->Link);
    }
    ExReleaseFastMutex(&g_Infilfs.QueueLock);

    if (!Item)
        return InfilfsCompleteIrp(Irp, STATUS_DEVICE_NOT_READY, 0);

    RtlCopyMemory(
        Irp->AssociatedIrp.SystemBuffer, Item->Request,
        sizeof(*Item->Request));
    return InfilfsCompleteIrp(
        Irp, STATUS_SUCCESS, sizeof(*Item->Request));
}

static NTSTATUS InfilfsControlCompleteRequest(PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
    struct infilfs_win_native_response *Input;
    PLIST_ENTRY Entry;
    INFILFS_NATIVE_REQUEST_ITEM *Item = NULL;

    if (IrpSp->Parameters.DeviceIoControl.InputBufferLength <
        sizeof(struct infilfs_win_native_response))
        return InfilfsCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
    Input = (struct infilfs_win_native_response *)
        Irp->AssociatedIrp.SystemBuffer;
    if (!Input ||
        Input->protocol_version != INFILFS_WIN_NATIVE_PROTOCOL_VERSION)
        return InfilfsCompleteIrp(Irp, STATUS_REVISION_MISMATCH, 0);

    ExAcquireFastMutex(&g_Infilfs.QueueLock);
    for (Entry = g_Infilfs.ActiveRequests.Flink;
         Entry != &g_Infilfs.ActiveRequests; Entry = Entry->Flink) {
        INFILFS_NATIVE_REQUEST_ITEM *Candidate =
            CONTAINING_RECORD(Entry, INFILFS_NATIVE_REQUEST_ITEM, Link);
        if (Candidate->Request->request_id == Input->request_id) {
            Item = Candidate;
            RtlCopyMemory(Item->Response, Input, sizeof(*Input));
            Item->Completed = TRUE;
            /*
             * Signal while QueueLock still protects Item lifetime. A timeout
             * cannot unlink/free this request until after KeSetEvent returns.
             */
            KeSetEvent(&Item->CompletionEvent, IO_NO_INCREMENT, FALSE);
            break;
        }
    }
    ExReleaseFastMutex(&g_Infilfs.QueueLock);

    if (!Item)
        return InfilfsCompleteIrp(Irp, STATUS_NOT_FOUND, 0);
    return InfilfsCompleteIrp(Irp, STATUS_SUCCESS, 0);
}

static NTSTATUS InfilfsControlRawIo(
    PIRP Irp, PIO_STACK_LOCATION IrpSp, BOOLEAN Write)
{
    struct infilfs_win_native_raw_io *Io;
    INFILFS_NATIVE_VOLUME *Volume;
    ULONG Required;
    NTSTATUS Status;

    if (IrpSp->Parameters.DeviceIoControl.InputBufferLength <
        FIELD_OFFSET(struct infilfs_win_native_raw_io, data))
        return InfilfsCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
    Io = (struct infilfs_win_native_raw_io *)Irp->AssociatedIrp.SystemBuffer;
    if (!Io ||
        Io->protocol_version != INFILFS_WIN_NATIVE_PROTOCOL_VERSION ||
        Io->size > INFILFS_WIN_NATIVE_IO_CHUNK)
        return InfilfsCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);

    Required = FIELD_OFFSET(struct infilfs_win_native_raw_io, data) + Io->size;
    if (IrpSp->Parameters.DeviceIoControl.InputBufferLength < Required ||
        (!Write &&
         IrpSp->Parameters.DeviceIoControl.OutputBufferLength < Required))
        return InfilfsCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);

    Volume = InfilfsFindVolume(Io->volume_id);
    if (!Volume)
        return InfilfsCompleteIrp(Irp, STATUS_VOLUME_DISMOUNTED, 0);
    if (Io->offset > Volume->SizeBytes ||
        Io->size > Volume->SizeBytes - Io->offset)
        return InfilfsCompleteIrp(Irp, STATUS_END_OF_FILE, 0);
    if (Write && Volume->ReadOnly)
        return InfilfsCompleteIrp(Irp, STATUS_MEDIA_WRITE_PROTECTED, 0);

    Status = InfilfsTargetIo(
        Volume->TargetDevice, Write ? IRP_MJ_WRITE : IRP_MJ_READ,
        Io->offset, Io->data, Io->size);
    return InfilfsCompleteIrp(
        Irp, Status, NT_SUCCESS(Status) ? Required : 0);
}

static NTSTATUS InfilfsControlDeviceIo(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    ULONG Code = IrpSp->Parameters.DeviceIoControl.IoControlCode;
    UNREFERENCED_PARAMETER(DeviceObject);

    switch (Code) {
    case IOCTL_INFILFS_NATIVE_WAIT_REQUEST:
        return InfilfsControlWaitRequest(Irp, IrpSp);
    case IOCTL_INFILFS_NATIVE_COMPLETE_REQUEST:
        return InfilfsControlCompleteRequest(Irp, IrpSp);
    case IOCTL_INFILFS_NATIVE_RAW_READ:
        return InfilfsControlRawIo(Irp, IrpSp, FALSE);
    case IOCTL_INFILFS_NATIVE_RAW_WRITE:
        return InfilfsControlRawIo(Irp, IrpSp, TRUE);
    case IOCTL_INFILFS_NATIVE_RAW_FLUSH: {
        struct infilfs_win_native_volume_info *Info =
            (struct infilfs_win_native_volume_info *)
            Irp->AssociatedIrp.SystemBuffer;
        INFILFS_NATIVE_VOLUME *Volume;
        if (IrpSp->Parameters.DeviceIoControl.InputBufferLength <
            sizeof(*Info))
            return InfilfsCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        Volume = InfilfsFindVolume(Info->volume_id);
        if (!Volume)
            return InfilfsCompleteIrp(Irp, STATUS_VOLUME_DISMOUNTED, 0);
        return InfilfsCompleteIrp(
            Irp, InfilfsTargetFlush(Volume->TargetDevice), 0);
    }
    case IOCTL_INFILFS_NATIVE_VOLUME_QUERY: {
        struct infilfs_win_native_volume_info *Info =
            (struct infilfs_win_native_volume_info *)
            Irp->AssociatedIrp.SystemBuffer;
        INFILFS_NATIVE_VOLUME *Volume;
        ULONGLONG Id;
        if (IrpSp->Parameters.DeviceIoControl.InputBufferLength <
                sizeof(ULONGLONG) ||
            IrpSp->Parameters.DeviceIoControl.OutputBufferLength <
                sizeof(*Info))
            return InfilfsCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        Id = *(ULONGLONG *)Irp->AssociatedIrp.SystemBuffer;
        Volume = InfilfsFindVolume(Id);
        if (!Volume)
            return InfilfsCompleteIrp(Irp, STATUS_VOLUME_DISMOUNTED, 0);
        RtlZeroMemory(Info, sizeof(*Info));
        Info->protocol_version = INFILFS_WIN_NATIVE_PROTOCOL_VERSION;
        Info->volume_id = Volume->VolumeId;
        Info->size_bytes = Volume->SizeBytes;
        Info->sector_size = Volume->SectorSize;
        Info->read_only = Volume->ReadOnly ? 1u : 0u;
        return InfilfsCompleteIrp(Irp, STATUS_SUCCESS, sizeof(*Info));
    }
    default:
        return InfilfsCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

static BOOLEAN InfilfsTargetReadOnly(PDEVICE_OBJECT Target)
{
    KEVENT Event;
    IO_STATUS_BLOCK Iosb;
    PIRP Irp;
    NTSTATUS Status;

    if (!Target)
        return TRUE;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp = IoBuildDeviceIoControlRequest(
        IOCTL_DISK_IS_WRITABLE, Target, NULL, 0, NULL, 0,
        FALSE, &Event, &Iosb);
    if (!Irp)
        return TRUE;

    Status = IoCallDriver(Target, Irp);
    if (Status == STATUS_PENDING) {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = Iosb.Status;
    }

    /*
     * Only a positive writable result permits mutation. A lower device that
     * cannot establish writability is treated read-only rather than allowing
     * the portable service to begin transactions that can never be committed.
     */
    return !NT_SUCCESS(Status);
}

static BOOLEAN InfilfsRecognizeVolume(
    PDEVICE_OBJECT Target, ULONGLONG *SizeBytes, ULONG *SectorSize)
{
    DISK_GEOMETRY Geometry;
    KEVENT Event;
    IO_STATUS_BLOCK Iosb;
    PIRP Irp;
    NTSTATUS Status;
    UCHAR *Block;
    INFILFS_NATIVE_SUPERBLOCK_PREFIX *Sb;
    GET_LENGTH_INFORMATION Length;

    RtlZeroMemory(&Geometry, sizeof(Geometry));
    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    Irp = IoBuildDeviceIoControlRequest(
        IOCTL_DISK_GET_DRIVE_GEOMETRY, Target, NULL, 0,
        &Geometry, sizeof(Geometry), FALSE, &Event, &Iosb);
    if (!Irp)
        return FALSE;
    Status = IoCallDriver(Target, Irp);
    if (Status == STATUS_PENDING) {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = Iosb.Status;
    }
    if (!NT_SUCCESS(Status))
        return FALSE;

    KeInitializeEvent(&Event, NotificationEvent, FALSE);
    RtlZeroMemory(&Length, sizeof(Length));
    Irp = IoBuildDeviceIoControlRequest(
        IOCTL_DISK_GET_LENGTH_INFO, Target, NULL, 0,
        &Length, sizeof(Length), FALSE, &Event, &Iosb);
    if (!Irp)
        return FALSE;
    Status = IoCallDriver(Target, Irp);
    if (Status == STATUS_PENDING) {
        KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
        Status = Iosb.Status;
    }
    if (!NT_SUCCESS(Status) || Length.Length.QuadPart < INFILFS_NATIVE_BLOCK_SIZE)
        return FALSE;

    Block = ExAllocatePool2(
        POOL_FLAG_NON_PAGED, INFILFS_NATIVE_BLOCK_SIZE, INFILFS_NATIVE_TAG);
    if (!Block)
        return FALSE;
    Status = InfilfsTargetIo(
        Target, IRP_MJ_READ, 0, Block, INFILFS_NATIVE_BLOCK_SIZE);
    if (!NT_SUCCESS(Status)) {
        ExFreePoolWithTag(Block, INFILFS_NATIVE_TAG);
        return FALSE;
    }

    Sb = (INFILFS_NATIVE_SUPERBLOCK_PREFIX *)Block;
    if (RtlCompareMemory(Sb->Magic, "INFS2026", 8) != 8 ||
        Sb->FormatMajor != INFILFS_NATIVE_FORMAT_MAJOR ||
        Sb->FormatMinor != INFILFS_NATIVE_FORMAT_MINOR ||
        Sb->BlockShift != 12 ||
        Sb->HeaderSize < sizeof(*Sb) ||
        Sb->TotalBlocks == 0 ||
        Sb->TotalBlocks >
            ((ULONGLONG)Length.Length.QuadPart / INFILFS_NATIVE_BLOCK_SIZE)) {
        ExFreePoolWithTag(Block, INFILFS_NATIVE_TAG);
        return FALSE;
    }

    *SizeBytes = (ULONGLONG)Length.Length.QuadPart;
    *SectorSize = Geometry.BytesPerSector ?
        Geometry.BytesPerSector : 512u;
    ExFreePoolWithTag(Block, INFILFS_NATIVE_TAG);
    return TRUE;
}

static NTSTATUS InfilfsMountVolume(
    PDEVICE_OBJECT FileSystemDevice, PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
    PVPB Vpb = IrpSp->Parameters.MountVolume.Vpb;
    PDEVICE_OBJECT Target;
    PDEVICE_OBJECT VolumeDevice = NULL;
    INFILFS_NATIVE_VOLUME *Volume;
    ULONGLONG SizeBytes = 0;
    ULONG SectorSize = 0;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(FileSystemDevice);
    if (!Vpb || !Vpb->RealDevice)
        return InfilfsCompleteIrp(Irp, STATUS_UNRECOGNIZED_VOLUME, 0);

    Target = IoGetAttachedDeviceReference(Vpb->RealDevice);
    if (!InfilfsRecognizeVolume(Target, &SizeBytes, &SectorSize)) {
        ObDereferenceObject(Target);
        return InfilfsCompleteIrp(Irp, STATUS_UNRECOGNIZED_VOLUME, 0);
    }

    Status = IoCreateDevice(
        g_Infilfs.DriverObject, sizeof(INFILFS_NATIVE_VOLUME), NULL,
        FILE_DEVICE_DISK_FILE_SYSTEM, 0, FALSE, &VolumeDevice);
    if (!NT_SUCCESS(Status)) {
        ObDereferenceObject(Target);
        return InfilfsCompleteIrp(Irp, Status, 0);
    }

    Volume = (INFILFS_NATIVE_VOLUME *)VolumeDevice->DeviceExtension;
    RtlZeroMemory(Volume, sizeof(*Volume));
    Volume->Signature = 'vSfI';
    Volume->DeviceObject = VolumeDevice;
    Volume->TargetDevice = Target;
    Volume->Vpb = Vpb;
    Volume->VolumeId =
        (ULONGLONG)InterlockedIncrement64(&g_Infilfs.NextVolumeId);
    Volume->SizeBytes = SizeBytes;
    Volume->SectorSize = SectorSize;
    Volume->ReadOnly = InfilfsTargetReadOnly(Target);
    ExInitializeResourceLite(&Volume->Resource);
    ExInitializeFastMutex(&Volume->FcbLock);
    InitializeListHead(&Volume->Fcbs);
    InitializeListHead(&Volume->GlobalLink);

    VolumeDevice->Flags |= DO_DIRECT_IO;
    VolumeDevice->Flags &= ~DO_DEVICE_INITIALIZING;
    Vpb->DeviceObject = VolumeDevice;
    Vpb->Flags |= VPB_MOUNTED;

    ExAcquireFastMutex(&g_Infilfs.VolumeLock);
    InsertTailList(&g_Infilfs.Volumes, &Volume->GlobalLink);
    ExReleaseFastMutex(&g_Infilfs.VolumeLock);

    return InfilfsCompleteIrp(Irp, STATUS_SUCCESS, 0);
}

static NTSTATUS InfilfsUserFsctl(
    PDEVICE_OBJECT DeviceObject, PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
    INFILFS_NATIVE_VOLUME *Volume = InfilfsVolumeFromDevice(DeviceObject);
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    INFILFS_NATIVE_FCB *Fcb = FileObject ?
        (INFILFS_NATIVE_FCB *)FileObject->FsContext : NULL;
    ULONG Code = IrpSp->Parameters.FileSystemControl.FsControlCode;

    switch (Code) {
    case FSCTL_REQUEST_OPLOCK_LEVEL_1:
    case FSCTL_REQUEST_OPLOCK_LEVEL_2:
    case FSCTL_REQUEST_BATCH_OPLOCK:
    case FSCTL_REQUEST_FILTER_OPLOCK:
    case FSCTL_OPLOCK_BREAK_ACKNOWLEDGE:
    case FSCTL_OPBATCH_ACK_CLOSE_PENDING:
    case FSCTL_OPLOCK_BREAK_NOTIFY:
    case FSCTL_OPLOCK_BREAK_ACK_NO_2:
    case FSCTL_REQUEST_OPLOCK:
        if (!Fcb || Fcb->ObjectType != INFILFS_WIN_NATIVE_OBJECT_FILE)
            return InfilfsCompleteIrp(
                Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
        /*
         * FsRtlOplockFsctrl owns completion/pending semantics for oplock
         * requests. The shared FCB keeps one oplock state across all opens.
         */
        return FsRtlOplockFsctrl(
            &Fcb->Oplock, Irp,
            (ULONG)(Fcb->References > 0 ? Fcb->References : 0));

    case FSCTL_LOCK_VOLUME:
        if (!Volume)
            return InfilfsCompleteIrp(
                Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
        ExAcquireFastMutex(&Volume->FcbLock);
        if (!IsListEmpty(&Volume->Fcbs)) {
            ExReleaseFastMutex(&Volume->FcbLock);
            return InfilfsCompleteIrp(
                Irp, STATUS_ACCESS_DENIED, 0);
        }
        Volume->Locked = TRUE;
        ExReleaseFastMutex(&Volume->FcbLock);
        return InfilfsCompleteIrp(Irp, STATUS_SUCCESS, 0);

    case FSCTL_UNLOCK_VOLUME:
        if (!Volume)
            return InfilfsCompleteIrp(
                Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
        Volume->Locked = FALSE;
        return InfilfsCompleteIrp(Irp, STATUS_SUCCESS, 0);

    case FSCTL_DISMOUNT_VOLUME:
        if (!Volume)
            return InfilfsCompleteIrp(
                Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
        if (!Volume->Locked)
            return InfilfsCompleteIrp(
                Irp, STATUS_ACCESS_DENIED, 0);
        Volume->Dismounted = TRUE;
        if (Volume->Vpb) {
            Volume->Vpb->Flags &= ~VPB_MOUNTED;
            Volume->Vpb->DeviceObject = NULL;
        }
        return InfilfsCompleteIrp(Irp, STATUS_SUCCESS, 0);

    default:
        return InfilfsCompleteIrp(
            Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

static NTSTATUS InfilfsFileSystemControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);

    if (InfilfsIsFileSystemDevice(DeviceObject) &&
        IrpSp->MinorFunction == IRP_MN_MOUNT_VOLUME)
        return InfilfsMountVolume(DeviceObject, Irp, IrpSp);

    if (IrpSp->MinorFunction == IRP_MN_VERIFY_VOLUME) {
        INFILFS_NATIVE_VOLUME *Volume =
            InfilfsVolumeFromDevice(DeviceObject);
        if (!Volume || Volume->Dismounted)
            return InfilfsCompleteIrp(
                Irp, STATUS_WRONG_VOLUME, 0);
        return InfilfsCompleteIrp(Irp, STATUS_SUCCESS, 0);
    }

    if (IrpSp->MinorFunction == IRP_MN_USER_FS_REQUEST)
        return InfilfsUserFsctl(DeviceObject, Irp, IrpSp);

    return InfilfsCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

static VOID InfilfsFreeFcb(INFILFS_NATIVE_FCB *Fcb)
{
    if (!Fcb)
        return;
    FsRtlUninitializeFileLock(&Fcb->FileLock);
    FsRtlUninitializeOplock(&Fcb->Oplock);
    if (Fcb->Path.Buffer)
        ExFreePoolWithTag(Fcb->Path.Buffer, INFILFS_NATIVE_FCB_TAG);
    ExDeleteResourceLite(&Fcb->PagingResource);
    ExDeleteResourceLite(&Fcb->MainResource);
    ExFreePoolWithTag(Fcb, INFILFS_NATIVE_FCB_TAG);
}

static INFILFS_NATIVE_FCB *InfilfsAllocateFcb(
    INFILFS_NATIVE_VOLUME *Volume, PCUNICODE_STRING Path,
    const struct infilfs_win_native_attributes *Attributes)
{
    INFILFS_NATIVE_FCB *Fcb;
    USHORT Bytes;

    if (!Path || !Attributes || Path->Length > Path->MaximumLength)
        return NULL;
    Fcb = ExAllocatePool2(
        POOL_FLAG_NON_PAGED, sizeof(*Fcb), INFILFS_NATIVE_FCB_TAG);
    if (!Fcb)
        return NULL;
    RtlZeroMemory(Fcb, sizeof(*Fcb));
    Fcb->Signature = 'fSfI';
    Fcb->Volume = Volume;
    InitializeListHead(&Fcb->VolumeLink);
    Fcb->References = 1;
    Fcb->FileId = Attributes->file_id;
    Fcb->ObjectType = Attributes->object_type;
    Fcb->FileAttributes = Attributes->file_attributes;
    Fcb->LinkCount = Attributes->link_count;
    Fcb->CreationTime.QuadPart = (LONGLONG)Attributes->creation_time_100ns;
    Fcb->AccessTime.QuadPart = (LONGLONG)Attributes->access_time_100ns;
    Fcb->WriteTime.QuadPart = (LONGLONG)Attributes->write_time_100ns;
    Fcb->ChangeTime.QuadPart = (LONGLONG)Attributes->change_time_100ns;
    ExInitializeFastMutex(&Fcb->HeaderMutex);
    if (!NT_SUCCESS(ExInitializeResourceLite(&Fcb->MainResource))) {
        ExFreePoolWithTag(Fcb, INFILFS_NATIVE_FCB_TAG);
        return NULL;
    }
    if (!NT_SUCCESS(ExInitializeResourceLite(&Fcb->PagingResource))) {
        ExDeleteResourceLite(&Fcb->MainResource);
        ExFreePoolWithTag(Fcb, INFILFS_NATIVE_FCB_TAG);
        return NULL;
    }
    FsRtlInitializeFileLock(&Fcb->FileLock, NULL, NULL);
    FsRtlInitializeOplock(&Fcb->Oplock);
    FsRtlSetupAdvancedHeader(&Fcb->Header, &Fcb->HeaderMutex);
    Fcb->Header.Resource = &Fcb->MainResource;
    Fcb->Header.PagingIoResource = &Fcb->PagingResource;
    Fcb->Header.FileSize.QuadPart = (LONGLONG)Attributes->logical_size;
    Fcb->Header.ValidDataLength = Fcb->Header.FileSize;
    Fcb->Header.AllocationSize.QuadPart =
        (LONGLONG)Attributes->allocation_size;
    Fcb->Header.IsFastIoPossible = FastIoIsNotPossible;
    Fcb->SectionObjectPointers.SharedCacheMap = NULL;
    Fcb->SectionObjectPointers.DataSectionObject = NULL;
    Fcb->SectionObjectPointers.ImageSectionObject = NULL;

    Bytes = Path->Length + sizeof(WCHAR);
    Fcb->Path.Buffer = ExAllocatePool2(
        POOL_FLAG_NON_PAGED, Bytes, INFILFS_NATIVE_FCB_TAG);
    if (!Fcb->Path.Buffer) {
        InfilfsFreeFcb(Fcb);
        return NULL;
    }
    Fcb->Path.Length = Path->Length;
    Fcb->Path.MaximumLength = Bytes;
    RtlCopyMemory(Fcb->Path.Buffer, Path->Buffer, Path->Length);
    Fcb->Path.Buffer[Path->Length / sizeof(WCHAR)] = L'\0';
    return Fcb;
}

static INFILFS_NATIVE_FCB *InfilfsGetOrCreateFcb(
    INFILFS_NATIVE_VOLUME *Volume, PCUNICODE_STRING Path,
    const struct infilfs_win_native_attributes *Attributes)
{
    PLIST_ENTRY Entry;
    INFILFS_NATIVE_FCB *Fcb = NULL;

    ExAcquireFastMutex(&Volume->FcbLock);
    for (Entry = Volume->Fcbs.Flink;
         Entry != &Volume->Fcbs; Entry = Entry->Flink) {
        INFILFS_NATIVE_FCB *Candidate =
            CONTAINING_RECORD(Entry, INFILFS_NATIVE_FCB, VolumeLink);
        if (Candidate->FileId == Attributes->file_id) {
            InterlockedIncrement(&Candidate->References);
            Fcb = Candidate;
            break;
        }
    }
    if (!Fcb) {
        Fcb = InfilfsAllocateFcb(Volume, Path, Attributes);
        if (Fcb)
            InsertTailList(&Volume->Fcbs, &Fcb->VolumeLink);
    }
    ExReleaseFastMutex(&Volume->FcbLock);
    return Fcb;
}

static VOID InfilfsDereferenceFcb(INFILFS_NATIVE_FCB *Fcb)
{
    INFILFS_NATIVE_VOLUME *Volume;
    if (!Fcb)
        return;
    Volume = Fcb->Volume;
    if (!Volume)
        return;

    ExAcquireFastMutex(&Volume->FcbLock);
    LONG References = InterlockedDecrement(&Fcb->References);
    if (References == 0) {
        RemoveEntryList(&Fcb->VolumeLink);
        InitializeListHead(&Fcb->VolumeLink);
    }
    ExReleaseFastMutex(&Volume->FcbLock);
    if (References == 0)
        InfilfsFreeFcb(Fcb);
}

static VOID InfilfsCopyPathToRequest(
    struct infilfs_win_native_request *Request, PCUNICODE_STRING Path)
{
    ULONG Chars;
    if (!Path || !Path->Buffer) {
        Request->path_chars = 0;
        return;
    }
    Chars = Path->Length / sizeof(WCHAR);
    if (Chars >= INFILFS_WIN_NATIVE_PATH_CHARS)
        Chars = INFILFS_WIN_NATIVE_PATH_CHARS - 1u;
    Request->path_chars = Chars;
    RtlCopyMemory(Request->path, Path->Buffer, Chars * sizeof(WCHAR));
    Request->path[Chars] = 0;
}

static NTSTATUS InfilfsLookupPath(
    INFILFS_NATIVE_VOLUME *Volume, PCUNICODE_STRING Path,
    struct infilfs_win_native_attributes *Attributes)
{
    struct infilfs_win_native_request *Request;
    struct infilfs_win_native_response *Response;
    NTSTATUS Status;

    Request = ExAllocatePool2(
        POOL_FLAG_PAGED, sizeof(*Request), INFILFS_NATIVE_REQUEST_TAG);
    Response = ExAllocatePool2(
        POOL_FLAG_PAGED, sizeof(*Response), INFILFS_NATIVE_REQUEST_TAG);
    if (!Request || !Response) {
        if (Request) ExFreePoolWithTag(Request, INFILFS_NATIVE_REQUEST_TAG);
        if (Response) ExFreePoolWithTag(Response, INFILFS_NATIVE_REQUEST_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(Request, sizeof(*Request));
    RtlZeroMemory(Response, sizeof(*Response));
    Request->opcode = INFILFS_WIN_NATIVE_OP_LOOKUP;
    InfilfsCopyPathToRequest(Request, Path);
    Status = InfilfsCallService(Volume, Request, Response);
    if (NT_SUCCESS(Status))
        *Attributes = Response->attributes;
    ExFreePoolWithTag(Response, INFILFS_NATIVE_REQUEST_TAG);
    ExFreePoolWithTag(Request, INFILFS_NATIVE_REQUEST_TAG);
    return Status;
}

static NTSTATUS InfilfsServiceMutation(
    INFILFS_NATIVE_VOLUME *Volume, ULONG Opcode,
    PCUNICODE_STRING Path, PCUNICODE_STRING SecondPath,
    ULONGLONG Length, ULONG Flags)
{
    struct infilfs_win_native_request *Request;
    struct infilfs_win_native_response *Response;
    NTSTATUS Status;

    Request = ExAllocatePool2(
        POOL_FLAG_PAGED, sizeof(*Request), INFILFS_NATIVE_REQUEST_TAG);
    Response = ExAllocatePool2(
        POOL_FLAG_PAGED, sizeof(*Response), INFILFS_NATIVE_REQUEST_TAG);
    if (!Request || !Response) {
        if (Request)
            ExFreePoolWithTag(Request, INFILFS_NATIVE_REQUEST_TAG);
        if (Response)
            ExFreePoolWithTag(Response, INFILFS_NATIVE_REQUEST_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(Request, sizeof(*Request));
    RtlZeroMemory(Response, sizeof(*Response));
    Request->opcode = Opcode;
    Request->length = Length;
    Request->flags = Flags;
    InfilfsCopyPathToRequest(Request, Path);
    if (SecondPath && SecondPath->Buffer) {
        ULONG Chars = SecondPath->Length / sizeof(WCHAR);
        if (Chars >= INFILFS_WIN_NATIVE_PATH_CHARS)
            Chars = INFILFS_WIN_NATIVE_PATH_CHARS - 1u;
        Request->second_path_chars = Chars;
        RtlCopyMemory(
            Request->second_path, SecondPath->Buffer,
            Chars * sizeof(WCHAR));
        Request->second_path[Chars] = 0;
    }

    Status = InfilfsCallService(Volume, Request, Response);
    ExFreePoolWithTag(Response, INFILFS_NATIVE_REQUEST_TAG);
    ExFreePoolWithTag(Request, INFILFS_NATIVE_REQUEST_TAG);
    return Status;
}

static NTSTATUS InfilfsFetchSecurityDescriptor(
    INFILFS_NATIVE_VOLUME *Volume, PCUNICODE_STRING Path,
    PSECURITY_DESCRIPTOR *DescriptorOut, PULONG LengthOut)
{
    struct infilfs_win_native_request *Request = NULL;
    struct infilfs_win_native_response *Response = NULL;
    PSECURITY_DESCRIPTOR Descriptor = NULL;
    NTSTATUS Status;

    if (!Volume || !Path || !DescriptorOut || !LengthOut)
        return STATUS_INVALID_PARAMETER;
    *DescriptorOut = NULL;
    *LengthOut = 0;

    Request = ExAllocatePool2(
        POOL_FLAG_PAGED, sizeof(*Request), INFILFS_NATIVE_REQUEST_TAG);
    Response = ExAllocatePool2(
        POOL_FLAG_PAGED, sizeof(*Response), INFILFS_NATIVE_REQUEST_TAG);
    if (!Request || !Response) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto out;
    }

    RtlZeroMemory(Request, sizeof(*Request));
    RtlZeroMemory(Response, sizeof(*Response));
    Request->opcode = INFILFS_WIN_NATIVE_OP_QUERY_SECURITY;
    Request->desired_access =
        OWNER_SECURITY_INFORMATION |
        GROUP_SECURITY_INFORMATION |
        DACL_SECURITY_INFORMATION;
    InfilfsCopyPathToRequest(Request, Path);
    Status = InfilfsCallService(Volume, Request, Response);
    if (!NT_SUCCESS(Status))
        goto out;
    if (!Response->output_bytes ||
        Response->output_bytes > sizeof(Response->output)) {
        Status = STATUS_INVALID_SECURITY_DESCR;
        goto out;
    }

    Descriptor = ExAllocatePool2(
        POOL_FLAG_PAGED, Response->output_bytes,
        INFILFS_NATIVE_REQUEST_TAG);
    if (!Descriptor) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto out;
    }
    RtlCopyMemory(
        Descriptor, Response->output, Response->output_bytes);
    if (!RtlValidSecurityDescriptor(Descriptor)) {
        Status = STATUS_INVALID_SECURITY_DESCR;
        goto out;
    }

    *DescriptorOut = Descriptor;
    *LengthOut = Response->output_bytes;
    Descriptor = NULL;
    Status = STATUS_SUCCESS;

out:
    if (Descriptor)
        ExFreePoolWithTag(Descriptor, INFILFS_NATIVE_REQUEST_TAG);
    if (Response)
        ExFreePoolWithTag(Response, INFILFS_NATIVE_REQUEST_TAG);
    if (Request)
        ExFreePoolWithTag(Request, INFILFS_NATIVE_REQUEST_TAG);
    return Status;
}

static NTSTATUS InfilfsStoreSecurityDescriptor(
    INFILFS_NATIVE_VOLUME *Volume, PCUNICODE_STRING Path,
    PSECURITY_DESCRIPTOR Descriptor)
{
    struct infilfs_win_native_request *Request = NULL;
    struct infilfs_win_native_response *Response = NULL;
    ULONG Bytes;
    NTSTATUS Status;

    if (!Volume || !Path || !Descriptor ||
        !RtlValidSecurityDescriptor(Descriptor))
        return STATUS_INVALID_PARAMETER;
    Bytes = RtlLengthSecurityDescriptor(Descriptor);
    if (!Bytes || Bytes > INFILFS_WIN_NATIVE_IO_CHUNK)
        return STATUS_BUFFER_OVERFLOW;

    Request = ExAllocatePool2(
        POOL_FLAG_PAGED, sizeof(*Request), INFILFS_NATIVE_REQUEST_TAG);
    Response = ExAllocatePool2(
        POOL_FLAG_PAGED, sizeof(*Response), INFILFS_NATIVE_REQUEST_TAG);
    if (!Request || !Response) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto out;
    }
    RtlZeroMemory(Request, sizeof(*Request));
    RtlZeroMemory(Response, sizeof(*Response));
    Request->opcode = INFILFS_WIN_NATIVE_OP_SET_SECURITY;
    Request->desired_access =
        OWNER_SECURITY_INFORMATION |
        GROUP_SECURITY_INFORMATION |
        DACL_SECURITY_INFORMATION;
    Request->input_bytes = Bytes;
    InfilfsCopyPathToRequest(Request, Path);
    RtlCopyMemory(Request->input, Descriptor, Bytes);
    Status = InfilfsCallService(Volume, Request, Response);

out:
    if (Response)
        ExFreePoolWithTag(Response, INFILFS_NATIVE_REQUEST_TAG);
    if (Request)
        ExFreePoolWithTag(Request, INFILFS_NATIVE_REQUEST_TAG);
    return Status;
}

static NTSTATUS InfilfsCheckAccess(
    INFILFS_NATIVE_VOLUME *Volume, PCUNICODE_STRING Path,
    PACCESS_STATE AccessState, ACCESS_MASK DesiredAccess,
    KPROCESSOR_MODE AccessMode)
{
    PSECURITY_DESCRIPTOR Descriptor = NULL;
    PPRIVILEGE_SET Privileges = NULL;
    ACCESS_MASK GrantedAccess = 0;
    ULONG DescriptorLength = 0;
    NTSTATUS AccessStatus = STATUS_SUCCESS;
    NTSTATUS Status;
    BOOLEAN Granted;

    if (!DesiredAccess)
        return STATUS_SUCCESS;
    if (!AccessState)
        return STATUS_INVALID_PARAMETER;

    Status = InfilfsFetchSecurityDescriptor(
        Volume, Path, &Descriptor, &DescriptorLength);
    if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
        return STATUS_SUCCESS;
    if (!NT_SUCCESS(Status))
        return Status;

    SeLockSubjectContext(&AccessState->SubjectSecurityContext);
    Granted = SeAccessCheck(
        Descriptor, &AccessState->SubjectSecurityContext, TRUE,
        DesiredAccess, AccessState->PreviouslyGrantedAccess,
        &Privileges, IoGetFileObjectGenericMapping(),
        AccessMode, &GrantedAccess, &AccessStatus);
    if (Privileges) {
        (void)SeAppendPrivileges(AccessState, Privileges);
        SeFreePrivileges(Privileges);
    }
    SeUnlockSubjectContext(&AccessState->SubjectSecurityContext);

    ExFreePoolWithTag(Descriptor, INFILFS_NATIVE_REQUEST_TAG);
    return Granted ? STATUS_SUCCESS : AccessStatus;
}

static NTSTATUS InfilfsCheckTraverseAccess(
    INFILFS_NATIVE_VOLUME *Volume, PCUNICODE_STRING Path,
    PACCESS_STATE AccessState, KPROCESSOR_MODE AccessMode)
{
    PWCHAR Buffer;
    UNICODE_STRING Prefix;
    ULONG Chars;
    NTSTATUS Status = STATUS_SUCCESS;

    if (!Path || !Path->Buffer || !AccessState)
        return STATUS_INVALID_PARAMETER;
    if (AccessState->Flags & TOKEN_HAS_TRAVERSE_PRIVILEGE)
        return STATUS_SUCCESS;

    Chars = Path->Length / sizeof(WCHAR);
    if (Chars <= 1u)
        return STATUS_SUCCESS;
    Buffer = ExAllocatePool2(
        POOL_FLAG_PAGED, Path->Length + sizeof(WCHAR),
        INFILFS_NATIVE_REQUEST_TAG);
    if (!Buffer)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlCopyMemory(Buffer, Path->Buffer, Path->Length);
    Buffer[Chars] = L'\0';

    for (ULONG i = 1u; i < Chars; ++i) {
        WCHAR Saved;
        if (Buffer[i] != L'\\')
            continue;
        Saved = Buffer[i];
        Buffer[i] = L'\0';
        Prefix.Buffer = Buffer;
        Prefix.Length = (USHORT)(i * sizeof(WCHAR));
        Prefix.MaximumLength = Prefix.Length + sizeof(WCHAR);
        Status = InfilfsCheckAccess(
            Volume, &Prefix, AccessState, FILE_TRAVERSE, AccessMode);
        Buffer[i] = Saved;
        if (!NT_SUCCESS(Status))
            break;
    }

    ExFreePoolWithTag(Buffer, INFILFS_NATIVE_REQUEST_TAG);
    return Status;
}

static NTSTATUS InfilfsParentPath(
    PCUNICODE_STRING Path, PUNICODE_STRING Parent)
{
    ULONG Chars;

    if (!Path || !Path->Buffer || !Parent || !Path->Length)
        return STATUS_INVALID_PARAMETER;
    Chars = Path->Length / sizeof(WCHAR);
    while (Chars > 1u && Path->Buffer[Chars - 1u] != L'\\')
        Chars--;
    if (Chars <= 1u) {
        Parent->Buffer = Path->Buffer;
        Parent->Length = sizeof(WCHAR);
        Parent->MaximumLength = Parent->Length;
        return STATUS_SUCCESS;
    }
    Parent->Buffer = Path->Buffer;
    Parent->Length = (USHORT)((Chars - 1u) * sizeof(WCHAR));
    Parent->MaximumLength = Parent->Length;
    return STATUS_SUCCESS;
}

static NTSTATUS InfilfsAssignInitialSecurity(
    INFILFS_NATIVE_VOLUME *Volume, PCUNICODE_STRING Path,
    BOOLEAN Directory, PACCESS_STATE AccessState)
{
    UNICODE_STRING ParentPath;
    PSECURITY_DESCRIPTOR ParentDescriptor = NULL;
    PSECURITY_DESCRIPTOR NewDescriptor = NULL;
    ULONG ParentLength = 0;
    NTSTATUS Status;

    if (!Volume || !Path || !AccessState)
        return STATUS_INVALID_PARAMETER;

    Status = InfilfsParentPath(Path, &ParentPath);
    if (!NT_SUCCESS(Status))
        return Status;
    Status = InfilfsFetchSecurityDescriptor(
        Volume, &ParentPath, &ParentDescriptor, &ParentLength);
    if (Status == STATUS_OBJECT_NAME_NOT_FOUND)
        Status = STATUS_SUCCESS;
    if (!NT_SUCCESS(Status))
        goto out;

    Status = SeAssignSecurity(
        ParentDescriptor, AccessState->SecurityDescriptor,
        &NewDescriptor, Directory,
        &AccessState->SubjectSecurityContext,
        IoGetFileObjectGenericMapping(), PagedPool);
    if (!NT_SUCCESS(Status))
        goto out;

    Status = InfilfsStoreSecurityDescriptor(
        Volume, Path, NewDescriptor);

out:
    if (NewDescriptor)
        SeDeassignSecurity(&NewDescriptor);
    if (ParentDescriptor)
        ExFreePoolWithTag(
            ParentDescriptor, INFILFS_NATIVE_REQUEST_TAG);
    return Status;
}

static NTSTATUS InfilfsResolveOpenPath(
    PFILE_OBJECT FileObject, PUNICODE_STRING Path, PWCHAR *Allocated)
{
    INFILFS_NATIVE_FCB *Parent;
    USHORT ParentLength;
    USHORT ChildLength;
    ULONG Bytes;
    BOOLEAN Separator;

    if (!FileObject || !Path || !Allocated)
        return STATUS_INVALID_PARAMETER;
    *Allocated = NULL;

    if (!FileObject->RelatedFileObject ||
        !FileObject->FileName.Length ||
        (FileObject->FileName.Buffer &&
         FileObject->FileName.Buffer[0] == L'\\')) {
        *Path = FileObject->FileName;
        if (!Path->Length) {
            static WCHAR Root[] = L"\\";
            RtlInitUnicodeString(Path, Root);
        }
        return STATUS_SUCCESS;
    }

    Parent = (INFILFS_NATIVE_FCB *)
        FileObject->RelatedFileObject->FsContext;
    if (!Parent || Parent->Signature != 'fSfI' ||
        !Parent->Path.Buffer)
        return STATUS_INVALID_PARAMETER;

    ParentLength = Parent->Path.Length;
    ChildLength = FileObject->FileName.Length;
    Separator = ParentLength >= sizeof(WCHAR) &&
        Parent->Path.Buffer[
            ParentLength / sizeof(WCHAR) - 1u] != L'\\';
    Bytes = (ULONG)ParentLength +
        (Separator ? sizeof(WCHAR) : 0u) +
        (ULONG)ChildLength;
    if (Bytes > MAXUSHORT - sizeof(WCHAR))
        return STATUS_NAME_TOO_LONG;

    PWCHAR Buffer = ExAllocatePool2(
        POOL_FLAG_PAGED, Bytes + sizeof(WCHAR),
        INFILFS_NATIVE_FCB_TAG);
    if (!Buffer)
        return STATUS_INSUFFICIENT_RESOURCES;

    ULONG Cursor = 0;
    RtlCopyMemory(Buffer, Parent->Path.Buffer, ParentLength);
    Cursor += ParentLength / sizeof(WCHAR);
    if (Separator)
        Buffer[Cursor++] = L'\\';
    RtlCopyMemory(
        Buffer + Cursor, FileObject->FileName.Buffer, ChildLength);
    Cursor += ChildLength / sizeof(WCHAR);
    Buffer[Cursor] = L'\0';

    Path->Buffer = Buffer;
    Path->Length = (USHORT)Bytes;
    Path->MaximumLength = (USHORT)(Bytes + sizeof(WCHAR));
    *Allocated = Buffer;
    return STATUS_SUCCESS;
}

static NTSTATUS InfilfsResolveSetTargetPath(
    HANDLE RootDirectory, PWCHAR FileName, ULONG FileNameLength,
    KPROCESSOR_MODE RequestorMode, PUNICODE_STRING Path, PWCHAR *Allocated)
{
    PFILE_OBJECT RootFile = NULL;
    INFILFS_NATIVE_FCB *Parent = NULL;
    USHORT ParentLength = 0;
    ULONG Bytes;
    BOOLEAN Separator = FALSE;
    NTSTATUS Status;

    if (!FileName || !FileNameLength || !Path || !Allocated ||
        (FileNameLength & (sizeof(WCHAR) - 1u)) != 0)
        return STATUS_INVALID_PARAMETER;
    *Allocated = NULL;

    if (!RootDirectory || FileName[0] == L'\\') {
        Path->Buffer = FileName;
        Path->Length = (USHORT)FileNameLength;
        Path->MaximumLength = Path->Length;
        return STATUS_SUCCESS;
    }

    Status = ObReferenceObjectByHandle(
        RootDirectory, FILE_TRAVERSE, *IoFileObjectType,
        RequestorMode, (PVOID *)&RootFile, NULL);
    if (!NT_SUCCESS(Status))
        return Status;

    Parent = (INFILFS_NATIVE_FCB *)RootFile->FsContext;
    if (!Parent || Parent->Signature != 'fSfI' || !Parent->Path.Buffer) {
        ObDereferenceObject(RootFile);
        return STATUS_INVALID_PARAMETER;
    }

    ParentLength = Parent->Path.Length;
    Separator = ParentLength >= sizeof(WCHAR) &&
        Parent->Path.Buffer[ParentLength / sizeof(WCHAR) - 1u] != L'\\';
    Bytes = (ULONG)ParentLength +
        (Separator ? sizeof(WCHAR) : 0u) + FileNameLength;
    if (Bytes > MAXUSHORT - sizeof(WCHAR)) {
        ObDereferenceObject(RootFile);
        return STATUS_NAME_TOO_LONG;
    }

    PWCHAR Buffer = ExAllocatePool2(
        POOL_FLAG_PAGED, Bytes + sizeof(WCHAR),
        INFILFS_NATIVE_FCB_TAG);
    if (!Buffer) {
        ObDereferenceObject(RootFile);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ULONG Cursor = 0;
    RtlCopyMemory(Buffer, Parent->Path.Buffer, ParentLength);
    Cursor += ParentLength / sizeof(WCHAR);
    if (Separator)
        Buffer[Cursor++] = L'\\';
    RtlCopyMemory(Buffer + Cursor, FileName, FileNameLength);
    Cursor += FileNameLength / sizeof(WCHAR);
    Buffer[Cursor] = L'\0';

    Path->Buffer = Buffer;
    Path->Length = (USHORT)Bytes;
    Path->MaximumLength = (USHORT)(Bytes + sizeof(WCHAR));
    *Allocated = Buffer;
    ObDereferenceObject(RootFile);
    return STATUS_SUCCESS;
}

static NTSTATUS InfilfsCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    INFILFS_NATIVE_VOLUME *Volume = InfilfsVolumeFromDevice(DeviceObject);
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    struct infilfs_win_native_attributes Attributes;
    INFILFS_NATIVE_FCB *Fcb = NULL;
    INFILFS_NATIVE_CCB *Ccb = NULL;
    UNICODE_STRING OpenPath;
    PWCHAR AllocatedPath = NULL;
    NTSTATUS Status;
    ULONG Disposition;
    ULONG Options;
    BOOLEAN DirectoryRequested;
    BOOLEAN CreatedObject = FALSE;
    PACCESS_STATE AccessState;
    ACCESS_MASK DesiredAccess;
    ULONG_PTR CreateInformation = FILE_OPENED;

    if (InfilfsIsControlDevice(DeviceObject) ||
        InfilfsIsFileSystemDevice(DeviceObject))
        return InfilfsCompleteIrp(Irp, STATUS_SUCCESS, FILE_OPENED);
    if (!Volume || !FileObject)
        return InfilfsCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    if (!IrpSp->Parameters.Create.SecurityContext ||
        !IrpSp->Parameters.Create.SecurityContext->AccessState)
        return InfilfsCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
    AccessState =
        IrpSp->Parameters.Create.SecurityContext->AccessState;
    DesiredAccess =
        IrpSp->Parameters.Create.SecurityContext->DesiredAccess;
    if (Volume->Dismounted)
        return InfilfsCompleteIrp(Irp, STATUS_VOLUME_DISMOUNTED, 0);
    if (Volume->Locked)
        return InfilfsCompleteIrp(Irp, STATUS_ACCESS_DENIED, 0);

    Status = InfilfsResolveOpenPath(
        FileObject, &OpenPath, &AllocatedPath);
    if (!NT_SUCCESS(Status))
        return InfilfsCompleteIrp(Irp, Status, 0);

    Disposition = (IrpSp->Parameters.Create.Options >> 24) & 0xffu;
    Options = IrpSp->Parameters.Create.Options & 0x00ffffffu;
    DirectoryRequested = (Options & FILE_DIRECTORY_FILE) != 0;

    Status = InfilfsCheckTraverseAccess(
        Volume, &OpenPath, AccessState, Irp->RequestorMode);
    if (!NT_SUCCESS(Status))
        goto complete;

    RtlZeroMemory(&Attributes, sizeof(Attributes));
    Status = InfilfsLookupPath(Volume, &OpenPath, &Attributes);
    if (Status == STATUS_OBJECT_NAME_NOT_FOUND) {
        if (Disposition == FILE_OPEN || Disposition == FILE_OVERWRITE)
            goto complete;

        {
            UNICODE_STRING ParentPath;
            ACCESS_MASK ParentAccess = DirectoryRequested ?
                FILE_ADD_SUBDIRECTORY : FILE_ADD_FILE;
            Status = InfilfsParentPath(&OpenPath, &ParentPath);
            if (NT_SUCCESS(Status))
                Status = InfilfsCheckAccess(
                    Volume, &ParentPath, AccessState,
                    ParentAccess, Irp->RequestorMode);
        }
        if (!NT_SUCCESS(Status))
            goto complete;

        Status = InfilfsServiceMutation(
            Volume,
            DirectoryRequested ? INFILFS_WIN_NATIVE_OP_MKDIR :
                                 INFILFS_WIN_NATIVE_OP_CREATE,
            &OpenPath, NULL, 0, 0);
        if (!NT_SUCCESS(Status))
            goto complete;
        CreatedObject = TRUE;
        Status = InfilfsAssignInitialSecurity(
            Volume, &OpenPath, DirectoryRequested, AccessState);
        if (!NT_SUCCESS(Status)) {
            (void)InfilfsServiceMutation(
                Volume,
                DirectoryRequested ? INFILFS_WIN_NATIVE_OP_RMDIR :
                                     INFILFS_WIN_NATIVE_OP_UNLINK,
                &OpenPath, NULL, 0, 0);
            goto complete;
        }
        CreateInformation = FILE_CREATED;
        RtlZeroMemory(&Attributes, sizeof(Attributes));
        Status = InfilfsLookupPath(
            Volume, &OpenPath, &Attributes);
    } else if (NT_SUCCESS(Status)) {
        ACCESS_MASK RequiredAccess = DesiredAccess;
        if (Disposition == FILE_OVERWRITE ||
            Disposition == FILE_OVERWRITE_IF)
            RequiredAccess |=
                FILE_WRITE_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES;
        if (Disposition == FILE_SUPERSEDE)
            RequiredAccess |=
                DELETE | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES;
        Status = InfilfsCheckAccess(
            Volume, &OpenPath, AccessState,
            RequiredAccess, Irp->RequestorMode);
        if (!NT_SUCCESS(Status))
            goto complete;

        if (Disposition == FILE_CREATE) {
            Status = STATUS_OBJECT_NAME_COLLISION;
            goto complete;
        }

        if (DirectoryRequested &&
            Attributes.object_type != INFILFS_WIN_NATIVE_OBJECT_DIRECTORY) {
            Status = STATUS_NOT_A_DIRECTORY;
            goto complete;
        }
        if ((Options & FILE_NON_DIRECTORY_FILE) &&
            Attributes.object_type == INFILFS_WIN_NATIVE_OBJECT_DIRECTORY) {
            Status = STATUS_FILE_IS_A_DIRECTORY;
            goto complete;
        }

        if (Disposition == FILE_OVERWRITE ||
            Disposition == FILE_OVERWRITE_IF ||
            Disposition == FILE_SUPERSEDE) {
            if (Attributes.object_type != INFILFS_WIN_NATIVE_OBJECT_FILE) {
                Status = STATUS_FILE_IS_A_DIRECTORY;
                goto complete;
            }
            Status = InfilfsServiceMutation(
                Volume, INFILFS_WIN_NATIVE_OP_TRUNCATE,
                &OpenPath, NULL, 0, 0);
            if (!NT_SUCCESS(Status))
                goto complete;
            Attributes.logical_size = 0;
            Attributes.allocation_size = 0;
            CreateInformation =
                Disposition == FILE_SUPERSEDE ?
                FILE_SUPERSEDED : FILE_OVERWRITTEN;
        }
    }
    if (!NT_SUCCESS(Status))
        goto complete;

    Fcb = InfilfsGetOrCreateFcb(
        Volume, &OpenPath, &Attributes);
    Ccb = ExAllocatePool2(
        POOL_FLAG_NON_PAGED, sizeof(*Ccb), INFILFS_NATIVE_CCB_TAG);
    if (!Fcb || !Ccb) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto complete;
    }
    RtlZeroMemory(Ccb, sizeof(*Ccb));
    Ccb->Signature = 'cSfI';

    if (!IrpSp->Parameters.Create.SecurityContext) {
        Status = STATUS_INVALID_PARAMETER;
        goto complete;
    }
    Status = IoCheckShareAccess(
        IrpSp->Parameters.Create.SecurityContext->DesiredAccess,
        IrpSp->Parameters.Create.ShareAccess,
        FileObject, &Fcb->ShareAccess, TRUE);
    if (!NT_SUCCESS(Status))
        goto complete;
    Ccb->ShareRegistered = TRUE;
    FileObject->ReadAccess =
        (DesiredAccess &
         (FILE_READ_DATA | FILE_EXECUTE | FILE_READ_ATTRIBUTES |
          FILE_READ_EA | READ_CONTROL)) != 0;
    FileObject->WriteAccess =
        (DesiredAccess &
         (FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES |
          FILE_WRITE_EA | WRITE_DAC | WRITE_OWNER)) != 0;
    FileObject->DeleteAccess = (DesiredAccess & DELETE) != 0;

    FileObject->FsContext = Fcb;
    FileObject->FsContext2 = Ccb;
    FileObject->SectionObjectPointer = &Fcb->SectionObjectPointers;

    if (Options & FILE_DELETE_ON_CLOSE) {
        Ccb->DeletePending = TRUE;
        InterlockedIncrement(&Fcb->DeletePendingCount);
    }

    if (Fcb->ObjectType == INFILFS_WIN_NATIVE_OBJECT_FILE &&
        !(FileObject->Flags & FO_NO_INTERMEDIATE_BUFFERING)) {
        CC_FILE_SIZES Sizes;
        Sizes.AllocationSize = Fcb->Header.AllocationSize;
        Sizes.FileSize = Fcb->Header.FileSize;
        Sizes.ValidDataLength = Fcb->Header.ValidDataLength;
        CcInitializeCacheMap(
            FileObject, &Sizes, FALSE,
            (PCACHE_MANAGER_CALLBACKS)&g_InfilfsCacheCallbacks, Fcb);
    }

    if (AllocatedPath)
        ExFreePoolWithTag(AllocatedPath, INFILFS_NATIVE_FCB_TAG);
    return InfilfsCompleteIrp(
        Irp, STATUS_SUCCESS, CreateInformation);

complete:
    UNREFERENCED_PARAMETER(CreatedObject);
    if (Ccb) {
        if (Ccb->ShareRegistered && Fcb)
            IoRemoveShareAccess(FileObject, &Fcb->ShareAccess);
        ExFreePoolWithTag(Ccb, INFILFS_NATIVE_CCB_TAG);
    }
    if (Fcb)
        InfilfsDereferenceFcb(Fcb);
    if (AllocatedPath)
        ExFreePoolWithTag(AllocatedPath, INFILFS_NATIVE_FCB_TAG);
    return InfilfsCompleteIrp(Irp, Status, 0);
}

static PVOID InfilfsGetIrpBuffer(PIRP Irp)
{
    if (Irp->MdlAddress)
        return MmGetSystemAddressForMdlSafe(
            Irp->MdlAddress, NormalPagePriority | MdlMappingNoExecute);
    if (Irp->AssociatedIrp.SystemBuffer)
        return Irp->AssociatedIrp.SystemBuffer;
    return Irp->UserBuffer;
}

static NTSTATUS InfilfsTransfer(
    INFILFS_NATIVE_VOLUME *Volume, INFILFS_NATIVE_FCB *Fcb,
    BOOLEAN Write, ULONGLONG Offset, PVOID Buffer, ULONG Length,
    BOOLEAN PagingIo, BOOLEAN WriteThrough, ULONG *Transferred)
{
    struct infilfs_win_native_request *Request;
    struct infilfs_win_native_response *Response;
    ULONG Done = 0;
    NTSTATUS Status = STATUS_SUCCESS;

    Request = ExAllocatePool2(
        POOL_FLAG_PAGED, sizeof(*Request), INFILFS_NATIVE_REQUEST_TAG);
    Response = ExAllocatePool2(
        POOL_FLAG_PAGED, sizeof(*Response), INFILFS_NATIVE_REQUEST_TAG);
    if (!Request || !Response) {
        if (Request) ExFreePoolWithTag(Request, INFILFS_NATIVE_REQUEST_TAG);
        if (Response) ExFreePoolWithTag(Response, INFILFS_NATIVE_REQUEST_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    while (Done < Length) {
        ULONG Chunk = Length - Done;
        if (Chunk > INFILFS_WIN_NATIVE_IO_CHUNK)
            Chunk = INFILFS_WIN_NATIVE_IO_CHUNK;
        RtlZeroMemory(Request, sizeof(*Request));
        RtlZeroMemory(Response, sizeof(*Response));
        Request->opcode = Write ?
            INFILFS_WIN_NATIVE_OP_WRITE : INFILFS_WIN_NATIVE_OP_READ;
        Request->offset = Offset + Done;
        Request->length = Chunk;
        if (PagingIo)
            Request->flags |= INFILFS_WIN_NATIVE_REQ_PAGING_IO;
        if (WriteThrough)
            Request->flags |= INFILFS_WIN_NATIVE_REQ_WRITE_THROUGH;
        InfilfsCopyPathToRequest(Request, &Fcb->Path);
        if (Write) {
            Request->input_bytes = Chunk;
            RtlCopyMemory(Request->input, (PUCHAR)Buffer + Done, Chunk);
        }
        Status = InfilfsCallService(Volume, Request, Response);
        if (!NT_SUCCESS(Status))
            break;
        if (!Write) {
            if (Response->output_bytes > Chunk) {
                Status = STATUS_DATA_ERROR;
                break;
            }
            RtlCopyMemory(
                (PUCHAR)Buffer + Done, Response->output,
                Response->output_bytes);
            Done += Response->output_bytes;
            if (Response->output_bytes < Chunk)
                break;
        } else {
            Done += Chunk;
        }
    }

    *Transferred = Done;
    ExFreePoolWithTag(Response, INFILFS_NATIVE_REQUEST_TAG);
    ExFreePoolWithTag(Request, INFILFS_NATIVE_REQUEST_TAG);
    return Status;
}

static NTSTATUS InfilfsFlushPortableVolume(
    INFILFS_NATIVE_VOLUME *Volume, PCUNICODE_STRING Path)
{
    struct infilfs_win_native_request *Request;
    struct infilfs_win_native_response *Response;
    NTSTATUS Status;

    if (!Volume)
        return STATUS_INVALID_DEVICE_REQUEST;

    Request = ExAllocatePool2(
        POOL_FLAG_PAGED, sizeof(*Request), INFILFS_NATIVE_REQUEST_TAG);
    Response = ExAllocatePool2(
        POOL_FLAG_PAGED, sizeof(*Response), INFILFS_NATIVE_REQUEST_TAG);
    if (!Request || !Response) {
        if (Request)
            ExFreePoolWithTag(Request, INFILFS_NATIVE_REQUEST_TAG);
        if (Response)
            ExFreePoolWithTag(Response, INFILFS_NATIVE_REQUEST_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(Request, sizeof(*Request));
    RtlZeroMemory(Response, sizeof(*Response));
    Request->opcode = INFILFS_WIN_NATIVE_OP_FLUSH;
    if (Path)
        InfilfsCopyPathToRequest(Request, Path);
    Status = InfilfsCallService(Volume, Request, Response);
    if (NT_SUCCESS(Status))
        Status = InfilfsTargetFlush(Volume->TargetDevice);

    ExFreePoolWithTag(Response, INFILFS_NATIVE_REQUEST_TAG);
    ExFreePoolWithTag(Request, INFILFS_NATIVE_REQUEST_TAG);
    return Status;
}

static NTSTATUS InfilfsRead(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    INFILFS_NATIVE_VOLUME *Volume = InfilfsVolumeFromDevice(DeviceObject);
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    INFILFS_NATIVE_FCB *Fcb;
    PVOID Buffer;
    ULONG Length;
    LARGE_INTEGER Offset;
    ULONG Done = 0;
    NTSTATUS Status;

    if (!Volume || !FileObject || !FileObject->FsContext)
        return InfilfsCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    Fcb = (INFILFS_NATIVE_FCB *)FileObject->FsContext;
    if (Fcb->ObjectType != INFILFS_WIN_NATIVE_OBJECT_FILE)
        return InfilfsCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);

    Length = IrpSp->Parameters.Read.Length;
    Offset = IrpSp->Parameters.Read.ByteOffset;
    if (Offset.HighPart == -1 &&
        Offset.LowPart == FILE_USE_FILE_POINTER_POSITION) {
        if (!(FileObject->Flags & FO_SYNCHRONOUS_IO))
            return InfilfsCompleteIrp(
                Irp, STATUS_INVALID_PARAMETER, 0);
        Offset = FileObject->CurrentByteOffset;
    }
    if (Offset.QuadPart < 0)
        return InfilfsCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
    if (!Length)
        return InfilfsCompleteIrp(Irp, STATUS_SUCCESS, 0);
    Buffer = InfilfsGetIrpBuffer(Irp);
    if (!Buffer)
        return InfilfsCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);

    if (!(Irp->Flags & (IRP_NOCACHE | IRP_PAGING_IO))) {
        IO_STATUS_BLOCK Iosb;
        __try {
            if (!CcCopyRead(
                    FileObject, &Offset, Length, TRUE, Buffer, &Iosb))
                return InfilfsCompleteIrp(
                    Irp, STATUS_CANT_WAIT, 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return InfilfsCompleteIrp(
                Irp, GetExceptionCode(), 0);
        }
        if (NT_SUCCESS(Iosb.Status) &&
            (FileObject->Flags & FO_SYNCHRONOUS_IO))
            FileObject->CurrentByteOffset.QuadPart =
                Offset.QuadPart + Iosb.Information;
        return InfilfsCompleteIrp(
            Irp, Iosb.Status, Iosb.Information);
    }

    Status = InfilfsTransfer(
        Volume, Fcb, FALSE, (ULONGLONG)Offset.QuadPart,
        Buffer, Length, (Irp->Flags & IRP_PAGING_IO) != 0,
        FALSE, &Done);
    if (NT_SUCCESS(Status) &&
        (FileObject->Flags & FO_SYNCHRONOUS_IO))
        FileObject->CurrentByteOffset.QuadPart =
            Offset.QuadPart + Done;
    return InfilfsCompleteIrp(Irp, Status, Done);
}

static NTSTATUS InfilfsWrite(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    INFILFS_NATIVE_VOLUME *Volume = InfilfsVolumeFromDevice(DeviceObject);
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    INFILFS_NATIVE_FCB *Fcb;
    PVOID Buffer;
    ULONG Length;
    LARGE_INTEGER Offset;
    ULONG Done = 0;
    NTSTATUS Status;
    BOOLEAN WriteThrough;

    if (!Volume || !FileObject || !FileObject->FsContext)
        return InfilfsCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    if (Volume->ReadOnly)
        return InfilfsCompleteIrp(Irp, STATUS_MEDIA_WRITE_PROTECTED, 0);
    Fcb = (INFILFS_NATIVE_FCB *)FileObject->FsContext;
    if (Fcb->ObjectType != INFILFS_WIN_NATIVE_OBJECT_FILE)
        return InfilfsCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);

    Length = IrpSp->Parameters.Write.Length;
    Offset = IrpSp->Parameters.Write.ByteOffset;
    if (Offset.HighPart == -1 &&
        Offset.LowPart == FILE_WRITE_TO_END_OF_FILE) {
        Offset = Fcb->Header.FileSize;
    } else if (Offset.HighPart == -1 &&
               Offset.LowPart == FILE_USE_FILE_POINTER_POSITION) {
        if (!(FileObject->Flags & FO_SYNCHRONOUS_IO))
            return InfilfsCompleteIrp(
                Irp, STATUS_INVALID_PARAMETER, 0);
        Offset = FileObject->CurrentByteOffset;
    }
    if (Offset.QuadPart < 0)
        return InfilfsCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
    if (!Length)
        return InfilfsCompleteIrp(Irp, STATUS_SUCCESS, 0);
    Buffer = InfilfsGetIrpBuffer(Irp);
    if (!Buffer)
        return InfilfsCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    WriteThrough =
        (IrpSp->Flags & SL_WRITE_THROUGH) != 0 ||
        (FileObject->Flags & FO_WRITE_THROUGH) != 0;

    if (!(Irp->Flags & (IRP_NOCACHE | IRP_PAGING_IO))) {
        BOOLEAN Copied = FALSE;
        __try {
            Copied = CcCopyWrite(
                FileObject, &Offset, Length, TRUE, Buffer);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return InfilfsCompleteIrp(
                Irp, GetExceptionCode(), 0);
        }
        if (!Copied)
            return InfilfsCompleteIrp(Irp, STATUS_CANT_WAIT, 0);
        if (Offset.QuadPart + Length > Fcb->Header.FileSize.QuadPart) {
            Fcb->Header.FileSize.QuadPart = Offset.QuadPart + Length;
            Fcb->Header.ValidDataLength = Fcb->Header.FileSize;
            if (Fcb->Header.AllocationSize.QuadPart <
                Fcb->Header.FileSize.QuadPart)
                Fcb->Header.AllocationSize = Fcb->Header.FileSize;
            {
                CC_FILE_SIZES Sizes;
                Sizes.AllocationSize = Fcb->Header.AllocationSize;
                Sizes.FileSize = Fcb->Header.FileSize;
                Sizes.ValidDataLength = Fcb->Header.ValidDataLength;
                CcSetFileSizes(FileObject, &Sizes);
            }
        }
        if (WriteThrough) {
            IO_STATUS_BLOCK FlushStatus;
            RtlZeroMemory(&FlushStatus, sizeof(FlushStatus));
            CcFlushCache(
                FileObject->SectionObjectPointer,
                &Offset, Length, &FlushStatus);
            if (!NT_SUCCESS(FlushStatus.Status))
                return InfilfsCompleteIrp(
                    Irp, FlushStatus.Status, 0);
            Status = InfilfsFlushPortableVolume(
                Volume, &Fcb->Path);
            if (!NT_SUCCESS(Status))
                return InfilfsCompleteIrp(Irp, Status, 0);
        }
        if (FileObject->Flags & FO_SYNCHRONOUS_IO)
            FileObject->CurrentByteOffset.QuadPart =
                Offset.QuadPart + Length;
        return InfilfsCompleteIrp(Irp, STATUS_SUCCESS, Length);
    }

    Status = InfilfsTransfer(
        Volume, Fcb, TRUE, (ULONGLONG)Offset.QuadPart,
        Buffer, Length, (Irp->Flags & IRP_PAGING_IO) != 0,
        WriteThrough, &Done);
    if (NT_SUCCESS(Status) &&
        Offset.QuadPart + Done > Fcb->Header.FileSize.QuadPart) {
        Fcb->Header.FileSize.QuadPart = Offset.QuadPart + Done;
        Fcb->Header.ValidDataLength = Fcb->Header.FileSize;
    }
    if (NT_SUCCESS(Status) &&
        (FileObject->Flags & FO_SYNCHRONOUS_IO))
        FileObject->CurrentByteOffset.QuadPart =
            Offset.QuadPart + Done;
    return InfilfsCompleteIrp(Irp, Status, Done);
}


static NTSTATUS InfilfsQueryInformation(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    INFILFS_NATIVE_FCB *Fcb = FileObject ?
        (INFILFS_NATIVE_FCB *)FileObject->FsContext : NULL;
    PVOID Buffer = Irp->AssociatedIrp.SystemBuffer;
    ULONG Length = IrpSp->Parameters.QueryFile.Length;
    FILE_INFORMATION_CLASS Class =
        IrpSp->Parameters.QueryFile.FileInformationClass;
    ULONG_PTR Used = 0;
    NTSTATUS Status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(DeviceObject);
    if (!Fcb || !Buffer)
        return InfilfsCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);

    switch (Class) {
    case FileBasicInformation: {
        PFILE_BASIC_INFORMATION Info = Buffer;
        if (Length < sizeof(*Info)) {
            Status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        RtlZeroMemory(Info, sizeof(*Info));
        Info->CreationTime = Fcb->CreationTime;
        Info->LastAccessTime = Fcb->AccessTime;
        Info->LastWriteTime = Fcb->WriteTime;
        Info->ChangeTime = Fcb->ChangeTime;
        Info->FileAttributes = Fcb->FileAttributes ?
            Fcb->FileAttributes : FILE_ATTRIBUTE_NORMAL;
        Used = sizeof(*Info);
        break;
    }
    case FileStandardInformation: {
        PFILE_STANDARD_INFORMATION Info = Buffer;
        if (Length < sizeof(*Info)) {
            Status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        RtlZeroMemory(Info, sizeof(*Info));
        Info->AllocationSize = Fcb->Header.AllocationSize;
        Info->EndOfFile = Fcb->Header.FileSize;
        Info->NumberOfLinks = Fcb->LinkCount ? Fcb->LinkCount : 1u;
        Info->DeletePending =
            InterlockedCompareExchange(
                &Fcb->DeletePendingCount, 0, 0) > 0;
        Info->Directory =
            Fcb->ObjectType == INFILFS_WIN_NATIVE_OBJECT_DIRECTORY;
        Used = sizeof(*Info);
        break;
    }
    case FileInternalInformation: {
        PFILE_INTERNAL_INFORMATION Info = Buffer;
        if (Length < sizeof(*Info)) {
            Status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        Info->IndexNumber.QuadPart = (LONGLONG)Fcb->FileId;
        Used = sizeof(*Info);
        break;
    }
    case FilePositionInformation: {
        PFILE_POSITION_INFORMATION Info = Buffer;
        if (Length < sizeof(*Info)) {
            Status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        Info->CurrentByteOffset = FileObject->CurrentByteOffset;
        Used = sizeof(*Info);
        break;
    }
    case FileNameInformation: {
        PFILE_NAME_INFORMATION Info = Buffer;
        ULONG NameBytes = Fcb->Path.Length;
        ULONG Required = FIELD_OFFSET(FILE_NAME_INFORMATION, FileName) +
                         NameBytes;
        if (Length < FIELD_OFFSET(FILE_NAME_INFORMATION, FileName)) {
            Status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        Info->FileNameLength = NameBytes;
        ULONG Copy = Length - FIELD_OFFSET(FILE_NAME_INFORMATION, FileName);
        if (Copy > NameBytes)
            Copy = NameBytes;
        if (Copy)
            RtlCopyMemory(Info->FileName, Fcb->Path.Buffer, Copy);
        Used = FIELD_OFFSET(FILE_NAME_INFORMATION, FileName) + Copy;
        if (Length < Required)
            Status = STATUS_BUFFER_OVERFLOW;
        break;
    }
    case FileNetworkOpenInformation: {
        PFILE_NETWORK_OPEN_INFORMATION Info = Buffer;
        if (Length < sizeof(*Info)) {
            Status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        RtlZeroMemory(Info, sizeof(*Info));
        Info->CreationTime = Fcb->CreationTime;
        Info->LastAccessTime = Fcb->AccessTime;
        Info->LastWriteTime = Fcb->WriteTime;
        Info->ChangeTime = Fcb->ChangeTime;
        Info->AllocationSize = Fcb->Header.AllocationSize;
        Info->EndOfFile = Fcb->Header.FileSize;
        Info->FileAttributes = Fcb->FileAttributes ?
            Fcb->FileAttributes : FILE_ATTRIBUTE_NORMAL;
        Used = sizeof(*Info);
        break;
    }
    default:
        Status = STATUS_INVALID_INFO_CLASS;
        break;
    }

    return InfilfsCompleteIrp(Irp, Status, Used);
}

static NTSTATUS InfilfsSetInformation(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    INFILFS_NATIVE_VOLUME *Volume = InfilfsVolumeFromDevice(DeviceObject);
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    INFILFS_NATIVE_FCB *Fcb = FileObject ?
        (INFILFS_NATIVE_FCB *)FileObject->FsContext : NULL;
    PVOID Buffer = Irp->AssociatedIrp.SystemBuffer;
    FILE_INFORMATION_CLASS Class =
        IrpSp->Parameters.SetFile.FileInformationClass;
    ULONG Length = IrpSp->Parameters.SetFile.Length;
    NTSTATUS Status = STATUS_SUCCESS;

    if (!Volume || !Fcb || !Buffer)
        return InfilfsCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
    if (Volume->ReadOnly)
        return InfilfsCompleteIrp(Irp, STATUS_MEDIA_WRITE_PROTECTED, 0);

    switch (Class) {
    case FileBasicInformation: {
        PFILE_BASIC_INFORMATION Info = Buffer;
        struct infilfs_win_native_request *Request = NULL;
        struct infilfs_win_native_response *Response = NULL;
        struct infilfs_win_native_basic Basic;

        if (Length < sizeof(*Info))
            return InfilfsCompleteIrp(
                Irp, STATUS_BUFFER_TOO_SMALL, 0);

        Request = ExAllocatePool2(
            POOL_FLAG_PAGED, sizeof(*Request),
            INFILFS_NATIVE_REQUEST_TAG);
        Response = ExAllocatePool2(
            POOL_FLAG_PAGED, sizeof(*Response),
            INFILFS_NATIVE_REQUEST_TAG);
        if (!Request || !Response) {
            if (Request)
                ExFreePoolWithTag(
                    Request, INFILFS_NATIVE_REQUEST_TAG);
            if (Response)
                ExFreePoolWithTag(
                    Response, INFILFS_NATIVE_REQUEST_TAG);
            return InfilfsCompleteIrp(
                Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
        }

        RtlZeroMemory(Request, sizeof(*Request));
        RtlZeroMemory(Response, sizeof(*Response));
        RtlZeroMemory(&Basic, sizeof(Basic));
        Basic.creation_time_100ns = Info->CreationTime.QuadPart;
        Basic.access_time_100ns = Info->LastAccessTime.QuadPart;
        Basic.write_time_100ns = Info->LastWriteTime.QuadPart;
        Basic.change_time_100ns = Info->ChangeTime.QuadPart;
        Basic.file_attributes = Info->FileAttributes;

        Request->opcode = INFILFS_WIN_NATIVE_OP_SET_BASIC;
        Request->input_bytes = sizeof(Basic);
        RtlCopyMemory(Request->input, &Basic, sizeof(Basic));
        InfilfsCopyPathToRequest(Request, &Fcb->Path);
        Status = InfilfsCallService(Volume, Request, Response);

        ExFreePoolWithTag(Response, INFILFS_NATIVE_REQUEST_TAG);
        ExFreePoolWithTag(Request, INFILFS_NATIVE_REQUEST_TAG);
        if (!NT_SUCCESS(Status))
            break;

        if (Info->CreationTime.QuadPart != 0)
            Fcb->CreationTime = Info->CreationTime;
        if (Info->LastAccessTime.QuadPart != 0)
            Fcb->AccessTime = Info->LastAccessTime;
        if (Info->LastWriteTime.QuadPart != 0)
            Fcb->WriteTime = Info->LastWriteTime;
        if (Info->ChangeTime.QuadPart != 0)
            Fcb->ChangeTime = Info->ChangeTime;
        if (Info->FileAttributes != 0)
            Fcb->FileAttributes = Info->FileAttributes;
        break;
    }
    case FileEndOfFileInformation: {
        PFILE_END_OF_FILE_INFORMATION Info = Buffer;
        CC_FILE_SIZES Sizes;
        if (Length < sizeof(*Info) || Info->EndOfFile.QuadPart < 0)
            return InfilfsCompleteIrp(
                Irp, STATUS_INVALID_PARAMETER, 0);
        if (FileObject->SectionObjectPointer)
            CcFlushCache(FileObject->SectionObjectPointer, NULL, 0, NULL);
        Status = InfilfsServiceMutation(
            Volume, INFILFS_WIN_NATIVE_OP_TRUNCATE,
            &Fcb->Path, NULL, (ULONGLONG)Info->EndOfFile.QuadPart, 0);
        if (!NT_SUCCESS(Status))
            break;
        Fcb->Header.FileSize = Info->EndOfFile;
        Fcb->Header.ValidDataLength = Info->EndOfFile;
        if (Fcb->Header.AllocationSize.QuadPart <
            Info->EndOfFile.QuadPart)
            Fcb->Header.AllocationSize = Info->EndOfFile;
        Sizes.AllocationSize = Fcb->Header.AllocationSize;
        Sizes.FileSize = Fcb->Header.FileSize;
        Sizes.ValidDataLength = Fcb->Header.ValidDataLength;
        CcSetFileSizes(FileObject, &Sizes);
        break;
    }
    case FileDispositionInformation: {
        PFILE_DISPOSITION_INFORMATION Info = Buffer;
        if (Length < sizeof(*Info))
            return InfilfsCompleteIrp(
                Irp, STATUS_BUFFER_TOO_SMALL, 0);
        {
            BOOLEAN Delete = Info->DeleteFile ? TRUE : FALSE;
            if (Delete != Ccb->DeletePending) {
                if (Delete)
                    InterlockedIncrement(&Fcb->DeletePendingCount);
                else
                    InterlockedDecrement(&Fcb->DeletePendingCount);
                Ccb->DeletePending = Delete;
            }
        }
        break;
    }
#ifdef FileDispositionInformationEx
    case FileDispositionInformationEx: {
        PFILE_DISPOSITION_INFORMATION_EX Info = Buffer;
        if (Length < sizeof(*Info))
            return InfilfsCompleteIrp(
                Irp, STATUS_BUFFER_TOO_SMALL, 0);
        {
            BOOLEAN Delete =
                (Info->Flags & FILE_DISPOSITION_DELETE) != 0;
            if (Delete != Ccb->DeletePending) {
                if (Delete)
                    InterlockedIncrement(&Fcb->DeletePendingCount);
                else
                    InterlockedDecrement(&Fcb->DeletePendingCount);
                Ccb->DeletePending = Delete;
            }
        }
        break;
    }
#endif
    case FileRenameInformation:
#ifdef FileRenameInformationEx
    case FileRenameInformationEx:
#endif
    {
        PFILE_RENAME_INFORMATION Info = Buffer;
        UNICODE_STRING Destination;
        PWCHAR AllocatedDestination = NULL;
        ULONG Replace = 0;
        if (Length < FIELD_OFFSET(FILE_RENAME_INFORMATION, FileName) ||
            Info->FileNameLength == 0 ||
            Info->FileNameLength >
                Length - FIELD_OFFSET(FILE_RENAME_INFORMATION, FileName))
            return InfilfsCompleteIrp(
                Irp, STATUS_INVALID_PARAMETER, 0);

        Status = InfilfsResolveSetTargetPath(
            Info->RootDirectory, Info->FileName, Info->FileNameLength,
            Irp->RequestorMode, &Destination, &AllocatedDestination);
        if (!NT_SUCCESS(Status))
            break;

#ifdef FileRenameInformationEx
        if (Class == FileRenameInformationEx) {
            PFILE_RENAME_INFORMATION_EX Ex = Buffer;
            Replace = (Ex->Flags & FILE_RENAME_REPLACE_IF_EXISTS) ?
                INFILFS_WIN_NATIVE_REQ_REPLACE : 0;
        } else
#endif
        {
            Replace = Info->ReplaceIfExists ?
                INFILFS_WIN_NATIVE_REQ_REPLACE : 0;
        }

        Status = InfilfsServiceMutation(
            Volume, INFILFS_WIN_NATIVE_OP_RENAME,
            &Fcb->Path, &Destination, 0, Replace);
        if (NT_SUCCESS(Status)) {
            USHORT Bytes = Destination.Length + sizeof(WCHAR);
            PWCHAR NewPath = ExAllocatePool2(
                POOL_FLAG_NON_PAGED, Bytes, INFILFS_NATIVE_FCB_TAG);
            if (!NewPath) {
                Status = STATUS_INSUFFICIENT_RESOURCES;
            } else {
                RtlCopyMemory(
                    NewPath, Destination.Buffer, Destination.Length);
                NewPath[Destination.Length / sizeof(WCHAR)] = L'\0';
                if (Fcb->Path.Buffer)
                    ExFreePoolWithTag(
                        Fcb->Path.Buffer, INFILFS_NATIVE_FCB_TAG);
                Fcb->Path.Buffer = NewPath;
                Fcb->Path.Length = Destination.Length;
                Fcb->Path.MaximumLength = Bytes;
            }
        }
        if (AllocatedDestination)
            ExFreePoolWithTag(
                AllocatedDestination, INFILFS_NATIVE_FCB_TAG);
        break;
    }
    case FileLinkInformation: {
        PFILE_LINK_INFORMATION Info = Buffer;
        UNICODE_STRING Destination;
        PWCHAR AllocatedDestination = NULL;
        ULONG Replace = 0;
        if (Fcb->ObjectType != INFILFS_WIN_NATIVE_OBJECT_FILE) {
            Status = STATUS_FILE_IS_A_DIRECTORY;
            break;
        }
        if (Length < FIELD_OFFSET(FILE_LINK_INFORMATION, FileName) ||
            Info->FileNameLength == 0 ||
            Info->FileNameLength >
                Length - FIELD_OFFSET(FILE_LINK_INFORMATION, FileName)) {
            Status = STATUS_INVALID_PARAMETER;
            break;
        }

        Status = InfilfsResolveSetTargetPath(
            Info->RootDirectory, Info->FileName, Info->FileNameLength,
            Irp->RequestorMode, &Destination, &AllocatedDestination);
        if (!NT_SUCCESS(Status))
            break;
        Replace = Info->ReplaceIfExists ?
            INFILFS_WIN_NATIVE_REQ_REPLACE : 0;
        Status = InfilfsServiceMutation(
            Volume, INFILFS_WIN_NATIVE_OP_LINK,
            &Fcb->Path, &Destination, 0, Replace);
        if (NT_SUCCESS(Status))
            Fcb->LinkCount++;
        if (AllocatedDestination)
            ExFreePoolWithTag(
                AllocatedDestination, INFILFS_NATIVE_FCB_TAG);
        break;
    }
    case FilePositionInformation: {
        PFILE_POSITION_INFORMATION Info = Buffer;
        if (Length < sizeof(*Info) || Info->CurrentByteOffset.QuadPart < 0)
            return InfilfsCompleteIrp(
                Irp, STATUS_INVALID_PARAMETER, 0);
        FileObject->CurrentByteOffset = Info->CurrentByteOffset;
        break;
    }
    default:
        Status = STATUS_INVALID_INFO_CLASS;
        break;
    }

    return InfilfsCompleteIrp(Irp, Status, 0);
}

static NTSTATUS InfilfsFormatDirectoryEntry(
    FILE_INFORMATION_CLASS Class, PUCHAR Buffer, ULONG Capacity,
    const struct infilfs_win_native_dirent *Source, ULONG FileIndex,
    ULONG *EntryBytesOut)
{
    ULONG NameBytes;
    ULONG EntryBytes;

    if (!Buffer || !Source || !EntryBytesOut)
        return STATUS_INVALID_PARAMETER;
    if (Source->name_chars >= INFILFS_WIN_NATIVE_NAME_CHARS)
        return STATUS_FILE_CORRUPT_ERROR;

    NameBytes = Source->name_chars * sizeof(WCHAR);
    switch (Class) {
    case FileDirectoryInformation: {
        PFILE_DIRECTORY_INFORMATION Entry;
        EntryBytes = FIELD_OFFSET(
            FILE_DIRECTORY_INFORMATION, FileName) + NameBytes;
        EntryBytes = (EntryBytes + 7u) & ~7u;
        if (EntryBytes > Capacity)
            return STATUS_BUFFER_OVERFLOW;
        Entry = (PFILE_DIRECTORY_INFORMATION)Buffer;
        RtlZeroMemory(Entry, EntryBytes);
        Entry->FileIndex = FileIndex;
        Entry->CreationTime.QuadPart =
            (LONGLONG)Source->attributes.creation_time_100ns;
        Entry->LastAccessTime.QuadPart =
            (LONGLONG)Source->attributes.access_time_100ns;
        Entry->LastWriteTime.QuadPart =
            (LONGLONG)Source->attributes.write_time_100ns;
        Entry->ChangeTime.QuadPart =
            (LONGLONG)Source->attributes.change_time_100ns;
        Entry->EndOfFile.QuadPart =
            (LONGLONG)Source->attributes.logical_size;
        Entry->AllocationSize.QuadPart =
            (LONGLONG)Source->attributes.allocation_size;
        Entry->FileAttributes = Source->attributes.file_attributes;
        Entry->FileNameLength = NameBytes;
        if (NameBytes)
            RtlCopyMemory(Entry->FileName, Source->name, NameBytes);
        break;
    }
    case FileFullDirectoryInformation: {
        PFILE_FULL_DIR_INFORMATION Entry;
        EntryBytes = FIELD_OFFSET(
            FILE_FULL_DIR_INFORMATION, FileName) + NameBytes;
        EntryBytes = (EntryBytes + 7u) & ~7u;
        if (EntryBytes > Capacity)
            return STATUS_BUFFER_OVERFLOW;
        Entry = (PFILE_FULL_DIR_INFORMATION)Buffer;
        RtlZeroMemory(Entry, EntryBytes);
        Entry->FileIndex = FileIndex;
        Entry->CreationTime.QuadPart =
            (LONGLONG)Source->attributes.creation_time_100ns;
        Entry->LastAccessTime.QuadPart =
            (LONGLONG)Source->attributes.access_time_100ns;
        Entry->LastWriteTime.QuadPart =
            (LONGLONG)Source->attributes.write_time_100ns;
        Entry->ChangeTime.QuadPart =
            (LONGLONG)Source->attributes.change_time_100ns;
        Entry->EndOfFile.QuadPart =
            (LONGLONG)Source->attributes.logical_size;
        Entry->AllocationSize.QuadPart =
            (LONGLONG)Source->attributes.allocation_size;
        Entry->FileAttributes = Source->attributes.file_attributes;
        Entry->FileNameLength = NameBytes;
        Entry->EaSize = 0;
        if (NameBytes)
            RtlCopyMemory(Entry->FileName, Source->name, NameBytes);
        break;
    }
    case FileBothDirectoryInformation: {
        PFILE_BOTH_DIR_INFORMATION Entry;
        EntryBytes = FIELD_OFFSET(
            FILE_BOTH_DIR_INFORMATION, FileName) + NameBytes;
        EntryBytes = (EntryBytes + 7u) & ~7u;
        if (EntryBytes > Capacity)
            return STATUS_BUFFER_OVERFLOW;
        Entry = (PFILE_BOTH_DIR_INFORMATION)Buffer;
        RtlZeroMemory(Entry, EntryBytes);
        Entry->FileIndex = FileIndex;
        Entry->CreationTime.QuadPart =
            (LONGLONG)Source->attributes.creation_time_100ns;
        Entry->LastAccessTime.QuadPart =
            (LONGLONG)Source->attributes.access_time_100ns;
        Entry->LastWriteTime.QuadPart =
            (LONGLONG)Source->attributes.write_time_100ns;
        Entry->ChangeTime.QuadPart =
            (LONGLONG)Source->attributes.change_time_100ns;
        Entry->EndOfFile.QuadPart =
            (LONGLONG)Source->attributes.logical_size;
        Entry->AllocationSize.QuadPart =
            (LONGLONG)Source->attributes.allocation_size;
        Entry->FileAttributes = Source->attributes.file_attributes;
        Entry->FileNameLength = NameBytes;
        Entry->EaSize = 0;
        Entry->ShortNameLength = 0;
        if (NameBytes)
            RtlCopyMemory(Entry->FileName, Source->name, NameBytes);
        break;
    }
    case FileNamesInformation: {
        PFILE_NAMES_INFORMATION Entry;
        EntryBytes = FIELD_OFFSET(
            FILE_NAMES_INFORMATION, FileName) + NameBytes;
        EntryBytes = (EntryBytes + 7u) & ~7u;
        if (EntryBytes > Capacity)
            return STATUS_BUFFER_OVERFLOW;
        Entry = (PFILE_NAMES_INFORMATION)Buffer;
        RtlZeroMemory(Entry, EntryBytes);
        Entry->FileIndex = FileIndex;
        Entry->FileNameLength = NameBytes;
        if (NameBytes)
            RtlCopyMemory(Entry->FileName, Source->name, NameBytes);
        break;
    }
    case FileIdFullDirectoryInformation: {
        PFILE_ID_FULL_DIR_INFORMATION Entry;
        EntryBytes = FIELD_OFFSET(
            FILE_ID_FULL_DIR_INFORMATION, FileName) + NameBytes;
        EntryBytes = (EntryBytes + 7u) & ~7u;
        if (EntryBytes > Capacity)
            return STATUS_BUFFER_OVERFLOW;
        Entry = (PFILE_ID_FULL_DIR_INFORMATION)Buffer;
        RtlZeroMemory(Entry, EntryBytes);
        Entry->FileIndex = FileIndex;
        Entry->CreationTime.QuadPart =
            (LONGLONG)Source->attributes.creation_time_100ns;
        Entry->LastAccessTime.QuadPart =
            (LONGLONG)Source->attributes.access_time_100ns;
        Entry->LastWriteTime.QuadPart =
            (LONGLONG)Source->attributes.write_time_100ns;
        Entry->ChangeTime.QuadPart =
            (LONGLONG)Source->attributes.change_time_100ns;
        Entry->EndOfFile.QuadPart =
            (LONGLONG)Source->attributes.logical_size;
        Entry->AllocationSize.QuadPart =
            (LONGLONG)Source->attributes.allocation_size;
        Entry->FileAttributes = Source->attributes.file_attributes;
        Entry->FileNameLength = NameBytes;
        Entry->EaSize = 0;
        Entry->FileId.QuadPart = (LONGLONG)Source->attributes.file_id;
        if (NameBytes)
            RtlCopyMemory(Entry->FileName, Source->name, NameBytes);
        break;
    }
    case FileIdBothDirectoryInformation: {
        PFILE_ID_BOTH_DIR_INFORMATION Entry;
        EntryBytes = FIELD_OFFSET(
            FILE_ID_BOTH_DIR_INFORMATION, FileName) + NameBytes;
        EntryBytes = (EntryBytes + 7u) & ~7u;
        if (EntryBytes > Capacity)
            return STATUS_BUFFER_OVERFLOW;
        Entry = (PFILE_ID_BOTH_DIR_INFORMATION)Buffer;
        RtlZeroMemory(Entry, EntryBytes);
        Entry->FileIndex = FileIndex;
        Entry->CreationTime.QuadPart =
            (LONGLONG)Source->attributes.creation_time_100ns;
        Entry->LastAccessTime.QuadPart =
            (LONGLONG)Source->attributes.access_time_100ns;
        Entry->LastWriteTime.QuadPart =
            (LONGLONG)Source->attributes.write_time_100ns;
        Entry->ChangeTime.QuadPart =
            (LONGLONG)Source->attributes.change_time_100ns;
        Entry->EndOfFile.QuadPart =
            (LONGLONG)Source->attributes.logical_size;
        Entry->AllocationSize.QuadPart =
            (LONGLONG)Source->attributes.allocation_size;
        Entry->FileAttributes = Source->attributes.file_attributes;
        Entry->FileNameLength = NameBytes;
        Entry->EaSize = 0;
        Entry->ShortNameLength = 0;
        Entry->FileId.QuadPart = (LONGLONG)Source->attributes.file_id;
        if (NameBytes)
            RtlCopyMemory(Entry->FileName, Source->name, NameBytes);
        break;
    }
    default:
        return STATUS_INVALID_INFO_CLASS;
    }

    *EntryBytesOut = EntryBytes;
    return STATUS_SUCCESS;
}

static NTSTATUS InfilfsDirectoryControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    INFILFS_NATIVE_VOLUME *Volume = InfilfsVolumeFromDevice(DeviceObject);
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    INFILFS_NATIVE_FCB *Fcb = FileObject ?
        (INFILFS_NATIVE_FCB *)FileObject->FsContext : NULL;
    INFILFS_NATIVE_CCB *Ccb = FileObject ?
        (INFILFS_NATIVE_CCB *)FileObject->FsContext2 : NULL;
    struct infilfs_win_native_request *Request = NULL;
    struct infilfs_win_native_response *Response = NULL;
    FILE_INFORMATION_CLASS Class;
    PUCHAR Output;
    PUCHAR Previous = NULL;
    ULONG OutputLength;
    ULONG Used = 0;
    BOOLEAN Matched = FALSE;
    NTSTATUS Status = STATUS_SUCCESS;

    if (!Volume || !Fcb || !Ccb ||
        Fcb->ObjectType != INFILFS_WIN_NATIVE_OBJECT_DIRECTORY)
        return InfilfsCompleteIrp(Irp, STATUS_NOT_A_DIRECTORY, 0);
    if (IrpSp->MinorFunction != IRP_MN_QUERY_DIRECTORY)
        return InfilfsCompleteIrp(
            Irp, STATUS_INVALID_DEVICE_REQUEST, 0);

    Class = IrpSp->Parameters.QueryDirectory.FileInformationClass;
    switch (Class) {
    case FileDirectoryInformation:
    case FileFullDirectoryInformation:
    case FileBothDirectoryInformation:
    case FileNamesInformation:
    case FileIdFullDirectoryInformation:
    case FileIdBothDirectoryInformation:
        break;
    default:
        return InfilfsCompleteIrp(
            Irp, STATUS_INVALID_INFO_CLASS, 0);
    }

    Output = (PUCHAR)InfilfsGetIrpBuffer(Irp);
    OutputLength = IrpSp->Parameters.QueryDirectory.Length;
    if (!Output || !OutputLength)
        return InfilfsCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
    if (IrpSp->Flags & SL_RESTART_SCAN)
        Ccb->DirectoryIndex = 0;

    Request = ExAllocatePool2(
        POOL_FLAG_PAGED, sizeof(*Request), INFILFS_NATIVE_REQUEST_TAG);
    Response = ExAllocatePool2(
        POOL_FLAG_PAGED, sizeof(*Response), INFILFS_NATIVE_REQUEST_TAG);
    if (!Request || !Response) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto out;
    }
    RtlZeroMemory(Request, sizeof(*Request));
    RtlZeroMemory(Response, sizeof(*Response));
    Request->opcode = INFILFS_WIN_NATIVE_OP_ENUMERATE;
    Request->offset = Ccb->DirectoryIndex;
    InfilfsCopyPathToRequest(Request, &Fcb->Path);
    Status = InfilfsCallService(Volume, Request, Response);
    if (!NT_SUCCESS(Status))
        goto out;

    for (ULONG i = 0; i < Response->entry_count; ++i) {
        SIZE_T Offset =
            (SIZE_T)i * sizeof(struct infilfs_win_native_dirent);
        struct infilfs_win_native_dirent *Source;
        ULONG EntryBytes = 0;
        ULONG NameBytes;
        UNICODE_STRING Name;
        PUCHAR Entry;

        if (Offset + sizeof(*Source) > Response->output_bytes)
            break;
        Source = (struct infilfs_win_native_dirent *)
            (Response->output + Offset);
        if (Source->name_chars >= INFILFS_WIN_NATIVE_NAME_CHARS) {
            Status = STATUS_FILE_CORRUPT_ERROR;
            break;
        }
        NameBytes = Source->name_chars * sizeof(WCHAR);
        Name.Buffer = (PWCHAR)Source->name;
        Name.Length = (USHORT)NameBytes;
        Name.MaximumLength = Name.Length;

        if (IrpSp->Parameters.QueryDirectory.FileName &&
            !FsRtlIsNameInExpression(
                IrpSp->Parameters.QueryDirectory.FileName,
                &Name, TRUE, NULL)) {
            Ccb->DirectoryIndex++;
            continue;
        }

        Matched = TRUE;
        Entry = Output + Used;
        Status = InfilfsFormatDirectoryEntry(
            Class, Entry, OutputLength - Used, Source,
            Ccb->DirectoryIndex, &EntryBytes);
        if (Status == STATUS_BUFFER_OVERFLOW) {
            if (Used)
                Status = STATUS_SUCCESS;
            break;
        }
        if (!NT_SUCCESS(Status))
            break;

        if (Previous)
            *(PULONG)Previous = (ULONG)(Entry - Previous);
        Previous = Entry;
        Used += EntryBytes;
        Ccb->DirectoryIndex++;
        if (IrpSp->Flags & SL_RETURN_SINGLE_ENTRY)
            break;
    }

    if (NT_SUCCESS(Status) && !Used)
        Status = Matched ? STATUS_BUFFER_OVERFLOW :
            (Ccb->DirectoryIndex == 0 ?
                STATUS_NO_SUCH_FILE : STATUS_NO_MORE_FILES);
out:
    if (Response)
        ExFreePoolWithTag(Response, INFILFS_NATIVE_REQUEST_TAG);
    if (Request)
        ExFreePoolWithTag(Request, INFILFS_NATIVE_REQUEST_TAG);
    return InfilfsCompleteIrp(Irp, Status, Used);
}

static NTSTATUS InfilfsQueryVolumeInformation(
    PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    INFILFS_NATIVE_VOLUME *Volume = InfilfsVolumeFromDevice(DeviceObject);
    PVOID Buffer = Irp->AssociatedIrp.SystemBuffer;
    ULONG Length = IrpSp->Parameters.QueryVolume.Length;
    FS_INFORMATION_CLASS Class =
        IrpSp->Parameters.QueryVolume.FsInformationClass;
    ULONG_PTR Used = 0;
    NTSTATUS Status = STATUS_SUCCESS;

    if (!Volume || !Buffer)
        return InfilfsCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);

    switch (Class) {
    case FileFsVolumeInformation: {
        PFILE_FS_VOLUME_INFORMATION Info = Buffer;
        struct infilfs_win_native_volume_state State;
        WCHAR Label[INFILFS_WIN_NATIVE_LABEL_BYTES + 1u];
        ULONG LabelBytes = 0;
        ULONG Required;
        ULONG Copy;

        Status = InfilfsQueryPortableVolumeState(Volume, &State);
        if (!NT_SUCCESS(Status))
            break;
        RtlZeroMemory(Label, sizeof(Label));
        if (State.label_bytes) {
            Status = RtlUTF8ToUnicodeN(
                Label, sizeof(Label) - sizeof(WCHAR), &LabelBytes,
                (PCCH)State.label, State.label_bytes);
            if (!NT_SUCCESS(Status)) {
                Status = STATUS_FILE_CORRUPT_ERROR;
                break;
            }
        }
        Required = FIELD_OFFSET(
            FILE_FS_VOLUME_INFORMATION, VolumeLabel) + LabelBytes;
        if (Length < FIELD_OFFSET(
                FILE_FS_VOLUME_INFORMATION, VolumeLabel)) {
            Status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        RtlZeroMemory(Info, Length);
        Info->VolumeSerialNumber = InfilfsVolumeSerial(&State);
        Info->SupportsObjects = FALSE;
        Info->VolumeLabelLength = LabelBytes;
        Copy = Length -
            FIELD_OFFSET(FILE_FS_VOLUME_INFORMATION, VolumeLabel);
        if (Copy > LabelBytes)
            Copy = LabelBytes;
        if (Copy)
            RtlCopyMemory(Info->VolumeLabel, Label, Copy);
        Used = FIELD_OFFSET(
            FILE_FS_VOLUME_INFORMATION, VolumeLabel) + Copy;
        if (Length < Required)
            Status = STATUS_BUFFER_OVERFLOW;
        break;
    }
    case FileFsSizeInformation: {
        PFILE_FS_SIZE_INFORMATION Info = Buffer;
        struct infilfs_win_native_volume_state State;
        if (Length < sizeof(*Info)) {
            Status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        Status = InfilfsQueryPortableVolumeState(Volume, &State);
        if (!NT_SUCCESS(Status))
            break;
        RtlZeroMemory(Info, sizeof(*Info));
        Info->TotalAllocationUnits.QuadPart = State.total_blocks;
        Info->AvailableAllocationUnits.QuadPart = State.free_blocks;
        Info->SectorsPerAllocationUnit =
            INFILFS_NATIVE_BLOCK_SIZE / Volume->SectorSize;
        Info->BytesPerSector = Volume->SectorSize;
        Used = sizeof(*Info);
        break;
    }
    case FileFsFullSizeInformation: {
        PFILE_FS_FULL_SIZE_INFORMATION Info = Buffer;
        struct infilfs_win_native_volume_state State;
        if (Length < sizeof(*Info)) {
            Status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        Status = InfilfsQueryPortableVolumeState(Volume, &State);
        if (!NT_SUCCESS(Status))
            break;
        RtlZeroMemory(Info, sizeof(*Info));
        Info->TotalAllocationUnits.QuadPart = State.total_blocks;
        Info->CallerAvailableAllocationUnits.QuadPart = State.free_blocks;
        Info->ActualAvailableAllocationUnits.QuadPart = State.free_blocks;
        Info->SectorsPerAllocationUnit =
            INFILFS_NATIVE_BLOCK_SIZE / Volume->SectorSize;
        Info->BytesPerSector = Volume->SectorSize;
        Used = sizeof(*Info);
        break;
    }
    case FileFsDeviceInformation: {
        PFILE_FS_DEVICE_INFORMATION Info = Buffer;
        if (Length < sizeof(*Info)) {
            Status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        Info->DeviceType = FILE_DEVICE_DISK_FILE_SYSTEM;
        Info->Characteristics = DeviceObject->Characteristics;
        if (Volume->ReadOnly)
            Info->Characteristics |= FILE_READ_ONLY_DEVICE;
        Used = sizeof(*Info);
        break;
    }
    case FileFsAttributeInformation: {
        PFILE_FS_ATTRIBUTE_INFORMATION Info = Buffer;
        static const WCHAR Name[] = L"InfiltratorFS";
        ULONG NameBytes = sizeof(Name) - sizeof(WCHAR);
        ULONG Required = FIELD_OFFSET(
            FILE_FS_ATTRIBUTE_INFORMATION, FileSystemName) + NameBytes;
        if (Length < FIELD_OFFSET(
                FILE_FS_ATTRIBUTE_INFORMATION, FileSystemName)) {
            Status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        RtlZeroMemory(Info, Length);
        /*
         * Advertise only Windows contracts implemented by this FSD today.
         * Portable sparse/reparse-like primitives do not make the corresponding
         * Windows FSCTL contracts true until those FSCTLs are wired end to end.
         */
        Info->FileSystemAttributes =
            FILE_CASE_PRESERVED_NAMES |
            FILE_UNICODE_ON_DISK |
            FILE_PERSISTENT_ACLS |
            FILE_SUPPORTS_HARD_LINKS;
        Info->MaximumComponentNameLength = 1023;
        Info->FileSystemNameLength = NameBytes;
        ULONG Copy = Length -
            FIELD_OFFSET(FILE_FS_ATTRIBUTE_INFORMATION, FileSystemName);
        if (Copy > NameBytes)
            Copy = NameBytes;
        RtlCopyMemory(Info->FileSystemName, Name, Copy);
        Used = FIELD_OFFSET(
            FILE_FS_ATTRIBUTE_INFORMATION, FileSystemName) + Copy;
        if (Length < Required)
            Status = STATUS_BUFFER_OVERFLOW;
        break;
    }
    default:
        Status = STATUS_INVALID_INFO_CLASS;
        break;
    }

    return InfilfsCompleteIrp(Irp, Status, Used);
}

static NTSTATUS InfilfsQuerySecurity(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    INFILFS_NATIVE_VOLUME *Volume = InfilfsVolumeFromDevice(DeviceObject);
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    INFILFS_NATIVE_FCB *Fcb = FileObject ?
        (INFILFS_NATIVE_FCB *)FileObject->FsContext : NULL;
    struct infilfs_win_native_request *Request = NULL;
    struct infilfs_win_native_response *Response = NULL;
    PVOID Output;
    ULONG Capacity;
    NTSTATUS Status;

    if (!Volume || !Fcb)
        return InfilfsCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);

    Capacity = IrpSp->Parameters.QuerySecurity.Length;
    Output = Irp->UserBuffer;
    if (!Output && Capacity)
        return InfilfsCompleteIrp(Irp, STATUS_INVALID_USER_BUFFER, 0);

    Request = ExAllocatePool2(
        POOL_FLAG_PAGED, sizeof(*Request), INFILFS_NATIVE_REQUEST_TAG);
    Response = ExAllocatePool2(
        POOL_FLAG_PAGED, sizeof(*Response), INFILFS_NATIVE_REQUEST_TAG);
    if (!Request || !Response) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto out;
    }

    RtlZeroMemory(Request, sizeof(*Request));
    RtlZeroMemory(Response, sizeof(*Response));
    Request->opcode = INFILFS_WIN_NATIVE_OP_QUERY_SECURITY;
    Request->desired_access =
        (ULONG)IrpSp->Parameters.QuerySecurity.SecurityInformation;
    InfilfsCopyPathToRequest(Request, &Fcb->Path);
    Status = InfilfsCallService(Volume, Request, Response);
    if (!NT_SUCCESS(Status))
        goto out;

    if (Response->output_bytes > sizeof(Response->output)) {
        Status = STATUS_DATA_ERROR;
        goto out;
    }

    Irp->IoStatus.Information = Response->output_bytes;
    if (Capacity < Response->output_bytes) {
        Status = STATUS_BUFFER_TOO_SMALL;
        goto out;
    }

    if (Response->output_bytes) {
        __try {
            ProbeForWrite(Output, Response->output_bytes, sizeof(UCHAR));
            RtlCopyMemory(Output, Response->output, Response->output_bytes);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Status = GetExceptionCode();
            goto out;
        }
    }
    Status = STATUS_SUCCESS;

out:
    if (Response)
        ExFreePoolWithTag(Response, INFILFS_NATIVE_REQUEST_TAG);
    if (Request)
        ExFreePoolWithTag(Request, INFILFS_NATIVE_REQUEST_TAG);
    return InfilfsCompleteIrp(
        Irp, Status,
        Status == STATUS_SUCCESS ? Irp->IoStatus.Information :
        (Status == STATUS_BUFFER_TOO_SMALL ? Irp->IoStatus.Information : 0));
}

static NTSTATUS InfilfsSetSecurity(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    INFILFS_NATIVE_VOLUME *Volume = InfilfsVolumeFromDevice(DeviceObject);
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    INFILFS_NATIVE_FCB *Fcb = FileObject ?
        (INFILFS_NATIVE_FCB *)FileObject->FsContext : NULL;
    PSECURITY_DESCRIPTOR Descriptor =
        IrpSp->Parameters.SetSecurity.SecurityDescriptor;
    struct infilfs_win_native_request *Request = NULL;
    struct infilfs_win_native_response *Response = NULL;
    ULONG Bytes;
    NTSTATUS Status;

    if (!Volume || !Fcb || !Descriptor)
        return InfilfsCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
    if (Volume->ReadOnly)
        return InfilfsCompleteIrp(Irp, STATUS_MEDIA_WRITE_PROTECTED, 0);

    __try {
        if (!RtlValidSecurityDescriptor(Descriptor))
            return InfilfsCompleteIrp(
                Irp, STATUS_INVALID_SECURITY_DESCR, 0);
        Bytes = RtlLengthSecurityDescriptor(Descriptor);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return InfilfsCompleteIrp(Irp, GetExceptionCode(), 0);
    }
    if (!Bytes || Bytes > INFILFS_WIN_NATIVE_IO_CHUNK)
        return InfilfsCompleteIrp(Irp, STATUS_BUFFER_OVERFLOW, 0);

    Request = ExAllocatePool2(
        POOL_FLAG_PAGED, sizeof(*Request), INFILFS_NATIVE_REQUEST_TAG);
    Response = ExAllocatePool2(
        POOL_FLAG_PAGED, sizeof(*Response), INFILFS_NATIVE_REQUEST_TAG);
    if (!Request || !Response) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto out;
    }

    RtlZeroMemory(Request, sizeof(*Request));
    RtlZeroMemory(Response, sizeof(*Response));
    Request->opcode = INFILFS_WIN_NATIVE_OP_SET_SECURITY;
    Request->desired_access =
        (ULONG)IrpSp->Parameters.SetSecurity.SecurityInformation;
    Request->input_bytes = Bytes;
    InfilfsCopyPathToRequest(Request, &Fcb->Path);
    __try {
        RtlCopyMemory(Request->input, Descriptor, Bytes);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Status = GetExceptionCode();
        goto out;
    }

    Status = InfilfsCallService(Volume, Request, Response);

out:
    if (Response)
        ExFreePoolWithTag(Response, INFILFS_NATIVE_REQUEST_TAG);
    if (Request)
        ExFreePoolWithTag(Request, INFILFS_NATIVE_REQUEST_TAG);
    return InfilfsCompleteIrp(Irp, Status, 0);
}

static NTSTATUS InfilfsFlushBuffers(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    INFILFS_NATIVE_VOLUME *Volume = InfilfsVolumeFromDevice(DeviceObject);
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    INFILFS_NATIVE_FCB *Fcb = FileObject ?
        (INFILFS_NATIVE_FCB *)FileObject->FsContext : NULL;
    NTSTATUS Status;

    if (!Volume)
        return InfilfsCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    if (Fcb && FileObject->SectionObjectPointer)
        CcFlushCache(
            FileObject->SectionObjectPointer, NULL, 0, &Irp->IoStatus);

    Status = InfilfsFlushPortableVolume(
        Volume, Fcb ? &Fcb->Path : NULL);
    return InfilfsCompleteIrp(Irp, Status, 0);
}

static NTSTATUS InfilfsLockControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    INFILFS_NATIVE_FCB *Fcb = FileObject ?
        (INFILFS_NATIVE_FCB *)FileObject->FsContext : NULL;
    UNREFERENCED_PARAMETER(DeviceObject);

    if (!Fcb || Fcb->ObjectType != INFILFS_WIN_NATIVE_OBJECT_FILE)
        return InfilfsCompleteIrp(
            Irp, STATUS_INVALID_DEVICE_REQUEST, 0);

    /*
     * FsRtl owns completion for IRP_MJ_LOCK_CONTROL. Keeping the FILE_LOCK on
     * the shared FCB makes overlapping opens observe one Windows byte-range
     * lock namespace rather than one lock set per handle.
     */
    return FsRtlProcessFileLock(&Fcb->FileLock, Irp, NULL);
}

static NTSTATUS InfilfsCleanup(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    INFILFS_NATIVE_VOLUME *Volume = InfilfsVolumeFromDevice(DeviceObject);
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    INFILFS_NATIVE_FCB *Fcb = FileObject ?
        (INFILFS_NATIVE_FCB *)FileObject->FsContext : NULL;
    INFILFS_NATIVE_CCB *Ccb = FileObject ?
        (INFILFS_NATIVE_CCB *)FileObject->FsContext2 : NULL;
    NTSTATUS Status = STATUS_SUCCESS;

    if (!FileObject)
        return InfilfsCompleteIrp(Irp, STATUS_SUCCESS, 0);

    if (Fcb && Fcb->ObjectType == INFILFS_WIN_NATIVE_OBJECT_FILE) {
        (void)FsRtlFastUnlockAll(
            &Fcb->FileLock, FileObject,
            IoGetRequestorProcess(Irp), NULL);
    }

    if (FileObject->SectionObjectPointer)
        CcFlushCache(FileObject->SectionObjectPointer, NULL, 0, NULL);
    CcUninitializeCacheMap(FileObject, NULL, NULL);

    if (Ccb && Ccb->ShareRegistered && Fcb) {
        IoRemoveShareAccess(FileObject, &Fcb->ShareAccess);
        Ccb->ShareRegistered = FALSE;
    }

    if (Volume && Fcb && Ccb && Ccb->DeletePending) {
        Status = InfilfsServiceMutation(
            Volume,
            Fcb->ObjectType == INFILFS_WIN_NATIVE_OBJECT_DIRECTORY ?
                INFILFS_WIN_NATIVE_OP_RMDIR :
                INFILFS_WIN_NATIVE_OP_UNLINK,
            &Fcb->Path, NULL, 0, 0);
        if (NT_SUCCESS(Status))
            FileObject->DeletePending = TRUE;
        InterlockedDecrement(&Fcb->DeletePendingCount);
        Ccb->DeletePending = FALSE;
    }
    return InfilfsCompleteIrp(Irp, Status, 0);
}

static NTSTATUS InfilfsClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    INFILFS_NATIVE_FCB *Fcb;
    INFILFS_NATIVE_CCB *Ccb;
    UNREFERENCED_PARAMETER(DeviceObject);

    if (!FileObject)
        return InfilfsCompleteIrp(Irp, STATUS_SUCCESS, 0);
    Fcb = (INFILFS_NATIVE_FCB *)FileObject->FsContext;
    Ccb = (INFILFS_NATIVE_CCB *)FileObject->FsContext2;
    FileObject->FsContext = NULL;
    FileObject->FsContext2 = NULL;
    if (Ccb && Ccb->ShareRegistered && Fcb) {
        IoRemoveShareAccess(FileObject, &Fcb->ShareAccess);
        Ccb->ShareRegistered = FALSE;
    }
    if (Ccb && Ccb->DeletePending && Fcb) {
        InterlockedDecrement(&Fcb->DeletePendingCount);
        Ccb->DeletePending = FALSE;
    }
    if (Ccb)
        ExFreePoolWithTag(Ccb, INFILFS_NATIVE_CCB_TAG);
    if (Fcb)
        InfilfsDereferenceFcb(Fcb);
    return InfilfsCompleteIrp(Irp, STATUS_SUCCESS, 0);
}

static BOOLEAN InfilfsAcquireForLazyWrite(
    PVOID Context, BOOLEAN Wait)
{
    INFILFS_NATIVE_FCB *Fcb = (INFILFS_NATIVE_FCB *)Context;
    return ExAcquireResourceExclusiveLite(&Fcb->MainResource, Wait);
}

static VOID InfilfsReleaseFromLazyWrite(PVOID Context)
{
    INFILFS_NATIVE_FCB *Fcb = (INFILFS_NATIVE_FCB *)Context;
    ExReleaseResourceLite(&Fcb->MainResource);
}

static BOOLEAN InfilfsAcquireForReadAhead(
    PVOID Context, BOOLEAN Wait)
{
    INFILFS_NATIVE_FCB *Fcb = (INFILFS_NATIVE_FCB *)Context;
    return ExAcquireResourceSharedLite(&Fcb->MainResource, Wait);
}

static VOID InfilfsReleaseFromReadAhead(PVOID Context)
{
    INFILFS_NATIVE_FCB *Fcb = (INFILFS_NATIVE_FCB *)Context;
    ExReleaseResourceLite(&Fcb->MainResource);
}

static const CACHE_MANAGER_CALLBACKS g_InfilfsCacheCallbacks = {
    InfilfsAcquireForLazyWrite,
    InfilfsReleaseFromLazyWrite,
    InfilfsAcquireForReadAhead,
    InfilfsReleaseFromReadAhead
};

static NTSTATUS InfilfsShutdown(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    INFILFS_NATIVE_VOLUME **Volumes = NULL;
    PLIST_ENTRY Entry;
    ULONG Count = 0;
    ULONG Index = 0;
    NTSTATUS Result = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(DeviceObject);

    /*
     * Never hold VolumeLock while calling the service. infs_volume_sync()
     * reaches back through RAW_FLUSH, whose control path resolves the volume
     * under this same lock. Mounted volume objects are not destroyed until
     * driver unload, so a pointer snapshot is stable for this shutdown pass.
     */
    ExAcquireFastMutex(&g_Infilfs.VolumeLock);
    for (Entry = g_Infilfs.Volumes.Flink;
         Entry != &g_Infilfs.Volumes; Entry = Entry->Flink)
        Count++;
    if (Count)
        Volumes = ExAllocatePool2(
            POOL_FLAG_PAGED,
            sizeof(*Volumes) * Count,
            INFILFS_NATIVE_REQUEST_TAG);
    if (Count && !Volumes) {
        ExReleaseFastMutex(&g_Infilfs.VolumeLock);
        return InfilfsCompleteIrp(
            Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    }
    for (Entry = g_Infilfs.Volumes.Flink;
         Entry != &g_Infilfs.Volumes && Index < Count;
         Entry = Entry->Flink) {
        Volumes[Index++] = CONTAINING_RECORD(
            Entry, INFILFS_NATIVE_VOLUME, GlobalLink);
    }
    ExReleaseFastMutex(&g_Infilfs.VolumeLock);

    for (Index = 0; Index < Count; ++Index) {
        INFILFS_NATIVE_VOLUME *Volume = Volumes[Index];
        struct infilfs_win_native_request *Request;
        struct infilfs_win_native_response *Response;
        NTSTATUS Status;

        if (!Volume || Volume->Dismounted)
            continue;

        Request = ExAllocatePool2(
            POOL_FLAG_PAGED, sizeof(*Request),
            INFILFS_NATIVE_REQUEST_TAG);
        Response = ExAllocatePool2(
            POOL_FLAG_PAGED, sizeof(*Response),
            INFILFS_NATIVE_REQUEST_TAG);
        if (!Request || !Response) {
            if (Request)
                ExFreePoolWithTag(
                    Request, INFILFS_NATIVE_REQUEST_TAG);
            if (Response)
                ExFreePoolWithTag(
                    Response, INFILFS_NATIVE_REQUEST_TAG);
            if (NT_SUCCESS(Result))
                Result = STATUS_INSUFFICIENT_RESOURCES;
            continue;
        }

        RtlZeroMemory(Request, sizeof(*Request));
        RtlZeroMemory(Response, sizeof(*Response));
        Request->opcode = INFILFS_WIN_NATIVE_OP_FLUSH;
        Status = InfilfsCallService(Volume, Request, Response);
        if (NT_SUCCESS(Status))
            Status = InfilfsTargetFlush(Volume->TargetDevice);
        if (!NT_SUCCESS(Status) && NT_SUCCESS(Result))
            Result = Status;
        ExFreePoolWithTag(Response, INFILFS_NATIVE_REQUEST_TAG);
        ExFreePoolWithTag(Request, INFILFS_NATIVE_REQUEST_TAG);
    }

    if (Volumes)
        ExFreePoolWithTag(Volumes, INFILFS_NATIVE_REQUEST_TAG);
    return InfilfsCompleteIrp(Irp, Result, 0);
}

static NTSTATUS InfilfsDefaultDispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    return InfilfsCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

static VOID InfilfsAbortServiceRequests(VOID)
{
    PLIST_ENTRY Entry;

    ExAcquireFastMutex(&g_Infilfs.QueueLock);
    while (!IsListEmpty(&g_Infilfs.PendingRequests)) {
        INFILFS_NATIVE_REQUEST_ITEM *Item;
        Entry = RemoveHeadList(&g_Infilfs.PendingRequests);
        Item = CONTAINING_RECORD(
            Entry, INFILFS_NATIVE_REQUEST_ITEM, Link);
        InitializeListHead(&Item->Link);
        KeSetEvent(&Item->CompletionEvent, IO_NO_INCREMENT, FALSE);
    }
    while (!IsListEmpty(&g_Infilfs.ActiveRequests)) {
        INFILFS_NATIVE_REQUEST_ITEM *Item;
        Entry = RemoveHeadList(&g_Infilfs.ActiveRequests);
        Item = CONTAINING_RECORD(
            Entry, INFILFS_NATIVE_REQUEST_ITEM, Link);
        InitializeListHead(&Item->Link);
        KeSetEvent(&Item->CompletionEvent, IO_NO_INCREMENT, FALSE);
    }
    ExReleaseFastMutex(&g_Infilfs.QueueLock);
}

static VOID InfilfsUnload(PDRIVER_OBJECT DriverObject)
{
    PLIST_ENTRY Entry;
    UNREFERENCED_PARAMETER(DriverObject);
    InterlockedExchange(&g_Infilfs.Unloading, 1);
    InfilfsAbortServiceRequests();

    if (g_Infilfs.FileSystemDevice) {
        IoUnregisterFileSystem(g_Infilfs.FileSystemDevice);
        IoUnregisterShutdownNotification(g_Infilfs.FileSystemDevice);
    }

    ExAcquireFastMutex(&g_Infilfs.VolumeLock);
    while (!IsListEmpty(&g_Infilfs.Volumes)) {
        INFILFS_NATIVE_VOLUME *Volume;
        Entry = RemoveHeadList(&g_Infilfs.Volumes);
        Volume = CONTAINING_RECORD(
            Entry, INFILFS_NATIVE_VOLUME, GlobalLink);
        if (Volume->Vpb) {
            Volume->Vpb->Flags &= ~VPB_MOUNTED;
            Volume->Vpb->DeviceObject = NULL;
        }
        if (Volume->TargetDevice)
            ObDereferenceObject(Volume->TargetDevice);
        ExAcquireFastMutex(&Volume->FcbLock);
        while (!IsListEmpty(&Volume->Fcbs)) {
            INFILFS_NATIVE_FCB *Fcb =
                CONTAINING_RECORD(
                    RemoveHeadList(&Volume->Fcbs),
                    INFILFS_NATIVE_FCB, VolumeLink);
            InitializeListHead(&Fcb->VolumeLink);
            ExReleaseFastMutex(&Volume->FcbLock);
            InfilfsFreeFcb(Fcb);
            ExAcquireFastMutex(&Volume->FcbLock);
        }
        ExReleaseFastMutex(&Volume->FcbLock);
        ExDeleteResourceLite(&Volume->Resource);
        IoDeleteDevice(Volume->DeviceObject);
    }
    ExReleaseFastMutex(&g_Infilfs.VolumeLock);

    if (g_Infilfs.ControlDosName.Buffer)
        IoDeleteSymbolicLink(&g_Infilfs.ControlDosName);
    if (g_Infilfs.ControlDevice)
        IoDeleteDevice(g_Infilfs.ControlDevice);
    if (g_Infilfs.FileSystemDevice)
        IoDeleteDevice(g_Infilfs.FileSystemDevice);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    UNICODE_STRING FsName;
    UNICODE_STRING ControlName;
    UNICODE_STRING DosName;
    NTSTATUS Status;
    ULONG i;

    UNREFERENCED_PARAMETER(RegistryPath);
    RtlZeroMemory(&g_Infilfs, sizeof(g_Infilfs));
    g_Infilfs.DriverObject = DriverObject;
    InitializeListHead(&g_Infilfs.PendingRequests);
    InitializeListHead(&g_Infilfs.ActiveRequests);
    InitializeListHead(&g_Infilfs.Volumes);
    ExInitializeFastMutex(&g_Infilfs.QueueLock);
    ExInitializeFastMutex(&g_Infilfs.VolumeLock);
    KeInitializeSemaphore(&g_Infilfs.PendingSemaphore, 0, MAXLONG);

    RtlInitUnicodeString(&FsName, L"\\FileSystem\\InfiltratorFS");
    Status = IoCreateDevice(
        DriverObject, 0, &FsName, FILE_DEVICE_DISK_FILE_SYSTEM,
        0, FALSE, &g_Infilfs.FileSystemDevice);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlInitUnicodeString(
        &ControlName, L"\\Device\\InfiltratorFSControl");
    Status = IoCreateDevice(
        DriverObject, 0, &ControlName, INFILFS_WIN_NATIVE_DEVICE_TYPE,
        FILE_DEVICE_SECURE_OPEN, FALSE, &g_Infilfs.ControlDevice);
    if (!NT_SUCCESS(Status)) {
        IoDeleteDevice(g_Infilfs.FileSystemDevice);
        g_Infilfs.FileSystemDevice = NULL;
        return Status;
    }

    RtlInitUnicodeString(&DosName, L"\\DosDevices\\InfiltratorFSControl");
    g_Infilfs.ControlDosName = DosName;
    Status = IoCreateSymbolicLink(&DosName, &ControlName);
    if (!NT_SUCCESS(Status)) {
        IoDeleteDevice(g_Infilfs.ControlDevice);
        IoDeleteDevice(g_Infilfs.FileSystemDevice);
        g_Infilfs.ControlDevice = NULL;
        g_Infilfs.FileSystemDevice = NULL;
        return Status;
    }

    for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; ++i)
        DriverObject->MajorFunction[i] = InfilfsDefaultDispatch;
    DriverObject->MajorFunction[IRP_MJ_CREATE] = InfilfsCreate;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = InfilfsClose;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = InfilfsCleanup;
    DriverObject->MajorFunction[IRP_MJ_READ] = InfilfsRead;
    DriverObject->MajorFunction[IRP_MJ_WRITE] = InfilfsWrite;
    DriverObject->MajorFunction[IRP_MJ_QUERY_INFORMATION] =
        InfilfsQueryInformation;
    DriverObject->MajorFunction[IRP_MJ_SET_INFORMATION] =
        InfilfsSetInformation;
    DriverObject->MajorFunction[IRP_MJ_DIRECTORY_CONTROL] =
        InfilfsDirectoryControl;
    DriverObject->MajorFunction[IRP_MJ_QUERY_VOLUME_INFORMATION] =
        InfilfsQueryVolumeInformation;
    DriverObject->MajorFunction[IRP_MJ_QUERY_SECURITY] =
        InfilfsQuerySecurity;
    DriverObject->MajorFunction[IRP_MJ_SET_SECURITY] =
        InfilfsSetSecurity;
    DriverObject->MajorFunction[IRP_MJ_LOCK_CONTROL] =
        InfilfsLockControl;
    DriverObject->MajorFunction[IRP_MJ_FLUSH_BUFFERS] = InfilfsFlushBuffers;
    DriverObject->MajorFunction[IRP_MJ_FILE_SYSTEM_CONTROL] =
        InfilfsFileSystemControl;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] =
        InfilfsControlDeviceIo;
    DriverObject->MajorFunction[IRP_MJ_SHUTDOWN] =
        InfilfsShutdown;
    DriverObject->DriverUnload = InfilfsUnload;

    g_Infilfs.FileSystemDevice->Flags |= DO_DIRECT_IO;
    g_Infilfs.FileSystemDevice->Flags &= ~DO_DEVICE_INITIALIZING;
    g_Infilfs.ControlDevice->Flags |= DO_BUFFERED_IO;
    g_Infilfs.ControlDevice->Flags &= ~DO_DEVICE_INITIALIZING;

    Status = IoRegisterShutdownNotification(
        g_Infilfs.FileSystemDevice);
    if (!NT_SUCCESS(Status)) {
        IoDeleteSymbolicLink(&g_Infilfs.ControlDosName);
        IoDeleteDevice(g_Infilfs.ControlDevice);
        IoDeleteDevice(g_Infilfs.FileSystemDevice);
        g_Infilfs.ControlDevice = NULL;
        g_Infilfs.FileSystemDevice = NULL;
        return Status;
    }

    IoRegisterFileSystem(g_Infilfs.FileSystemDevice);
    return STATUS_SUCCESS;
}
