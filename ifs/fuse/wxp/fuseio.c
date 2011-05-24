/*++

Copyright (c) 2011 FoW Group

Module Name:

    FuseIo.c

Abstract:

    This module implements the main routines for the FUSE filesystem driver

--*/

#include <ntdef.h>
#include <NtStatus.h>
#include <Ntstrsafe.h>
#include "fuseprocs.h"
#include "fusent_proto.h"
#include "hashmap.h"
#include "fusestruc.h"

NTSTATUS
FuseAddUserspaceIrp (
    IN PIRP Irp,
    IN PIO_STACK_LOCATION IrpSp
    );

BOOLEAN
FuseAddIrpToModuleList (
    IN PIRP Irp,
    IN PMODULE_STRUCT ModuleStruct,
    IN BOOLEAN AddToModuleList
    );

VOID
FuseCheckForWork (
    IN PMODULE_STRUCT ModuleStruct
    );

NTSTATUS
FuseCopyResponse (
    IN PMODULE_STRUCT ModuleStruct,
    IN PIRP Irp,
    IN PIO_STACK_LOCATION IrpSp
    );

BOOLEAN
FuseCheckUnmountModule (
    IN PIO_STACK_LOCATION IrpSp
    );

NTSTATUS
FuseQueryBasicInfo (
    IN OUT PFILE_BASIC_INFORMATION Buffer,
    IN OUT PLONG Length
    );

NTSTATUS
FuseQueryStandardInfo (
    IN OUT PFILE_STANDARD_INFORMATION Buffer,
    IN OUT PLONG Length
    );

NTSTATUS
FuseQueryNameInfo (
    IN PIO_STACK_LOCATION IrpSp,
    IN OUT PFILE_NAME_INFORMATION Buffer,
    IN OUT PLONG Length
    );

NTSTATUS
FuseQueryFsVolumeInfo (
    IN PIO_STACK_LOCATION IrpSp,
    IN OUT PFILE_FS_VOLUME_INFORMATION Buffer,
    IN OUT PLONG Length
    );

NTSTATUS
FuseQueryFsSizeInfo (
    IN PIO_STACK_LOCATION IrpSp,
    IN OUT PFILE_FS_SIZE_INFORMATION Buffer,
    IN OUT PLONG Length
    );

NTSTATUS
FuseQueryFsDeviceInfo (
    IN PIO_STACK_LOCATION IrpSp,
    IN OUT PFILE_FS_DEVICE_INFORMATION Buffer,
    IN OUT PLONG Length
    );

NTSTATUS
FuseQueryFsAttributeInfo (
    IN PIO_STACK_LOCATION IrpSp,
    IN OUT PFILE_FS_ATTRIBUTE_INFORMATION Buffer,
    IN OUT PLONG Length
    );

NTSTATUS
FuseQueryFsFullSizeInfo (
    IN PIO_STACK_LOCATION IrpSp,
    IN OUT PFILE_FS_FULL_SIZE_INFORMATION Buffer,
    IN OUT PLONG Length
    );

//
//  This hash map implementation uses linear probing, which
//  is not very fast for densely populated maps, but we set
//  the initial size of the map to be large enough so that
//  the type of probing is somewhat irrelevant
//
hashmap ModuleMap;

NTSTATUS
FuseAddUserspaceIrp (
    IN PIRP Irp,
    IN PIO_STACK_LOCATION IrpSp
    )

{
    NTSTATUS Status = STATUS_SUCCESS;
    WCHAR* FileName = IrpSp->FileObject->FileName.Buffer + 1;
    PUNICODE_STRING FileNameString = &IrpSp->FileObject->FileName;
    WCHAR* ModuleName;
    WCHAR* BackslashPosition = NULL;
    PMODULE_STRUCT ModuleStruct;

    DbgPrint("Adding userspace IRP to queue for file %S\n", FileName);

    //
    //  Check if a module was opened, as opposed to just \Device\Fuse. There
    //  is nothing to be done if the file is not on a module
    //

    if (FileNameString->Length > 0) {

        //
        //  First, check if there is an entry in the hash map for the module.
        //  Shift FileName by one WCHAR so that we don't read the initial \
        //

        BackslashPosition = wcsstr(FileName, L"\\");
        
        if(!BackslashPosition) {
            ModuleName = FileName;
        } else {
            ULONG ModuleNameLength = (ULONG)(BackslashPosition - FileName);
            ModuleName = (WCHAR*) ExAllocatePool(PagedPool, sizeof(WCHAR) * (ModuleNameLength + 1));
            memcpy(ModuleName, FileName, sizeof(WCHAR) * (ModuleNameLength));
            ModuleName[ModuleNameLength] = '\0';
        }

        DbgPrint("Parsed module name for userspace request is %S\n", ModuleName);

        ModuleStruct = (PMODULE_STRUCT) hmap_get(&ModuleMap, ModuleName);
        if(!ModuleStruct) {
            DbgPrint("No entry in map found for module %S. Completing request\n", ModuleName);

        
        } else {
            DbgPrint("Found entry in map for module %S. Adding userspace IRP to module queue\n", ModuleName);

            FuseAddIrpToModuleList(Irp, ModuleStruct, FALSE);

            //
            //  Mark the request as status pending. The module thread that calls
            //  NtFsControlFile with a response will complete it, or if there is
            //  a request to hand it off to right now, that request will complete it
            //

            IoMarkIrpPending(Irp);
            Status = STATUS_PENDING;

            FuseCheckForWork(ModuleStruct);
        }

        //
        //  If we allocated the module name string in memory, free it
        //

        if(ModuleName != FileName) {
            ExFreePool(ModuleName);
        }
    } else {
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        Status = STATUS_SUCCESS;
    }

    return Status;
}

