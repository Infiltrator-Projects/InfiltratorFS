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
    ERESOURCE Resource;
    LIST_ENTRY GlobalLink;
} INFILFS_NATIVE_VOLUME;

typedef struct _INFILFS_NATIVE_FCB {
    ULONG Signature;
    FSRTL_ADVANCED_FCB_HEADER Header;
    FAST_MUTEX HeaderMutex;
    SECTION_OBJECT_POINTERS SectionObjectPointers;
    ERESOURCE MainResource;
    ERESOURCE PagingResource;
    volatile LONG References;
    ULONGLONG FileId;
    ULONG ObjectType;
    ULONG FileAttributes;
    UNICODE_STRING Path;
    BOOLEAN DeletePending;
} INFILFS_NATIVE_FCB;

typedef struct _INFILFS_NATIVE_CCB {
    ULONG Signature;
    ULONG DirectoryIndex;
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
        if (Volume->VolumeId == VolumeId) {
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
    NTSTATUS Status;

    if (!Volume || !Request || !Response || g_Infilfs.Unloading)
        return STATUS_DEVICE_NOT_READY;

    Item = ExAllocatePool2(
        POOL_FLAG_PAGED, sizeof(*Item), INFILFS_NATIVE_REQUEST_TAG);
    if (!Item)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(Item, sizeof(*Item));
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

    ExAcquireFastMutex(&g_Infilfs.QueueLock);
    if (!IsListEmpty(&Item->Link))
        RemoveEntryList(&Item->Link);
    InitializeListHead(&Item->Link);
    ExReleaseFastMutex(&g_Infilfs.QueueLock);

    if (Status == STATUS_TIMEOUT)
        Status = STATUS_DEVICE_NOT_READY;
    else if (NT_SUCCESS(Status) && !Item->Completed)
        Status = STATUS_DEVICE_NOT_READY;
    else if (NT_SUCCESS(Status))
        Status = InfilfsStatusFromPortable(Response->status);

    ExFreePoolWithTag(Item, INFILFS_NATIVE_REQUEST_TAG);
    return Status;
}

static NTSTATUS InfilfsControlWaitRequest(PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
    INFILFS_NATIVE_REQUEST_ITEM *Item = NULL;
    PLIST_ENTRY Entry;
    NTSTATUS Status;

    if (IrpSp->Parameters.DeviceIoControl.OutputBufferLength <
        sizeof(struct infilfs_win_native_request))
        return InfilfsCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);

    Status = KeWaitForSingleObject(
        &g_Infilfs.PendingSemaphore, Executive, KernelMode, FALSE, NULL);
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
            break;
        }
    }
    ExReleaseFastMutex(&g_Infilfs.QueueLock);

    if (!Item)
        return InfilfsCompleteIrp(Irp, STATUS_NOT_FOUND, 0);
    KeSetEvent(&Item->CompletionEvent, IO_NO_INCREMENT, FALSE);
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
    Volume->ReadOnly = FALSE;
    ExInitializeResourceLite(&Volume->Resource);
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

static NTSTATUS InfilfsFileSystemControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);

    if (InfilfsIsFileSystemDevice(DeviceObject) &&
        IrpSp->MinorFunction == IRP_MN_MOUNT_VOLUME)
        return InfilfsMountVolume(DeviceObject, Irp, IrpSp);

    if (IrpSp->MinorFunction == IRP_MN_VERIFY_VOLUME)
        return InfilfsCompleteIrp(Irp, STATUS_SUCCESS, 0);

    return InfilfsCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

static VOID InfilfsFreeFcb(INFILFS_NATIVE_FCB *Fcb)
{
    if (!Fcb)
        return;
    if (Fcb->Path.Buffer)
        ExFreePoolWithTag(Fcb->Path.Buffer, INFILFS_NATIVE_FCB_TAG);
    ExDeleteResourceLite(&Fcb->PagingResource);
    ExDeleteResourceLite(&Fcb->MainResource);
    ExFreePoolWithTag(Fcb, INFILFS_NATIVE_FCB_TAG);
}

