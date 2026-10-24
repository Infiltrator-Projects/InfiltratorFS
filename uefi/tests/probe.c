/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Disposable OVMF-only test executable. Not an installed Boot component. */
#include <efi.h>
#include <efilib.h>
#include <stdint.h>
#include "expected.h"

#define COM1 0x3f8
#define EXIT_PORT 0xf4
static void outs(uint16_t port, uint8_t v) { __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(port)); }
static uint8_t ins(uint16_t port) { uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(port)); return v; }
static void serial(const char *msg) {
    while (*msg) {
        unsigned int n = 0;
        while (!(ins(COM1+5) & 0x20) && ++n < 1000000) {}
        outs(COM1, (uint8_t)*msg++);
    }
}
static void finish(unsigned int ok, const char *stage) {
    serial(stage); serial(ok ? " PASS\r\n" : " FAIL\r\n");
    __asm__ volatile ("outl %0,%1"::"a"((uint32_t)(ok ? 0 : 1)),"Nd"((uint16_t)EXIT_PORT));
    for (;;) __asm__ volatile("pause");
}
static UINT64 fnv(UINT64 hash, const UINT8 *b, UINTN len) {
    UINTN i;
    for (i=0; i<len; ++i) { hash ^= b[i]; hash *= 1099511628211ULL; }
    return hash;
}
static EFI_STATUS check_file(EFI_FILE_HANDLE root, CHAR16 *path, UINT64 expected_size, UINT64 expected_hash) {
    EFI_FILE_HANDLE f = NULL;
    EFI_STATUS st = uefi_call_wrapper(root->Open,5,root,&f,path,EFI_FILE_MODE_READ,0);
    if (EFI_ERROR(st)) return st;
    EFI_FILE_INFO *info = LibFileInfo(f);
    if (info == NULL || info->FileSize != expected_size) {
        if (info) FreePool(info);
        uefi_call_wrapper(f->Close,1,f);
        return EFI_VOLUME_CORRUPTED;
    }
    FreePool(info);
    UINT8 *chunk = AllocatePool(65536);
    if (!chunk) { uefi_call_wrapper(f->Close,1,f); return EFI_OUT_OF_RESOURCES; }
    UINT64 hash=1469598103934665603ULL, done=0;
    while (done < expected_size) {
        UINTN want=(UINTN)((expected_size-done)>65536?65536:(expected_size-done));
        UINTN actual=want;
        st=uefi_call_wrapper(f->Read,3,f,&actual,chunk);
        if (EFI_ERROR(st) || actual==0 || actual>want) { st=EFI_DEVICE_ERROR; break; }
        hash=fnv(hash,chunk,actual);
        done+=actual;
    }
    FreePool(chunk);
    uefi_call_wrapper(f->Close,1,f);
    if (EFI_ERROR(st)) return st;
    if (done != expected_size || hash != expected_hash) return EFI_CRC_ERROR;
    return EFI_SUCCESS;
}
/* GNU-EFI's crt0 calls efi_main through the C ABI; its startup code
 * handles the firmware entry ABI. EFIAPI here corrupts both arguments. */
EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *system) {
    InitializeLib(image,system);
    outs(COM1+1,0); outs(COM1+3,0x80); outs(COM1,1); outs(COM1+1,0);
    outs(COM1+3,3); outs(COM1+2,0xc7); outs(COM1+4,0x0b);
    serial("INFS_UEFI_PROBE_STARTED\r\n");
    serial("STAGE_AFTER_SERIAL\r\n");
    EFI_GUID loaded_guid=EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID block_guid=EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_GUID sfs_guid=EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_LOADED_IMAGE *li=NULL;
    serial("STAGE_HANDLE_PROTOCOL_BEGIN\r\n");
    EFI_STATUS st=uefi_call_wrapper(BS->HandleProtocol,3,image,&loaded_guid,(VOID**)&li);
    serial("STAGE_HANDLE_PROTOCOL_END\r\n");
    if (EFI_ERROR(st) || !li) { finish(0,"HANDLE_LOADED_IMAGE"); return st; }
    serial("STAGE_FILE_DEVICE_PATH_BEGIN\r\n");
    EFI_DEVICE_PATH *dp=FileDevicePath(li->DeviceHandle,L"\\InfiltratorFS_x64.efi");
    serial("STAGE_FILE_DEVICE_PATH_END\r\n");
    if(!dp) {finish(0,"MAKE_DRIVER_PATH");return EFI_OUT_OF_RESOURCES;}
    EFI_HANDLE drv=NULL;
    serial("STAGE_LOAD_IMAGE_BEGIN\r\n");
    st=uefi_call_wrapper(BS->LoadImage,6,FALSE,image,dp,NULL,0,&drv);
    serial("STAGE_LOAD_IMAGE_END\r\n");
    if (EFI_ERROR(st)) {finish(0,"LOAD_DRIVER");return st;}
    serial("DRIVER_LOAD_PASS\r\n");
    serial("STAGE_START_IMAGE_BEGIN\r\n");
    st=uefi_call_wrapper(BS->StartImage,3,drv,NULL,NULL);
    serial("STAGE_START_IMAGE_END\r\n");
    if (EFI_ERROR(st)) {finish(0,"START_DRIVER");return st;}
    serial("DRIVER_START_PASS\r\n");
    EFI_HANDLE *handles=NULL;
    UINTN count=0;
    st=uefi_call_wrapper(BS->LocateHandleBuffer,5,ByProtocol,&block_guid,NULL,&count,&handles);
    if (EFI_ERROR(st)) {finish(0,"BLOCKIO_ENUMERATE");return st;}
    for (UINTN i=0;i<count;i++)
        uefi_call_wrapper(BS->ConnectController,4,handles[i],NULL,NULL,TRUE);
    if(handles)FreePool(handles);
    serial("BLOCKIO_CONNECT_DONE\r\n");
    handles=NULL;count=0;
    st=uefi_call_wrapper(BS->LocateHandleBuffer,5,ByProtocol,&sfs_guid,NULL,&count,&handles);
    if(EFI_ERROR(st)){finish(0,"SFS_ENUMERATE");return st;}
    for(UINTN i=0;i<count;i++){
        EFI_FILE_IO_INTERFACE *fs=NULL;
        if(EFI_ERROR(uefi_call_wrapper(BS->HandleProtocol,3,handles[i],&sfs_guid,(VOID**)&fs)) || !fs) continue;
        EFI_FILE_HANDLE root=NULL;
        if(EFI_ERROR(uefi_call_wrapper(fs->OpenVolume,2,fs,&root)) || !root) continue;
        st=check_file(root,L"\\boot\\vmlinuz",PROBE_KERNEL_SIZE,PROBE_KERNEL_HASH);
        if(st==EFI_SUCCESS){
            serial("KERNEL_READ_PASS\r\n");
            st=check_file(root,L"\\boot\\initrd.img",PROBE_INITRD_SIZE,PROBE_INITRD_HASH);
            uefi_call_wrapper(root->Close,1,root);
            if(st==EFI_SUCCESS){finish(1,"INFS_UEFI_READ");return EFI_SUCCESS;}
            finish(0,"INITRD_READ");return st;
        }
        uefi_call_wrapper(root->Close,1,root);
    }
    finish(0,"OFS_OR_FAT_ONLY_NO_INFILTRATORFS_FILES");
    return EFI_NOT_FOUND;
}