BOOLEAN
FuseAddIrpToModuleList (
    IN PIRP Irp,
    IN PMODULE_STRUCT ModuleStruct,
    IN BOOLEAN AddToModuleList
    )
//
//  Adds the given IRP to the module IRP list if the AddToModuleList flag is on, and
//  adds the given IRP to the userspace IRP list otherwise. Validates the user buffer
//  of module IRPs and returns FALSE if the validation fails, TRUE otherwise
//
{
    DbgPrint("Adding IRP to queue for module %S from a %s request\n",
        ModuleStruct->ModuleName,
        (AddToModuleList ? "module" : "userspace"));

    if(AddToModuleList) {
        ULONG ExpectedBufferLength;
        PIO_STACK_LOCATION ModuleIrpSp = IoGetCurrentIrpStackLocation(Irp);
            
        PWSTR ModuleName = ModuleStruct->ModuleName;
        ULONG FileNameLength = wcslen(ModuleName) * (sizeof(WCHAR) + 1);
        ULONG StackLength = Irp->StackCount * sizeof(IO_STACK_LOCATION);
        ExpectedBufferLength = sizeof(FUSENT_REQ) + StackLength + 4 + FileNameLength;

        //
        //  Check that the output buffer is large enough to contain the work request
        //

        if(ModuleIrpSp->Parameters.DeviceIoControl.OutputBufferLength < ExpectedBufferLength) {

            DbgPrint("Module %S supplied a bad request buffer. Required size: %d. Actual size: %d\n",
                ModuleStruct->ModuleName, ExpectedBufferLength, ModuleIrpSp->Parameters.DeviceIoControl.OutputBufferLength);
            
            return FALSE;
        }
    }

    //
    //  Acquire the lock for the module struct
    //

    ExAcquireFastMutex(&ModuleStruct->ModuleLock);

    __try {
        //
        //  Add to the list specified by Module--TRUE -> module list, FALSE -> userspace list
        //

        PIRP_LIST LastEntry = AddToModuleList ? ModuleStruct->ModuleIrpListEnd : ModuleStruct->UserspaceIrpListEnd;
        if(!LastEntry) {

            //
            //  If the first entry in the list is NULL, replace it with a new entry
            //

            if(AddToModuleList) {
                ModuleStruct->ModuleIrpList = (PIRP_LIST) ExAllocatePool(PagedPool, sizeof(IRP_LIST));
                LastEntry = ModuleStruct->ModuleIrpList;
            } else {
                ModuleStruct->UserspaceIrpList = (PIRP_LIST) ExAllocatePool(PagedPool, sizeof(IRP_LIST));
                LastEntry = ModuleStruct->UserspaceIrpList;
            }
        } else {

            //
            //  Insert the new entry at the end of the list
            //

            LastEntry->Next = (PIRP_LIST) ExAllocatePool(PagedPool, sizeof(IRP_LIST));
            LastEntry = LastEntry->Next;
        }

        //
        //  Set the fields of the new entry in the IRP list
        //

        LastEntry->Irp = Irp;
        LastEntry->Next = NULL;

        if(AddToModuleList) {
            ModuleStruct->ModuleIrpListEnd = LastEntry;
        } else {
            ModuleStruct->UserspaceIrpListEnd = LastEntry;
        }
    } __finally {
        ExReleaseFastMutex(&ModuleStruct->ModuleLock);
    }

    DbgPrint("Successfully added %s request IRP to queue\n", (AddToModuleList ? "module" : "userspace"));

    return TRUE;
}