static INFILFS_NATIVE_FCB *InfilfsAllocateFcb(
    PCUNICODE_STRING Path,
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
    Fcb->References = 1;
    Fcb->FileId = Attributes->file_id;
    Fcb->ObjectType = Attributes->object_type;
    Fcb->FileAttributes = Attributes->file_attributes;
    ExInitializeFastMutex(&Fcb->HeaderMutex);
    if (!NT_SUCCESS(ExInitializeResourceLite(&Fcb->MainResource)) ||
        !NT_SUCCESS(ExInitializeResourceLite(&Fcb->PagingResource))) {
        InfilfsFreeFcb(Fcb);
        return NULL;
    }
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

static NTSTATUS InfilfsCreate(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    INFILFS_NATIVE_VOLUME *Volume = InfilfsVolumeFromDevice(DeviceObject);
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    struct infilfs_win_native_attributes Attributes;
    INFILFS_NATIVE_FCB *Fcb;
    INFILFS_NATIVE_CCB *Ccb;
    NTSTATUS Status;
    ULONG Disposition;
    ULONG Options;
    BOOLEAN DirectoryRequested;
    ULONG_PTR CreateInformation = FILE_OPENED;

    if (InfilfsIsControlDevice(DeviceObject) ||
        InfilfsIsFileSystemDevice(DeviceObject))
        return InfilfsCompleteIrp(Irp, STATUS_SUCCESS, FILE_OPENED);
    if (!Volume || !FileObject)
        return InfilfsCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);

    Disposition = (IrpSp->Parameters.Create.Options >> 24) & 0xffu;
    Options = IrpSp->Parameters.Create.Options & 0x00ffffffu;
    DirectoryRequested = (Options & FILE_DIRECTORY_FILE) != 0;

    RtlZeroMemory(&Attributes, sizeof(Attributes));
    Status = InfilfsLookupPath(Volume, &FileObject->FileName, &Attributes);
    if (Status == STATUS_OBJECT_NAME_NOT_FOUND) {
        if (Disposition == FILE_OPEN || Disposition == FILE_OVERWRITE)
            return InfilfsCompleteIrp(Irp, Status, 0);

        Status = InfilfsServiceMutation(
            Volume,
            DirectoryRequested ? INFILFS_WIN_NATIVE_OP_MKDIR :
                                 INFILFS_WIN_NATIVE_OP_CREATE,
            &FileObject->FileName, NULL, 0, 0);
        if (!NT_SUCCESS(Status))
            return InfilfsCompleteIrp(Irp, Status, 0);
        CreateInformation = FILE_CREATED;
        RtlZeroMemory(&Attributes, sizeof(Attributes));
        Status = InfilfsLookupPath(
            Volume, &FileObject->FileName, &Attributes);
    } else if (NT_SUCCESS(Status)) {
        if (Disposition == FILE_CREATE)
            return InfilfsCompleteIrp(
                Irp, STATUS_OBJECT_NAME_COLLISION, 0);

        if (DirectoryRequested &&
            Attributes.object_type != INFILFS_WIN_NATIVE_OBJECT_DIRECTORY)
            return InfilfsCompleteIrp(
                Irp, STATUS_NOT_A_DIRECTORY, 0);
        if ((Options & FILE_NON_DIRECTORY_FILE) &&
            Attributes.object_type == INFILFS_WIN_NATIVE_OBJECT_DIRECTORY)
            return InfilfsCompleteIrp(
                Irp, STATUS_FILE_IS_A_DIRECTORY, 0);

        if (Disposition == FILE_OVERWRITE ||
            Disposition == FILE_OVERWRITE_IF ||
            Disposition == FILE_SUPERSEDE) {
            if (Attributes.object_type != INFILFS_WIN_NATIVE_OBJECT_FILE)
                return InfilfsCompleteIrp(
                    Irp, STATUS_FILE_IS_A_DIRECTORY, 0);
            Status = InfilfsServiceMutation(
                Volume, INFILFS_WIN_NATIVE_OP_TRUNCATE,
                &FileObject->FileName, NULL, 0, 0);
            if (!NT_SUCCESS(Status))
                return InfilfsCompleteIrp(Irp, Status, 0);
            Attributes.logical_size = 0;
            Attributes.allocation_size = 0;
            CreateInformation =
                Disposition == FILE_SUPERSEDE ?
                FILE_SUPERSEDED : FILE_OVERWRITTEN;
        }
    }
    if (!NT_SUCCESS(Status))
        return InfilfsCompleteIrp(Irp, Status, 0);

    Fcb = InfilfsAllocateFcb(&FileObject->FileName, &Attributes);
    Ccb = ExAllocatePool2(
        POOL_FLAG_NON_PAGED, sizeof(*Ccb), INFILFS_NATIVE_CCB_TAG);
    if (!Fcb || !Ccb) {
        if (Fcb) InfilfsFreeFcb(Fcb);
        if (Ccb) ExFreePoolWithTag(Ccb, INFILFS_NATIVE_CCB_TAG);
        return InfilfsCompleteIrp(
            Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    }
    RtlZeroMemory(Ccb, sizeof(*Ccb));
    Ccb->Signature = 'cSfI';

    FileObject->FsContext = Fcb;
    FileObject->FsContext2 = Ccb;
    FileObject->SectionObjectPointer = &Fcb->SectionObjectPointers;

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

    return InfilfsCompleteIrp(Irp, STATUS_SUCCESS, CreateInformation);
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
    if (!Length)
        return InfilfsCompleteIrp(Irp, STATUS_SUCCESS, 0);
    Buffer = InfilfsGetIrpBuffer(Irp);
    if (!Buffer)
        return InfilfsCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);

    if (!(Irp->Flags & (IRP_NOCACHE | IRP_PAGING_IO))) {
        IO_STATUS_BLOCK Iosb;
        if (!CcCopyRead(
                FileObject, &Offset, Length, TRUE, Buffer, &Iosb))
            return InfilfsCompleteIrp(Irp, STATUS_CANT_WAIT, 0);
        return InfilfsCompleteIrp(
            Irp, Iosb.Status, Iosb.Information);
    }

    Status = InfilfsTransfer(
        Volume, Fcb, FALSE, (ULONGLONG)Offset.QuadPart,
        Buffer, Length, (Irp->Flags & IRP_PAGING_IO) != 0,
        FALSE, &Done);
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
    if (!Length)
        return InfilfsCompleteIrp(Irp, STATUS_SUCCESS, 0);
    Buffer = InfilfsGetIrpBuffer(Irp);
    if (!Buffer)
        return InfilfsCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    WriteThrough =
        (IrpSp->Flags & SL_WRITE_THROUGH) != 0 ||
        (FileObject->Flags & FO_WRITE_THROUGH) != 0;

    if (!(Irp->Flags & (IRP_NOCACHE | IRP_PAGING_IO))) {
        if (!CcCopyWrite(FileObject, &Offset, Length, TRUE, Buffer))
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
    return InfilfsCompleteIrp(Irp, Status, Done);
}

static NTSTATUS InfilfsFlushBuffers(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    INFILFS_NATIVE_VOLUME *Volume = InfilfsVolumeFromDevice(DeviceObject);
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    INFILFS_NATIVE_FCB *Fcb = FileObject ?
        (INFILFS_NATIVE_FCB *)FileObject->FsContext : NULL;
    struct infilfs_win_native_request *Request;
    struct infilfs_win_native_response *Response;
    NTSTATUS Status;

    if (!Volume)
        return InfilfsCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    if (Fcb && FileObject->SectionObjectPointer)
        CcFlushCache(
            FileObject->SectionObjectPointer, NULL, 0, &Irp->IoStatus);

    Request = ExAllocatePool2(
        POOL_FLAG_PAGED, sizeof(*Request), INFILFS_NATIVE_REQUEST_TAG);
    Response = ExAllocatePool2(
        POOL_FLAG_PAGED, sizeof(*Response), INFILFS_NATIVE_REQUEST_TAG);
    if (!Request || !Response) {
        if (Request) ExFreePoolWithTag(Request, INFILFS_NATIVE_REQUEST_TAG);
        if (Response) ExFreePoolWithTag(Response, INFILFS_NATIVE_REQUEST_TAG);
        return InfilfsCompleteIrp(
            Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
    }
    RtlZeroMemory(Request, sizeof(*Request));
    RtlZeroMemory(Response, sizeof(*Response));
    Request->opcode = INFILFS_WIN_NATIVE_OP_FLUSH;
    if (Fcb)
        InfilfsCopyPathToRequest(Request, &Fcb->Path);
    Status = InfilfsCallService(Volume, Request, Response);
    if (NT_SUCCESS(Status))
        Status = InfilfsTargetFlush(Volume->TargetDevice);
    ExFreePoolWithTag(Response, INFILFS_NATIVE_REQUEST_TAG);
    ExFreePoolWithTag(Request, INFILFS_NATIVE_REQUEST_TAG);
    return InfilfsCompleteIrp(Irp, Status, 0);
}

static NTSTATUS InfilfsCleanup(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT FileObject = IrpSp->FileObject;
    UNREFERENCED_PARAMETER(DeviceObject);

    if (FileObject && FileObject->SectionObjectPointer)
        CcFlushCache(FileObject->SectionObjectPointer, NULL, 0, NULL);
    if (FileObject)
        CcUninitializeCacheMap(FileObject, NULL, NULL);
    return InfilfsCompleteIrp(Irp, STATUS_SUCCESS, 0);
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
    if (Ccb)
        ExFreePoolWithTag(Ccb, INFILFS_NATIVE_CCB_TAG);
    if (Fcb && InterlockedDecrement(&Fcb->References) == 0)
        InfilfsFreeFcb(Fcb);
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

static NTSTATUS InfilfsDefaultDispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    return InfilfsCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

static VOID InfilfsUnload(PDRIVER_OBJECT DriverObject)
{
    PLIST_ENTRY Entry;
    UNREFERENCED_PARAMETER(DriverObject);
    InterlockedExchange(&g_Infilfs.Unloading, 1);

    if (g_Infilfs.FileSystemDevice)
        IoUnregisterFileSystem(g_Infilfs.FileSystemDevice);

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
    DriverObject->MajorFunction[IRP_MJ_FLUSH_BUFFERS] = InfilfsFlushBuffers;
    DriverObject->MajorFunction[IRP_MJ_FILE_SYSTEM_CONTROL] =
        InfilfsFileSystemControl;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] =
        InfilfsControlDeviceIo;
    DriverObject->DriverUnload = InfilfsUnload;

    g_Infilfs.FileSystemDevice->Flags |= DO_DIRECT_IO;
    g_Infilfs.FileSystemDevice->Flags &= ~DO_DEVICE_INITIALIZING;
    g_Infilfs.ControlDevice->Flags |= DO_BUFFERED_IO;
    g_Infilfs.ControlDevice->Flags &= ~DO_DEVICE_INITIALIZING;

    IoRegisterFileSystem(g_Infilfs.FileSystemDevice);
    return STATUS_SUCCESS;
}