VOID
FuseCheckForWork (
    IN PMODULE_STRUCT ModuleStruct
    )
{
    //
    //  Check if there is both a userspace request and a module
    //  IRP to which to hand it off
    //

    if(ModuleStruct->ModuleIrpList && ModuleStruct->UserspaceIrpList) {

        ExAcquireFastMutex(&ModuleStruct->ModuleLock);

        __try {

            //
            //  Perform the check again now that we have the lock
            //

            while(ModuleStruct->ModuleIrpList && ModuleStruct->UserspaceIrpList) {
                PIRP_LIST ModuleIrpListEntry = ModuleStruct->ModuleIrpList;
                PIRP_LIST UserspaceIrpListEntry = ModuleStruct->UserspaceIrpList;
                PIRP ModuleIrp = ModuleIrpListEntry->Irp;
                PIRP UserspaceIrp = UserspaceIrpListEntry->Irp;
                PIO_STACK_LOCATION UserspaceIrpSp;
                FUSENT_REQ* FuseNtReq;
                WCHAR* FileName;
                ULONG FileNameLength;
                PULONG FileNameLengthField;

                PWSTR ModuleName = ModuleStruct->ModuleName;
                ULONG StackLength = UserspaceIrp->StackCount * sizeof(IO_STACK_LOCATION);

                DbgPrint("Work found for module %S. Pairing userspace request with module request for work\n", ModuleName);

                //
                //  Remove the head entry from the module IRP queue
                //

                ModuleStruct->ModuleIrpList = ModuleIrpListEntry->Next;

                //
                //  If we removed the first entry of the queue, update the end
                //  pointer to NULL so that it is not pointing to an invalid entry
                //

                if(!ModuleStruct->ModuleIrpList) {
                    ModuleStruct->ModuleIrpListEnd = NULL;
                }

                //
                //  Remove the head IRP from the userspace IRP queue
                //

                ModuleStruct->UserspaceIrpList = UserspaceIrpListEntry->Next;

                //
                //  If we removed the first entry of the queue, update the end
                //  pointer to NULL so that it is not pointing to an invalid entry
                //

                if(!ModuleStruct->UserspaceIrpList) {
                    ModuleStruct->UserspaceIrpListEnd = NULL;
                }

                //
                //  Fill out the FUSE module request using the userspace IRP
                //

                UserspaceIrpSp = IoGetCurrentIrpStackLocation(UserspaceIrp);

                FuseNtReq = (FUSENT_REQ*) ModuleIrp->AssociatedIrp.SystemBuffer;

                FuseNtReq->pirp = UserspaceIrp;
                FuseNtReq->fop = UserspaceIrpSp->FileObject;
                FuseNtReq->irp = *UserspaceIrp;
                memcpy(FuseNtReq->iostack, UserspaceIrp + 1, StackLength);

                FileNameLengthField = (PULONG) (((PCHAR) FuseNtReq->iostack) + StackLength);
                FileName = UserspaceIrpSp->FileObject->FileName.Buffer;
                
                //
                //  Strip out the module name from the file name
                //
                FileName ++;
                while(FileName[0] != L'\\' && FileName[0] != L'\0') {
                    FileName ++;
                }

                FileNameLength = (wcslen(FileName) + 1) * sizeof(WCHAR);
                *FileNameLengthField = FileNameLength;

                memcpy(FileNameLengthField + 1, FileName, FileNameLength);
                ModuleIrp->IoStatus.Information = sizeof(FUSENT_REQ) + StackLength + 4 + FileNameLength;

                //
                //  Add the userspace IRP to the list of outstanding IRPs (i.e. the IRPs
                //  for which the module has yet to send a reply)
                //

                if(ModuleStruct->OutstandingIrpListEnd) {
                    ModuleStruct->OutstandingIrpListEnd->Next = UserspaceIrpListEntry;
                    ModuleStruct->OutstandingIrpListEnd = ModuleStruct->OutstandingIrpListEnd->Next;
                } else {
                    ModuleStruct->OutstandingIrpList = UserspaceIrpListEntry;
                    ModuleStruct->OutstandingIrpListEnd = ModuleStruct->OutstandingIrpList;
                }

                //
                //  Free the module IRP entry from memory
                //

                ExFreePool(ModuleIrpListEntry);

                //
                //  Complete the module's IRP to signal that the module should process it
                //

                ModuleIrp->IoStatus.Status = STATUS_SUCCESS;
                IoCompleteRequest(ModuleIrp, IO_NO_INCREMENT);
            }
        } __finally {
            ExReleaseFastMutex(&ModuleStruct->ModuleLock);
        }
    }
}

NTSTATUS
FuseCopyResponse (
    IN PMODULE_STRUCT ModuleStruct,
    IN PIRP Irp,
    IN PIO_STACK_LOCATION IrpSp
    )
//
//  Attempt to find an outstanding userspace IRP whose pointer
//  matches that given in the module's response. If one is found
//  (i.e. the module's response is legitimate) then pair up the
//  response with the userspace IRP and complete both IRPs
//
{
    NTSTATUS Status = STATUS_SUCCESS;

    //
    //  First validate the user buffer
    //

    if(IrpSp->Parameters.DeviceIoControl.InputBufferLength >= sizeof(FUSENT_RESP)) {

        FUSENT_RESP* FuseNtResp = (FUSENT_RESP*) Irp->AssociatedIrp.SystemBuffer;
        PIRP UserspaceIrp = FuseNtResp->pirp;

        //
        //  Do *not* attempt to complete the userspace IRP until we verify that the pointer
        //  is valid. A list of outstanding userspace IRPs is maintained for this purpose
        //

        ExAcquireFastMutex(&ModuleStruct->ModuleLock);

        __try {
            PIRP_LIST CurrentEntry = ModuleStruct->OutstandingIrpList;
            PIRP_LIST PreviousEntry = NULL;
            if(!CurrentEntry) {

                //
                //  The list is empty so the module's response is invalid
                //

                DbgPrint("Module %S sent a response but there are no outstanding userspace IRPs\n", ModuleStruct->ModuleName);

                Status = STATUS_INVALID_DEVICE_REQUEST;
            } else {
                BOOLEAN MatchFound = FALSE;

                while(CurrentEntry && !MatchFound) {
                    if(CurrentEntry->Irp == UserspaceIrp) {
                        MatchFound = TRUE;
                    } else {
                        PreviousEntry = CurrentEntry;
                        CurrentEntry = CurrentEntry->Next;
                    }
                }

                if(MatchFound) {

                    PIO_STACK_LOCATION UserspaceIrpSp = IoGetCurrentIrpStackLocation(UserspaceIrp);

                    DbgPrint("Match found for response from module %S. Completing userspace request. Status is %d for major code %x on file %S\n",
                        ModuleStruct->ModuleName, FuseNtResp->status, UserspaceIrp->Flags, UserspaceIrpSp->FileObject->FileName.Buffer);

                    //
                    //  We've found a match. Complete the userspace request
                    //
                    //  TODO: copy buffers for reads and writes
                    if(FuseNtResp->error != 0) {
                        UserspaceIrp->IoStatus.Status = -FuseNtResp->error;
                    } else {
                        UserspaceIrp->IoStatus.Status = FuseNtResp->status;
                    }
                    IoCompleteRequest(UserspaceIrp, IO_NO_INCREMENT);

                    //
                    //  Remove the entry from the outstanding IRP queue
                    //

                    if(PreviousEntry) {
                        PreviousEntry->Next = PreviousEntry->Next->Next;
                    } else {
                        ModuleStruct->OutstandingIrpList = CurrentEntry->Next;
                    }

                    if(!CurrentEntry->Next) {
                        ModuleStruct->OutstandingIrpListEnd = PreviousEntry;
                    }

                    //
                    //  Free the entry from memory
                    //

                    ExFreePool(CurrentEntry);

                    Status = STATUS_SUCCESS;
                } else {

                    //
                    //  No userspace IRP with an address matching that given was found,
                    //  so the module's response is assumed to be invalid
                    //

                    DbgPrint("Module %S sent a response but no match for the userspace IRP %x was found\n", ModuleStruct->ModuleName, UserspaceIrp);

                    Status = STATUS_INVALID_DEVICE_REQUEST;
                }
            }
        } __finally {
            ExReleaseFastMutex(&ModuleStruct->ModuleLock);
        }

    } else {

        DbgPrint("Response from %S has a bad user buffer. Expected size: %d. Actual size: \n",
            ModuleStruct->ModuleName, sizeof(FUSENT_RESP), IrpSp->Parameters.DeviceIoControl.InputBufferLength);

        Status = STATUS_INVALID_USER_BUFFER;
    }

    return Status;
}

BOOLEAN
FuseCheckUnmountModule (
    IN PIO_STACK_LOCATION IrpSp
    )
//
//  Check whether the closure of the file represented by the given file object pointer
//  is an indication that a module should be unmounted. A module should be unmounted
//  when the file handle with which it was initially mounted is closed.
//
//  Returns whether a module was dismounted
//
{
    if(IrpSp->FileObject->FileName.Length > 0) {
        WCHAR* ModuleName = IrpSp->FileObject->FileName.Buffer + 1;
        PMODULE_STRUCT ModuleStruct = (PMODULE_STRUCT) hmap_get(&ModuleMap, ModuleName);

        if(ModuleStruct && ModuleStruct->ModuleFileObject == IrpSp->FileObject) {

            DbgPrint("Dismounting module %S, as the last reference to its handle has been lost\n", ModuleName);

            //
            //  The file object matches, so perform a dismount
            //

            ExAcquireFastMutex(&ModuleStruct->ModuleLock);

            hmap_remove(&ModuleMap, ModuleName);

            //
            //  No need to "release" the lock, as it has been freed
            //

            return TRUE;
        }
    }

    return FALSE;
}

NTSTATUS
FuseFsdFileSystemControl (
    IN PVOLUME_DEVICE_OBJECT VolumeDeviceObject,
    IN PIRP Irp
    )
{
    NTSTATUS Status = STATUS_SUCCESS;
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);

    DbgPrint("FuseFsdFileSystemControl\n");

    //
    //  When a module requests a mount, its unique name of the form
    //  [module-name]-[process ID] is added to the module map and space is
    //  allocated for the module's storage structure
    //
    //  Modules will call NtFsControlFile with IRP_FUSE_MODULE_REQUEST
    //  potentially many times with many different buffers while waiting
    //  for work from userspace applications. When work comes in in the
    //  form of CreateFiles, ReadFiles, WriteFiles, etc., the driver hands
    //  off requests to the module by filling out and then completing IRPs,
    //  which signals to the module that there is work to be done
    //
    //  When a module completes work, it calls NtFsControlFile with
    //  IRP_FUSE_MODULE_RESPONSE, and the IRP for the corresponding
    //  CreateFile, ReadFile, WriteFile, etc. is filled out using the
    //  response from the module and then completed
    //

    if(IrpSp->FileObject->FileName.Length <= 1) {

        //
        //  File names pointing to valid modules should be of the form "\[name]-[pid]"
        //

        return STATUS_INVALID_PARAMETER;

    } else if(IrpSp->Parameters.FileSystemControl.FsControlCode == IRP_FUSE_MOUNT) {
        WCHAR* ModuleName = IrpSp->FileObject->FileName.Buffer + 1;
        PMODULE_STRUCT ModuleStruct;

        DbgPrint("A mount has been requested for module %S\n", ModuleName);

        //
        //  Set up a new module struct for storing state and add it to the
        //  module hash map
        //

        ModuleStruct = (PMODULE_STRUCT) ExAllocatePool(PagedPool, sizeof(MODULE_STRUCT));
        RtlZeroMemory(ModuleStruct, sizeof(MODULE_STRUCT));
        ExInitializeFastMutex(&ModuleStruct->ModuleLock);

        //
        //  Store the module's file object so that we can later verify that the
        //  module is what is requesting or providing responses to work and not
        //  some malicious application or rogue group member
        //

        ModuleStruct->ModuleFileObject = IrpSp->FileObject;
        ModuleStruct->ModuleName = (WCHAR*) ExAllocatePool(PagedPool, sizeof(WCHAR) * (wcslen(ModuleName) + 1));
        RtlStringCchCopyW(ModuleStruct->ModuleName, wcslen(ModuleName) + 1, ModuleName);
        hmap_add(&ModuleMap, ModuleStruct->ModuleName, ModuleStruct);

        Status = STATUS_SUCCESS;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);

    } else if(IrpSp->Parameters.FileSystemControl.FsControlCode == IRP_FUSE_MODULE_REQUEST ||
        IrpSp->Parameters.FileSystemControl.FsControlCode == IRP_FUSE_MODULE_RESPONSE) {

        WCHAR* ModuleName = IrpSp->FileObject->FileName.Buffer + 1;
        PMODULE_STRUCT ModuleStruct = (PMODULE_STRUCT) hmap_get(&ModuleMap, ModuleName);

        //
        //  Verify that whoever is making the request is using the same file handle that
        //  the module initially used to set up communication
        //

        if(!ModuleStruct) {
            DbgPrint("NtFsControlFile called on unmounted module %S with code %x\n", ModuleName, IrpSp->Parameters.FileSystemControl.FsControlCode);

            Status = STATUS_INVALID_PARAMETER;
        } else if(ModuleStruct->ModuleFileObject == IrpSp->FileObject) {

            if(IrpSp->Parameters.FileSystemControl.FsControlCode == IRP_FUSE_MODULE_REQUEST) {

                DbgPrint("Received request for work from module %S\n", ModuleName);

                //
                //  Save IRP in module struct and check if there is a userspace request to give it
                //

                if(!FuseAddIrpToModuleList(Irp, ModuleStruct, TRUE)) {

                    DbgPrint("Module %S supplied bad buffer; bailing\n", ModuleStruct->ModuleName);

                    //
                    //  If the addition of the IRP to the module list fails due to a bad buffer, bail out
                    //

                    Status = STATUS_INVALID_USER_BUFFER;
                    IoCompleteRequest(Irp, IO_NO_INCREMENT);
                } else {
                    FuseCheckForWork(ModuleStruct);

                    Status = STATUS_PENDING;
                    IoMarkIrpPending(Irp);
                }
            } else {

                DbgPrint("Received response for work from module %S\n", ModuleName);

                Status = FuseCopyResponse(ModuleStruct, Irp, IrpSp);

                if(Status == STATUS_SUCCESS) {
                    DbgPrint("Response for work from module %S was processed successfully\n", ModuleName);
                } else {
                    DbgPrint("Response for work from module %S was unsuccessfully processed\n", ModuleName);
                }

                IoCompleteRequest(Irp, IO_NO_INCREMENT);
            }
        } else {

            //
            //  Oh noes! Someone is trying to trick us into thinking that they are a FUSE module
            //

            DbgPrint("FUSE request or response received from invalid source. Original file object \
                     pointer was %x and given file object pointer is %x for module %S\n",
                     ModuleStruct->ModuleFileObject, IrpSp->FileObject, IrpSp->FileObject->FileName.Buffer + 1);

            Status = STATUS_INVALID_PARAMETER;
            IoCompleteRequest(Irp, IO_NO_INCREMENT);
        }
    } else {
        Status = STATUS_INVALID_PARAMETER;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
    }

    return Status;
}


NTSTATUS
FuseFsdCleanup (
    IN PVOLUME_DEVICE_OBJECT VolumeDeviceObject,
    IN PIRP Irp
    )
{
    PIO_STACK_LOCATION IrpSp;
    IrpSp = IoGetCurrentIrpStackLocation(Irp);

    DbgPrint("FuseFsdCleanup\n");

    return STATUS_SUCCESS;
}

NTSTATUS
FuseFsdClose (
    IN PVOLUME_DEVICE_OBJECT VolumeDeviceObject,
    IN PIRP Irp
    )
{
    PIO_STACK_LOCATION IrpSp;
    IrpSp = IoGetCurrentIrpStackLocation(Irp);

    FuseCheckUnmountModule(IrpSp);

    DbgPrint("FuseFsdClose\n");
    return STATUS_SUCCESS;
}

NTSTATUS
FuseFsdCreate (
    IN PVOLUME_DEVICE_OBJECT VolumeDeviceObject,
    IN PIRP Irp
    )
{
    PIO_STACK_LOCATION IrpSp;
    IrpSp = IoGetCurrentIrpStackLocation(Irp);

    DbgPrint("FuseFsdCreate called on '%wZ'\n", &IrpSp->FileObject->FileName);

    return FuseAddUserspaceIrp(Irp, IrpSp);
}

NTSTATUS
FuseFsdRead (
    IN PVOLUME_DEVICE_OBJECT VolumeDeviceObject,
    IN PIRP Irp
    )
{
    PIO_STACK_LOCATION IrpSp;
    IrpSp = IoGetCurrentIrpStackLocation(Irp);

    DbgPrint("FuseFsdRead called on '%wZ'\n", &IrpSp->FileObject->FileName);
    return STATUS_SUCCESS;
}

NTSTATUS
FuseFsdWrite (
    IN PVOLUME_DEVICE_OBJECT VolumeDeviceObject,
    IN PIRP Irp
    )
{
    PIO_STACK_LOCATION IrpSp;
    IrpSp = IoGetCurrentIrpStackLocation(Irp);

    DbgPrint("FuseFsdWrite called on '%wZ'\n", &IrpSp->FileObject->FileName);
    return STATUS_SUCCESS;
}

NTSTATUS
FuseFsdDeviceControl (
    IN PVOLUME_DEVICE_OBJECT VolumeDeviceObject,
    IN PIRP Irp
    )
{
    DbgPrint("FuseFsdDeviceControl\n");

    return STATUS_SUCCESS;
}

NTSTATUS
FuseFsdDirectoryControl (
    IN PVOLUME_DEVICE_OBJECT VolumeDeviceObject,
    IN PIRP Irp
    )
{
    DbgPrint("FuseFsdDirectoryControl\n");
    return STATUS_SUCCESS;
}

NTSTATUS
FuseFsdSetEa (
    IN PVOLUME_DEVICE_OBJECT VolumeDeviceObject,
    IN PIRP Irp
    )
{
    DbgPrint("FuseFsdSetEa\n");
    return STATUS_SUCCESS;
}

NTSTATUS
FuseFsdQueryInformation (
    IN PVOLUME_DEVICE_OBJECT VolumeDeviceObject,
    IN PIRP Irp
    )
{
    NTSTATUS Status = STATUS_SUCCESS;
    LONG Length;
    FILE_INFORMATION_CLASS FileInformationClass;
    PFILE_ALL_INFORMATION AllInfo;
    LARGE_INTEGER Time;
    LARGE_INTEGER FileSize;
    
    PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);

    DbgPrint("FuseFsdQueryInformation\n");

    Length = (LONG) IrpSp->Parameters.QueryFile.Length;
    FileInformationClass = IrpSp->Parameters.QueryFile.FileInformationClass;
    AllInfo = (PFILE_ALL_INFORMATION) Irp->AssociatedIrp.SystemBuffer;

    switch (FileInformationClass) {

    case FileAllInformation:

        //
        //  For the all information class we'll typecast a local
        //  pointer to the output buffer and then call the
        //  individual routines to fill in the buffer.
        //

        FuseQueryBasicInfo(&AllInfo->BasicInformation, &Length);
        FuseQueryStandardInfo(&AllInfo->StandardInformation, &Length);
        FuseQueryNameInfo(IrpSp, &AllInfo->NameInformation, &Length);

        break;

    case FileBasicInformation:

        FuseQueryBasicInfo(&AllInfo->BasicInformation, &Length);
        break;

    case FileStandardInformation:

        FuseQueryStandardInfo(&AllInfo->StandardInformation, &Length);
        break;

    case FileNameInformation:

        FuseQueryNameInfo(IrpSp, &AllInfo->NameInformation, &Length);
        break;

    default:

        Status = STATUS_INVALID_PARAMETER;
    }

    //
    //  If we overflowed the buffer, set the length to 0 and change the
    //  status to STATUS_BUFFER_OVERFLOW.
    //

    if ( Length < 0 ) {

        Status = STATUS_BUFFER_OVERFLOW;

        Length = 0;
    }

    //
    //  Set the information field to the number of bytes actually filled in
    //  and then complete the request
    //

    Irp->IoStatus.Information = IrpSp->Parameters.QueryFile.Length - Length;

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    
    return Status;
}

NTSTATUS
FuseQueryBasicInfo (
    IN OUT PFILE_BASIC_INFORMATION Buffer,
    IN OUT PLONG Length
    )
{
    NTSTATUS Status = STATUS_SUCCESS;
    LONG InformationLength = sizeof(FILE_BASIC_INFORMATION);
    LARGE_INTEGER Time;

    DbgPrint("FuseQueryBasicInfo\n");

    //
    //  First check if there is enough space to write the information to the buffer
    //

    if(*Length < InformationLength) {

        Status = STATUS_BUFFER_OVERFLOW;
    } else {

        RtlZeroMemory(Buffer, InformationLength);

        KeQuerySystemTime(&Time);

        Buffer->FileAttributes = FILE_ATTRIBUTE_NORMAL;
        Buffer->ChangeTime = Time;
        Buffer->CreationTime = Time;
        Buffer->LastAccessTime = Time;
        Buffer->LastWriteTime = Time;

        *Length -= InformationLength;
    }

    return Status;
}

NTSTATUS
FuseQueryStandardInfo (
    IN OUT PFILE_STANDARD_INFORMATION Buffer,
    IN OUT PLONG Length
    )
{
    NTSTATUS Status = STATUS_SUCCESS;
    LONG InformationLength = sizeof(FILE_STANDARD_INFORMATION);
    LARGE_INTEGER FileSize;

    DbgPrint("FuseQueryStandardInfo\n");

    //
    //  First check if there is enough space to write the information to the buffer
    //

    if(*Length < InformationLength) {

        Status = STATUS_BUFFER_OVERFLOW;
    } else {

        RtlZeroMemory(Buffer, InformationLength);

        FileSize.QuadPart = 0;

        // the size of the file as allocated on disk--for us this will just be the file size
        Buffer->AllocationSize = FileSize;

        // the position of the byte following the last byte in this file
        Buffer->EndOfFile = FileSize;

        Buffer->DeletePending = FALSE;
        Buffer->Directory = FALSE;

        // number of hard links to the file...I'm not sure that we have a way to determine
        // this. Let's set it to 1 for now
        Buffer->NumberOfLinks = 1;
    }

    return Status;
}

NTSTATUS
FuseQueryNameInfo (
    IN PIO_STACK_LOCATION IrpSp,
    IN OUT PFILE_NAME_INFORMATION Buffer,
    IN OUT PLONG Length
    )
{
    NTSTATUS Status = STATUS_SUCCESS;
    WCHAR* FileName = IrpSp->FileObject->FileName.Buffer;
    ULONG FileNameLength = IrpSp->FileObject->FileName.Length;
    LONG InformationLength = sizeof(WCHAR) * (FileNameLength + 1) + sizeof(ULONG);

    DbgPrint("FuseQueryNameInfo\n");

    //
    //  First check if there is enough space to write the information to the buffer
    //

    if(*Length < InformationLength) {

        Status = STATUS_BUFFER_OVERFLOW;
    } else {

        RtlZeroMemory(Buffer, InformationLength);

        RtlStringCchCopyW(Buffer->FileName, sizeof(WCHAR) * (FileNameLength + 1), FileName);
        Buffer->FileNameLength = sizeof(WCHAR) * (FileNameLength + 1);
    }

    return Status;
}

NTSTATUS
FuseFsdSetInformation (
    IN PVOLUME_DEVICE_OBJECT VolumeDeviceObject,
    IN PIRP Irp
    )
{
    DbgPrint("FuseFsdSetInformation\n");
    return STATUS_SUCCESS;
}

NTSTATUS
FuseFsdFlushBuffers (
    IN PVOLUME_DEVICE_OBJECT VolumeDeviceObject,
    IN PIRP Irp
    )
{
    DbgPrint("FuseFsdFlushBuffers\n");
    return STATUS_SUCCESS;
}

NTSTATUS
FuseFsdLockControl (
    IN PVOLUME_DEVICE_OBJECT VolumeDeviceObject,
    IN PIRP Irp
    )
{
    DbgPrint("FuseFsdLockControl\n");
    return STATUS_SUCCESS;
}

NTSTATUS
FuseFsdPnp (
    IN PVOLUME_DEVICE_OBJECT VolumeDeviceObject,
    IN PIRP Irp
    )
{
    DbgPrint("FuseFsdPnp\n");
    return STATUS_SUCCESS;
}

NTSTATUS
FuseFsdShutdown (
    IN PVOLUME_DEVICE_OBJECT VolumeDeviceObject,
    IN PIRP Irp
    )
{
    DbgPrint("FuseFsdShutdown\n");
    return STATUS_SUCCESS;
}

NTSTATUS
FuseFsdQueryVolumeInformation (
    IN PVOLUME_DEVICE_OBJECT VolumeDeviceObject,
    IN PIRP Irp
    )
{
    NTSTATUS Status = STATUS_SUCCESS;
    PIO_STACK_LOCATION IrpSp;

    LONG Length;
    FS_INFORMATION_CLASS FsInformationClass;
    PVOID Buffer;
    ULONG VolumeLabelLength;
    LARGE_INTEGER Time;

    DbgPrint("FuseFsdQueryVolumeInformation\n");

    //
    //  Get the current stack location
    //

    IrpSp = IoGetCurrentIrpStackLocation( Irp );
    
    //
    //  Reference our input parameters to make things easier
    //

    Length = IrpSp->Parameters.QueryVolume.Length;
    FsInformationClass = IrpSp->Parameters.QueryVolume.FsInformationClass;
    Buffer = Irp->AssociatedIrp.SystemBuffer;

    switch (FsInformationClass) {

    case FileFsVolumeInformation:

        Status = FuseQueryFsVolumeInfo( IrpSp, (PFILE_FS_VOLUME_INFORMATION) Buffer, &Length );
        break;
        /*
    case FileFsSizeInformation:

        Status = FuseQueryFsSizeInfo( IrpSp, (PFILE_FS_SIZE_INFORMATION) Buffer, &Length );
        break;

    case FileFsDeviceInformation:

        Status = FuseQueryFsDeviceInfo( IrpSp, (PFILE_FS_DEVICE_INFORMATION) Buffer, &Length );
        break;

    case FileFsAttributeInformation:

        Status = FuseQueryFsAttributeInfo( IrpSp, (PFILE_FS_ATTRIBUTE_INFORMATION) Buffer, &Length );
        break;

    case FileFsFullSizeInformation:

        Status = FuseQueryFsFullSizeInfo( IrpSp, (PFILE_FS_FULL_SIZE_INFORMATION) Buffer, &Length );
        break;
        */
    default:

        Status = STATUS_INVALID_PARAMETER;
        break;
    }

    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    
    return Status;
}

NTSTATUS
FuseQueryFsVolumeInfo (
    IN PIO_STACK_LOCATION IrpSp,
    IN OUT PFILE_FS_VOLUME_INFORMATION Buffer,
    IN OUT PLONG Length
    )
{
    NTSTATUS Status = STATUS_SUCCESS;
    LONG InformationLength = sizeof(FILE_FS_VOLUME_INFORMATION);
    LARGE_INTEGER Time;
    WCHAR* FileName = IrpSp->FileObject->FileName.Buffer;
    ULONG FileNameLength = IrpSp->FileObject->FileName.Length;
    WCHAR* VolumeLabel = FileName;
    ULONG VolumeLabelLength = FileNameLength;

    DbgPrint("FuseQueryFsVolumeInfo\n");

    //
    //  First check if there is enough space to write the information to the buffer
    //

    if(*Length < InformationLength) {

        Status = STATUS_BUFFER_OVERFLOW;
    } else {

        RtlZeroMemory(Buffer, InformationLength);

        //
        //  Copy the name of the module as the volume label, stripping out the initial backslash
        //
    
        if(FileNameLength >= 1) {
            ULONG ModuleNameLength = 0;
            WCHAR* BackslashPosition;

            VolumeLabel ++;
            VolumeLabelLength --;
            BackslashPosition = wcsstr(VolumeLabel, L"\\");

            if(BackslashPosition) {
                VolumeLabelLength = BackslashPosition - VolumeLabel;
            }
        }

        Buffer->VolumeLabelLength = sizeof(WCHAR) * (VolumeLabelLength + 1);
        RtlStringCchCopyW(Buffer->VolumeLabel, VolumeLabelLength, VolumeLabel);
        Buffer->VolumeSerialNumber = 0x1337;

        KeQuerySystemTime(&Time);
        Buffer->VolumeCreationTime = Time;
        Buffer->SupportsObjects = FALSE;
    }

    return Status;
}

NTSTATUS
FuseQueryFsSizeInfo (
    IN PIO_STACK_LOCATION IrpSp,
    IN OUT PFILE_FS_SIZE_INFORMATION Buffer,
    IN OUT PLONG Length
    )
{
    NTSTATUS Status = STATUS_SUCCESS;
    LONG InformationLength = sizeof(FILE_FS_SIZE_INFORMATION);

    //
    //  First check if there is enough space to write the information to the buffer
    //

    if(*Length < InformationLength) {

        Status = STATUS_BUFFER_OVERFLOW;
    } else {

        RtlZeroMemory(Buffer, InformationLength);

        
    }

    return Status;
}

NTSTATUS
FuseQueryFsDeviceInfo (
    IN PIO_STACK_LOCATION IrpSp,
    IN OUT PFILE_FS_DEVICE_INFORMATION Buffer,
    IN OUT PLONG Length
    )
{
    NTSTATUS Status = STATUS_SUCCESS;
    LONG InformationLength = sizeof(FILE_FS_DEVICE_INFORMATION);

    //
    //  First check if there is enough space to write the information to the buffer
    //

    if(*Length < InformationLength) {

        Status = STATUS_BUFFER_OVERFLOW;
    } else {

        RtlZeroMemory(Buffer, InformationLength);

        
    }

    return Status;
}

NTSTATUS
FuseQueryFsAttributeInfo (
    IN PIO_STACK_LOCATION IrpSp,
    IN OUT PFILE_FS_ATTRIBUTE_INFORMATION Buffer,
    IN OUT PLONG Length
    )
{
    NTSTATUS Status = STATUS_SUCCESS;
    LONG InformationLength = sizeof(FILE_FS_ATTRIBUTE_INFORMATION);

    //
    //  First check if there is enough space to write the information to the buffer
    //

    if(*Length < InformationLength) {

        Status = STATUS_BUFFER_OVERFLOW;
    } else {

        RtlZeroMemory(Buffer, InformationLength);

        
    }

    return Status;
}

NTSTATUS
FuseQueryFsFullSizeInfo (
    IN PIO_STACK_LOCATION IrpSp,
    IN OUT PFILE_FS_FULL_SIZE_INFORMATION Buffer,
    IN OUT PLONG Length
    )
{
    NTSTATUS Status = STATUS_SUCCESS;
    LONG InformationLength = sizeof(FILE_FS_FULL_SIZE_INFORMATION);

    //
    //  First check if there is enough space to write the information to the buffer
    //

    if(*Length < InformationLength) {

        Status = STATUS_BUFFER_OVERFLOW;
    } else {

        RtlZeroMemory(Buffer, InformationLength);

        
    }

    return Status;
}

NTSTATUS
FuseFsdSetVolumeInformation (
    IN PVOLUME_DEVICE_OBJECT VolumeDeviceObject,
    IN PIRP Irp
    )
{
    DbgPrint("FuseFsdSetVolumeInformation\n");
    return STATUS_SUCCESS;
}

BOOLEAN
FuseFastIoCheckIfPossible (
    IN PFILE_OBJECT FileObject,
    IN PLARGE_INTEGER FileOffset,
    IN ULONG Length,
    IN BOOLEAN Wait,
    IN ULONG LockKey,
    IN BOOLEAN CheckForReadOperation,
    OUT PIO_STATUS_BLOCK IoStatus,
    IN PDEVICE_OBJECT DeviceObject
    )
{
    DbgPrint("FuseFastIoCheckIfPossible\n");
    return FALSE;
}

BOOLEAN
FuseFastQueryBasicInfo (
    IN PFILE_OBJECT FileObject,
    IN BOOLEAN Wait,
    IN OUT PFILE_BASIC_INFORMATION Buffer,
    OUT PIO_STATUS_BLOCK IoStatus,
    IN PDEVICE_OBJECT DeviceObject
    )
{
    LONG Length = sizeof(FILE_BASIC_INFORMATION);

    DbgPrint("FuseFastQueryBasicInfo\n");

    return FuseQueryBasicInfo(Buffer, &Length) == STATUS_SUCCESS;
}

BOOLEAN
FuseFastQueryStdInfo (
    IN PFILE_OBJECT FileObject,
    IN BOOLEAN Wait,
    IN OUT PFILE_STANDARD_INFORMATION Buffer,
    OUT PIO_STATUS_BLOCK IoStatus,
    IN PDEVICE_OBJECT DeviceObject
    )
{
    DbgPrint("FuseFastQueryStdInfo\n");
    return FALSE;
}

BOOLEAN
FuseFastQueryNetworkOpenInfo (
    IN PFILE_OBJECT FileObject,
    IN BOOLEAN Wait,
    IN OUT PFILE_NETWORK_OPEN_INFORMATION Buffer,
    OUT PIO_STATUS_BLOCK IoStatus,
    IN PDEVICE_OBJECT DeviceObject
    )
{
    DbgPrint("FuseFastQueryNetworkOpenInfo\n");
    return FALSE;
}

BOOLEAN
FuseFastLock (
    IN PFILE_OBJECT FileObject,
    IN PLARGE_INTEGER FileOffset,
    IN PLARGE_INTEGER Length,
    PEPROCESS ProcessId,
    ULONG Key,
    BOOLEAN FailImmediately,
    BOOLEAN ExclusiveLock,
    OUT PIO_STATUS_BLOCK IoStatus,
    IN PDEVICE_OBJECT DeviceObject
    )
{
    DbgPrint("FuseFastLock\n");
    return FALSE;
}

BOOLEAN
FuseFastUnlockSingle (
    IN PFILE_OBJECT FileObject,
    IN PLARGE_INTEGER FileOffset,
    IN PLARGE_INTEGER Length,
    PEPROCESS ProcessId,
    ULONG Key,
    OUT PIO_STATUS_BLOCK IoStatus,
    IN PDEVICE_OBJECT DeviceObject
    )
{
    DbgPrint("FuseFastUnlockSingle\n");
    return FALSE;
}

BOOLEAN
FuseFastUnlockAll (
    IN PFILE_OBJECT FileObject,
    PEPROCESS ProcessId,
    OUT PIO_STATUS_BLOCK IoStatus,
    IN PDEVICE_OBJECT DeviceObject
    )
{
    DbgPrint("FuseFastUnlockAll\n");
    return FALSE;
}

BOOLEAN
FuseFastUnlockAllByKey (
    IN PFILE_OBJECT FileObject,
    PVOID ProcessId,
    ULONG Key,
    OUT PIO_STATUS_BLOCK IoStatus,
    IN PDEVICE_OBJECT DeviceObject
    )
{
    DbgPrint("FuseFastUnlockAllByKey\n");
    return FALSE;
}

NTSTATUS
FuseAcquireForCcFlush (
    IN PFILE_OBJECT FileObject,
    IN PDEVICE_OBJECT DeviceObject
    )
{
    DbgPrint("FuseAcquireForCcFlush\n");
    return STATUS_SUCCESS;
}

NTSTATUS
FuseReleaseForCcFlush (
    IN PFILE_OBJECT FileObject,
    IN PDEVICE_OBJECT DeviceObject
    )
{
    DbgPrint("FuseReleaseForCcFlush\n");
    return STATUS_SUCCESS;
}

