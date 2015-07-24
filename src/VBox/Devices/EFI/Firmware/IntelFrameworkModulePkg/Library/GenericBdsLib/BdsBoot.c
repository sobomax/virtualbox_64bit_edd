/** @file
  BDS Lib functions which relate with create or process the boot option.

Copyright (c) 2004 - 2012, Intel Corporation. All rights reserved.<BR>
This program and the accompanying materials
are licensed and made available under the terms and conditions of the BSD License
which accompanies this distribution.  The full text of the license may be found at
http://opensource.org/licenses/bsd-license.php

THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#include "InternalBdsLib.h"
#include "String.h"

BOOLEAN mEnumBootDevice = FALSE;
EFI_HII_HANDLE gBdsLibStringPackHandle = NULL;

#ifdef VBOX
/** Array of EFI boot files.  */
CHAR16 CONST *maBdsLibBootFiles[] = {
  VBOX_EFI_APPLE_MEDIA_FILE_NAME,
  EFI_REMOVABLE_MEDIA_FILE_NAME,
};
#endif

/**
  The constructor function register UNI strings into imageHandle.

  It will ASSERT() if that operation fails and it will always return EFI_SUCCESS.

  @param  ImageHandle   The firmware allocated handle for the EFI image.
  @param  SystemTable   A pointer to the EFI System Table.

  @retval EFI_SUCCESS   The constructor successfully added string package.
  @retval Other value   The constructor can't add string package.

**/
EFI_STATUS
EFIAPI
GenericBdsLibConstructor (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{

  gBdsLibStringPackHandle = HiiAddPackages (
                              &gBdsLibStringPackageGuid,
                              &ImageHandle,
                              GenericBdsLibStrings,
                              NULL
                              );

  ASSERT (gBdsLibStringPackHandle != NULL);

  return EFI_SUCCESS;
}



/**
  Boot the legacy system with the boot option

  @param  Option                 The legacy boot option which have BBS device path

  @retval EFI_UNSUPPORTED        There is no legacybios protocol, do not support
                                 legacy boot.
  @retval EFI_STATUS             Return the status of LegacyBios->LegacyBoot ().

**/
EFI_STATUS
BdsLibDoLegacyBoot (
  IN  BDS_COMMON_OPTION           *Option
  )
{
  EFI_STATUS                Status;
  EFI_LEGACY_BIOS_PROTOCOL  *LegacyBios;

  Status = gBS->LocateProtocol (&gEfiLegacyBiosProtocolGuid, NULL, (VOID **) &LegacyBios);
  if (EFI_ERROR (Status)) {
    //
    // If no LegacyBios protocol we do not support legacy boot
    //
    return EFI_UNSUPPORTED;
  }
  //
  // Notes: if we separate the int 19, then we don't need to refresh BBS
  //
  BdsRefreshBbsTableForBoot (Option);

  //
  // Write boot to OS performance data for legacy boot.
  //
  PERF_CODE (
    WriteBootToOsPerformanceData ();
  );

  DEBUG ((DEBUG_INFO | DEBUG_LOAD, "Legacy Boot: %S\n", Option->Description));
  return LegacyBios->LegacyBoot (
                      LegacyBios,
                      (BBS_BBS_DEVICE_PATH *) Option->DevicePath,
                      Option->LoadOptionsSize,
                      Option->LoadOptions
                      );
}

/**
  Internal function to check if the input boot option is a valid EFI NV Boot####.

  @param OptionToCheck  Boot option to be checked.

  @retval TRUE      This boot option matches a valid EFI NV Boot####.
  @retval FALSE     If not.

**/
BOOLEAN
IsBootOptionValidNVVarialbe (
  IN  BDS_COMMON_OPTION             *OptionToCheck
  )
{
  LIST_ENTRY        TempList;
  BDS_COMMON_OPTION *BootOption;
  BOOLEAN           Valid;
  CHAR16            OptionName[20];

  Valid = FALSE;

  InitializeListHead (&TempList);
  UnicodeSPrint (OptionName, sizeof (OptionName), L"Boot%04x", OptionToCheck->BootCurrent);

  BootOption = BdsLibVariableToOption (&TempList, OptionName);
  if (BootOption == NULL) {
    return FALSE;
  }

  //
  // If the Boot Option Number and Device Path matches, OptionToCheck matches a
  // valid EFI NV Boot####.
  //
  if ((OptionToCheck->BootCurrent == BootOption->BootCurrent) &&
      (CompareMem (OptionToCheck->DevicePath, BootOption->DevicePath, GetDevicePathSize (OptionToCheck->DevicePath)) == 0))
      {
    Valid = TRUE;
  }

#ifdef VBOX
  FreePool (BootOption->Description);
  FreePool (BootOption->DevicePath);
  FreePool (BootOption->LoadOptions);
  FreePool (BootOption->OptionName);
#endif
  FreePool (BootOption);

  return Valid;
}

/**
  Check whether a USB device match the specified USB Class device path. This
  function follows "Load Option Processing" behavior in UEFI specification.

  @param UsbIo       USB I/O protocol associated with the USB device.
  @param UsbClass    The USB Class device path to match.

  @retval TRUE       The USB device match the USB Class device path.
  @retval FALSE      The USB device does not match the USB Class device path.

**/
BOOLEAN
BdsMatchUsbClass (
  IN EFI_USB_IO_PROTOCOL        *UsbIo,
  IN USB_CLASS_DEVICE_PATH      *UsbClass
  )
{
  EFI_STATUS                    Status;
  EFI_USB_DEVICE_DESCRIPTOR     DevDesc;
  EFI_USB_INTERFACE_DESCRIPTOR  IfDesc;
  UINT8                         DeviceClass;
  UINT8                         DeviceSubClass;
  UINT8                         DeviceProtocol;

  if ((DevicePathType (UsbClass) != MESSAGING_DEVICE_PATH) ||
      (DevicePathSubType (UsbClass) != MSG_USB_CLASS_DP)){
    return FALSE;
  }

  //
  // Check Vendor Id and Product Id.
  //
  Status = UsbIo->UsbGetDeviceDescriptor (UsbIo, &DevDesc);
  if (EFI_ERROR (Status)) {
    return FALSE;
  }

  if ((UsbClass->VendorId != 0xffff) &&
      (UsbClass->VendorId != DevDesc.IdVendor)) {
    return FALSE;
  }

  if ((UsbClass->ProductId != 0xffff) &&
      (UsbClass->ProductId != DevDesc.IdProduct)) {
    return FALSE;
  }

  DeviceClass    = DevDesc.DeviceClass;
  DeviceSubClass = DevDesc.DeviceSubClass;
  DeviceProtocol = DevDesc.DeviceProtocol;
  if (DeviceClass == 0) {
    //
    // If Class in Device Descriptor is set to 0, use the Class, SubClass and
    // Protocol in Interface Descriptor instead.
    //
    Status = UsbIo->UsbGetInterfaceDescriptor (UsbIo, &IfDesc);
    if (EFI_ERROR (Status)) {
      return FALSE;
    }

    DeviceClass    = IfDesc.InterfaceClass;
    DeviceSubClass = IfDesc.InterfaceSubClass;
    DeviceProtocol = IfDesc.InterfaceProtocol;
  }

  //
  // Check Class, SubClass and Protocol.
  //
  if ((UsbClass->DeviceClass != 0xff) &&
      (UsbClass->DeviceClass != DeviceClass)) {
    return FALSE;
  }

  if ((UsbClass->DeviceSubClass != 0xff) &&
      (UsbClass->DeviceSubClass != DeviceSubClass)) {
    return FALSE;
  }

  if ((UsbClass->DeviceProtocol != 0xff) &&
      (UsbClass->DeviceProtocol != DeviceProtocol)) {
    return FALSE;
  }

  return TRUE;
}

/**
  Check whether a USB device match the specified USB WWID device path. This
  function follows "Load Option Processing" behavior in UEFI specification.

  @param UsbIo       USB I/O protocol associated with the USB device.
  @param UsbWwid     The USB WWID device path to match.

  @retval TRUE       The USB device match the USB WWID device path.
  @retval FALSE      The USB device does not match the USB WWID device path.

**/
BOOLEAN
BdsMatchUsbWwid (
  IN EFI_USB_IO_PROTOCOL        *UsbIo,
  IN USB_WWID_DEVICE_PATH       *UsbWwid
  )
{
  EFI_STATUS                   Status;
  EFI_USB_DEVICE_DESCRIPTOR    DevDesc;
  EFI_USB_INTERFACE_DESCRIPTOR IfDesc;
  UINT16                       *LangIdTable;
  UINT16                       TableSize;
  UINT16                       Index;
  CHAR16                       *CompareStr;
  UINTN                        CompareLen;
  CHAR16                       *SerialNumberStr;
  UINTN                        Length;

  if ((DevicePathType (UsbWwid) != MESSAGING_DEVICE_PATH) ||
      (DevicePathSubType (UsbWwid) != MSG_USB_WWID_DP )){
    return FALSE;
  }

  //
  // Check Vendor Id and Product Id.
  //
  Status = UsbIo->UsbGetDeviceDescriptor (UsbIo, &DevDesc);
  if (EFI_ERROR (Status)) {
    return FALSE;
  }
  if ((DevDesc.IdVendor != UsbWwid->VendorId) ||
      (DevDesc.IdProduct != UsbWwid->ProductId)) {
    return FALSE;
  }

  //
  // Check Interface Number.
  //
  Status = UsbIo->UsbGetInterfaceDescriptor (UsbIo, &IfDesc);
  if (EFI_ERROR (Status)) {
    return FALSE;
  }
  if (IfDesc.InterfaceNumber != UsbWwid->InterfaceNumber) {
    return FALSE;
  }

  //
  // Check Serial Number.
  //
  if (DevDesc.StrSerialNumber == 0) {
    return FALSE;
  }

  //
  // Get all supported languages.
  //
  TableSize = 0;
  LangIdTable = NULL;
  Status = UsbIo->UsbGetSupportedLanguages (UsbIo, &LangIdTable, &TableSize);
  if (EFI_ERROR (Status) || (TableSize == 0) || (LangIdTable == NULL)) {
    return FALSE;
  }

  //
  // Serial number in USB WWID device path is the last 64-or-less UTF-16 characters.
  //
  CompareStr = (CHAR16 *) (UINTN) (UsbWwid + 1);
  CompareLen = (DevicePathNodeLength (UsbWwid) - sizeof (USB_WWID_DEVICE_PATH)) / sizeof (CHAR16);
  if (CompareStr[CompareLen - 1] == L'\0') {
    CompareLen--;
  }

  //
  // Compare serial number in each supported language.
  //
  for (Index = 0; Index < TableSize / sizeof (UINT16); Index++) {
    SerialNumberStr = NULL;
    Status = UsbIo->UsbGetStringDescriptor (
                      UsbIo,
                      LangIdTable[Index],
                      DevDesc.StrSerialNumber,
                      &SerialNumberStr
                      );
    if (EFI_ERROR (Status) || (SerialNumberStr == NULL)) {
      continue;
    }

    Length = StrLen (SerialNumberStr);
    if ((Length >= CompareLen) &&
        (CompareMem (SerialNumberStr + Length - CompareLen, CompareStr, CompareLen * sizeof (CHAR16)) == 0)) {
      FreePool (SerialNumberStr);
      return TRUE;
    }

    FreePool (SerialNumberStr);
  }

  return FALSE;
}

/**
  Find a USB device path which match the specified short-form device path start
  with USB Class or USB WWID device path and load the boot file then return the
  image handle. If ParentDevicePath is NULL, this function will search in all USB
  devices of the platform. If ParentDevicePath is not NULL,this function will only
  search in its child devices.

  @param ParentDevicePath      The device path of the parent.
  @param ShortFormDevicePath   The USB Class or USB WWID device path to match.

  @return  The image Handle if find load file from specified short-form device path
           or NULL if not found.

**/
EFI_HANDLE *
BdsFindUsbDevice (
  IN EFI_DEVICE_PATH_PROTOCOL   *ParentDevicePath,
  IN EFI_DEVICE_PATH_PROTOCOL   *ShortFormDevicePath
  )
{
  EFI_STATUS                Status;
  UINTN                     UsbIoHandleCount;
  EFI_HANDLE                *UsbIoHandleBuffer;
  EFI_DEVICE_PATH_PROTOCOL  *UsbIoDevicePath;
  EFI_USB_IO_PROTOCOL       *UsbIo;
  UINTN                     Index;
  UINTN                     ParentSize;
  UINTN                     Size;
  EFI_HANDLE                ImageHandle;
  EFI_HANDLE                Handle;
  EFI_DEVICE_PATH_PROTOCOL  *FullDevicePath;
  EFI_DEVICE_PATH_PROTOCOL  *NextDevicePath;

  FullDevicePath = NULL;
  ImageHandle    = NULL;

  //
  // Get all UsbIo Handles.
  //
  UsbIoHandleCount = 0;
  UsbIoHandleBuffer = NULL;
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiUsbIoProtocolGuid,
                  NULL,
                  &UsbIoHandleCount,
                  &UsbIoHandleBuffer
                  );
  if (EFI_ERROR (Status) || (UsbIoHandleCount == 0) || (UsbIoHandleBuffer == NULL)) {
    return NULL;
  }

  ParentSize = (ParentDevicePath == NULL) ? 0 : GetDevicePathSize (ParentDevicePath);
  for (Index = 0; Index < UsbIoHandleCount; Index++) {
    //
    // Get the Usb IO interface.
    //
    Status = gBS->HandleProtocol(
                    UsbIoHandleBuffer[Index],
                    &gEfiUsbIoProtocolGuid,
                    (VOID **) &UsbIo
                    );
    if (EFI_ERROR (Status)) {
      continue;
    }

    UsbIoDevicePath = DevicePathFromHandle (UsbIoHandleBuffer[Index]);
    if (UsbIoDevicePath == NULL) {
      continue;
    }

    if (ParentDevicePath != NULL) {
      //
      // Compare starting part of UsbIoHandle's device path with ParentDevicePath.
      //
      Size = GetDevicePathSize (UsbIoDevicePath);
      if ((Size < ParentSize) ||
          (CompareMem (UsbIoDevicePath, ParentDevicePath, ParentSize - END_DEVICE_PATH_LENGTH) != 0)) {
        continue;
      }
    }

    if (BdsMatchUsbClass (UsbIo, (USB_CLASS_DEVICE_PATH *) ShortFormDevicePath) ||
        BdsMatchUsbWwid (UsbIo, (USB_WWID_DEVICE_PATH *) ShortFormDevicePath)) {
      //
      // Try to find if there is the boot file in this DevicePath
      //
      NextDevicePath = NextDevicePathNode (ShortFormDevicePath);
      if (!IsDevicePathEnd (NextDevicePath)) {
        FullDevicePath = AppendDevicePath (UsbIoDevicePath, NextDevicePath);
        //
        // Connect the full device path, so that Simple File System protocol
        // could be installed for this USB device.
        //
        BdsLibConnectDevicePath (FullDevicePath);
        REPORT_STATUS_CODE (EFI_PROGRESS_CODE, PcdGet32 (PcdProgressCodeOsLoaderLoad));
        Status = gBS->LoadImage (
                       TRUE,
                       gImageHandle,
                       FullDevicePath,
                       NULL,
                       0,
                       &ImageHandle
                       );
        FreePool (FullDevicePath);
      } else {
        FullDevicePath = UsbIoDevicePath;
        Status = EFI_NOT_FOUND;
      }

      //
      // If we didn't find an image directly, we need to try as if it is a removable device boot option
      // and load the image according to the default boot behavior for removable device.
      //
      if (EFI_ERROR (Status)) {
        //
        // check if there is a bootable removable media could be found in this device path ,
        // and get the bootable media handle
        //
        Handle = BdsLibGetBootableHandle(UsbIoDevicePath);
        if (Handle == NULL) {
          continue;
        }
        //
        // Load the default boot file \EFI\BOOT\boot{machinename}.EFI from removable Media
        //  machinename is ia32, ia64, x64, ...
        //
        VBoxLogFlowFuncMark();
#ifdef VBOX /* We look for serveral files because of apple. */
        {
          INT32 IdxFile;
          for (IdxFile = 0; IdxFile < sizeof(maBdsLibBootFiles) / sizeof(maBdsLibBootFiles[0]); IdxFile++) {
            FullDevicePath = FileDevicePath(Handle, maBdsLibBootFiles[IdxFile]);
            if (FullDevicePath != NULL) {
              Status = gBS->LoadImage(TRUE, gImageHandle, FullDevicePath, NULL, 0, &ImageHandle);
              if (!EFI_ERROR(Status)) {
                break;
              }
            }
          }
          if (!EFI_ERROR(Status)) {
            continue;
          }
        }
#else  /* !VBOX */
        FullDevicePath = FileDevicePath (Handle, EFI_REMOVABLE_MEDIA_FILE_NAME);
        if (FullDevicePath != NULL) {
          REPORT_STATUS_CODE (EFI_PROGRESS_CODE, PcdGet32 (PcdProgressCodeOsLoaderLoad));
          Status = gBS->LoadImage (
                          TRUE,
                          gImageHandle,
                          FullDevicePath,
                          NULL,
                          0,
                          &ImageHandle
                          );
          if (EFI_ERROR (Status)) {
            //
            // The DevicePath failed, and it's not a valid
            // removable media device.
            //
            continue;
          }
        } else {
          continue;
        }
#endif /* !VBOX */
      }
      break;
    }
  }

  FreePool (UsbIoHandleBuffer);
  return ImageHandle;
}

/**
  Expand USB Class or USB WWID device path node to be full device path of a USB
  device in platform then load the boot file on this full device path and return the
  image handle.

  This function support following 4 cases:
  1) Boot Option device path starts with a USB Class or USB WWID device path,
     and there is no Media FilePath device path in the end.
     In this case, it will follow Removable Media Boot Behavior.
  2) Boot Option device path starts with a USB Class or USB WWID device path,
     and ended with Media FilePath device path.
  3) Boot Option device path starts with a full device path to a USB Host Controller,
     contains a USB Class or USB WWID device path node, while not ended with Media
     FilePath device path. In this case, it will follow Removable Media Boot Behavior.
  4) Boot Option device path starts with a full device path to a USB Host Controller,
     contains a USB Class or USB WWID device path node, and ended with Media
     FilePath device path.

  @param  DevicePath    The Boot Option device path.

  @return  The image handle of boot file, or NULL if there is no boot file found in
           the specified USB Class or USB WWID device path.

**/
EFI_HANDLE *
BdsExpandUsbShortFormDevicePath (
  IN EFI_DEVICE_PATH_PROTOCOL       *DevicePath
  )
{
  EFI_HANDLE                *ImageHandle;
  EFI_DEVICE_PATH_PROTOCOL  *TempDevicePath;
  EFI_DEVICE_PATH_PROTOCOL  *ShortFormDevicePath;

  //
  // Search for USB Class or USB WWID device path node.
  //
  ShortFormDevicePath = NULL;
  ImageHandle         = NULL;
  TempDevicePath      = DevicePath;
  while (!IsDevicePathEnd (TempDevicePath)) {
    if ((DevicePathType (TempDevicePath) == MESSAGING_DEVICE_PATH) &&
        ((DevicePathSubType (TempDevicePath) == MSG_USB_CLASS_DP) ||
         (DevicePathSubType (TempDevicePath) == MSG_USB_WWID_DP))) {
      ShortFormDevicePath = TempDevicePath;
      break;
    }
    TempDevicePath = NextDevicePathNode (TempDevicePath);
  }

  if (ShortFormDevicePath == NULL) {
    //
    // No USB Class or USB WWID device path node found, do nothing.
    //
    return NULL;
  }

  if (ShortFormDevicePath == DevicePath) {
    //
    // Boot Option device path starts with USB Class or USB WWID device path.
    //
    ImageHandle = BdsFindUsbDevice (NULL, ShortFormDevicePath);
    if (ImageHandle == NULL) {
      //
      // Failed to find a match in existing devices, connect the short form USB
      // device path and try again.
      //
      BdsLibConnectUsbDevByShortFormDP (0xff, ShortFormDevicePath);
      ImageHandle = BdsFindUsbDevice (NULL, ShortFormDevicePath);
    }
  } else {
    //
    // Boot Option device path contains USB Class or USB WWID device path node.
    //

    //
    // Prepare the parent device path for search.
    //
    TempDevicePath = DuplicateDevicePath (DevicePath);
    ASSERT (TempDevicePath != NULL);
    SetDevicePathEndNode (((UINT8 *) TempDevicePath) + ((UINTN) ShortFormDevicePath - (UINTN) DevicePath));

    //
    // The USB Host Controller device path is already in Boot Option device path
    // and USB Bus driver already support RemainingDevicePath starts with USB
    // Class or USB WWID device path, so just search in existing USB devices and
    // doesn't perform ConnectController here.
    //
    ImageHandle = BdsFindUsbDevice (TempDevicePath, ShortFormDevicePath);
    FreePool (TempDevicePath);
  }

  return ImageHandle;
}

/**
  Process the boot option follow the UEFI specification and
  special treat the legacy boot option with BBS_DEVICE_PATH.

  @param  Option                 The boot option need to be processed
  @param  DevicePath             The device path which describe where to load the
                                 boot image or the legacy BBS device path to boot
                                 the legacy OS
  @param  ExitDataSize           The size of exit data.
  @param  ExitData               Data returned when Boot image failed.

  @retval EFI_SUCCESS            Boot from the input boot option successfully.
  @retval EFI_NOT_FOUND          If the Device Path is not found in the system

**/
EFI_STATUS
EFIAPI
BdsLibBootViaBootOption (
  IN  BDS_COMMON_OPTION             *Option,
  IN  EFI_DEVICE_PATH_PROTOCOL      *DevicePath,
  OUT UINTN                         *ExitDataSize,
  OUT CHAR16                        **ExitData OPTIONAL
  )
{
  EFI_STATUS                Status;
  EFI_STATUS                StatusLogo;
  EFI_HANDLE                Handle;
  EFI_HANDLE                ImageHandle;
  EFI_DEVICE_PATH_PROTOCOL  *FilePath;
  EFI_LOADED_IMAGE_PROTOCOL *ImageInfo;
  EFI_DEVICE_PATH_PROTOCOL  *WorkingDevicePath;
  EFI_ACPI_S3_SAVE_PROTOCOL *AcpiS3Save;
  LIST_ENTRY                TempBootLists;
  EFI_BOOT_LOGO_PROTOCOL    *BootLogo;

  //
  // Record the performance data for End of BDS
  //
  PERF_END(NULL, "BDS", NULL, 0);

  VBoxLog(("BdsLibBootViaBootOption: BootCurrent=0x%x OptionName=%s Description=%s StatusString=%s Attribute=0x%x LoadOptionsSize=0x%x\n",
           Option->BootCurrent, Option->OptionName, Option->Description, Option->StatusString, Option->Attribute, Option->LoadOptionsSize));

  *ExitDataSize = 0;
  *ExitData     = NULL;

  //
  // Notes: this code can be remove after the s3 script table
  // hook on the event EVT_SIGNAL_READY_TO_BOOT or
  // EVT_SIGNAL_LEGACY_BOOT
  //
  Status = gBS->LocateProtocol (&gEfiAcpiS3SaveProtocolGuid, NULL, (VOID **) &AcpiS3Save);
  if (!EFI_ERROR (Status)) {
    AcpiS3Save->S3Save (AcpiS3Save, NULL);
  }
  //
  // If it's Device Path that starts with a hard drive path, append it with the front part to compose a
  // full device path
  //
  WorkingDevicePath = NULL;
  if ((DevicePathType (DevicePath) == MEDIA_DEVICE_PATH) &&
      (DevicePathSubType (DevicePath) == MEDIA_HARDDRIVE_DP)) {
    WorkingDevicePath = BdsExpandPartitionPartialDevicePathToFull (
                          (HARDDRIVE_DEVICE_PATH *)DevicePath
                          );
    if (WorkingDevicePath != NULL) {
      DevicePath = WorkingDevicePath;
    }
  }

  //
  // Set Boot Current
  //
  if (IsBootOptionValidNVVarialbe (Option)) {
    //
    // For a temporary boot (i.e. a boot by selected a EFI Shell using "Boot From File"), Boot Current is actually not valid.
    // In this case, "BootCurrent" is not created.
    // Only create the BootCurrent variable when it points to a valid Boot#### variable.
    //
    gRT->SetVariable (
          L"BootCurrent",
          &gEfiGlobalVariableGuid,
          EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
          sizeof (UINT16),
          &Option->BootCurrent
          );
  }

  //
  // Signal the EVT_SIGNAL_READY_TO_BOOT event
  //
  EfiSignalEventReadyToBoot();

  //
  // Expand USB Class or USB WWID device path node to be full device path of a USB
  // device in platform then load the boot file on this full device path and get the
  // image handle.
  //
  ImageHandle = BdsExpandUsbShortFormDevicePath (DevicePath);

  //
  // Adjust the different type memory page number just before booting
  // and save the updated info into the variable for next boot to use
  //
  BdsSetMemoryTypeInformationVariable ();

  //
  // By expanding the USB Class or WWID device path, the ImageHandle has returnned.
  // Here get the ImageHandle for the non USB class or WWID device path.
  //
  if (ImageHandle == NULL) {
    ASSERT (Option->DevicePath != NULL);
    if ((DevicePathType (Option->DevicePath) == BBS_DEVICE_PATH) &&
        (DevicePathSubType (Option->DevicePath) == BBS_BBS_DP)
       ) {
      //
      // Check to see if we should legacy BOOT. If yes then do the legacy boot
      //
      return BdsLibDoLegacyBoot (Option);
    }

    //
    // If the boot option point to Internal FV shell, make sure it is valid
    //
    Status = BdsLibUpdateFvFileDevicePath (&DevicePath, PcdGetPtr(PcdShellFile));
    if (!EFI_ERROR(Status)) {
      if (Option->DevicePath != NULL) {
        FreePool(Option->DevicePath);
      }
      Option->DevicePath  = AllocateZeroPool (GetDevicePathSize (DevicePath));
      ASSERT(Option->DevicePath != NULL);
      CopyMem (Option->DevicePath, DevicePath, GetDevicePathSize (DevicePath));
      //
      // Update the shell boot option
      //
      InitializeListHead (&TempBootLists);
      BdsLibRegisterNewOption (&TempBootLists, DevicePath, L"EFI Internal Shell", L"BootOrder");

      //
      // free the temporary device path created by BdsLibUpdateFvFileDevicePath()
      //
      FreePool (DevicePath);
      DevicePath = Option->DevicePath;
    }

    DEBUG_CODE_BEGIN();

    if (Option->Description == NULL) {
      DEBUG ((DEBUG_INFO | DEBUG_LOAD, "Booting from unknown device path\n"));
    } else {
      DEBUG ((DEBUG_INFO | DEBUG_LOAD, "Booting %S\n", Option->Description));
    }

    DEBUG_CODE_END();

    //
    // Report status code for OS Loader LoadImage.
    //
    REPORT_STATUS_CODE (EFI_PROGRESS_CODE, PcdGet32 (PcdProgressCodeOsLoaderLoad));
    Status = gBS->LoadImage (
                    TRUE,
                    gImageHandle,
                    DevicePath,
                    NULL,
                    0,
                    &ImageHandle
                    );

    VBoxLogFlowFuncMarkRC(Status);
    //
    // If we didn't find an image directly, we need to try as if it is a removable device boot option
    // and load the image according to the default boot behavior for removable device.
    //
    if (EFI_ERROR (Status)) {
      //
      // check if there is a bootable removable media could be found in this device path ,
      // and get the bootable media handle
      //
      Handle = BdsLibGetBootableHandle(DevicePath);
      VBoxLogFlowFuncMarkVar(Handle, "%p");
      if (Handle == NULL) {
        goto Done;
      }
#ifdef VBOX /* We have more options to try out because of apple. */
      //
      // Try parse any config="folder-path/" stuff found in the optional
      // section of the boot option and attempt to load boot.efi it.
      //
      if (Option->LoadOptionsSize) {
        /*
         * First, see the there is a config=folder in the load options.  We
         * interpret it as a path to the folder containing boot.efi and go
         * about constructing the path in a bit of a round about way...
         *
         * This is what we got when Lion rebooted after the first installation stage:
         *   Variable - fAttr=0x07 - '8be4df61-93ca-11d2-aa0d-00e098032b8c:Boot0080' - cb=0xbe
         *   0000: 01 00 00 00 48 00 4d 00-61 00 63 00 20 00 4f 00 ....H.M.a.c. .O.
         *   0010: 53 00 20 00 58 00 00 00-02 01 0c 00 d0 41 03 0a S. .X........A..
         *   0020: 00 00 00 00 01 01 06 00-02 1f 03 01 08 00 00 01 ................
         *   0030: 00 00 04 01 2a 00 02 00-00 00 28 40 06 00 00 00 ....*.....(@....
         *   0040: 00 00 b0 bf 75 02 00 00-00 00 e5 b2 19 a7 cb 59 ....u..........Y
         *   0050: 69 4c b9 92 51 3c fd 4b-d7 c4 02 02 7f ff 04 00 iL..Q<.K........
         *   0060: 63 00 6f 00 6e 00 66 00-69 00 67 00 3d 00 22 00 c.o.n.f.i.g.=.".
         *   0070: 5c 00 4d 00 61 00 63 00-20 00 4f 00 53 00 20 00 \.M.a.c. .O.S. .
         *   0080: 58 00 20 00 49 00 6e 00-73 00 74 00 61 00 6c 00 X. .I.n.s.t.a.l.
         *   0090: 6c 00 20 00 44 00 61 00-74 00 61 00 5c 00 63 00 l. .D.a.t.a.\.c.
         *   00a0: 6f 00 6d 00 2e 00 61 00-70 00 70 00 6c 00 65 00 o.m...a.p.p.l.e.
         *   00b0: 2e 00 42 00 6f 00 6f 00-74 00 22 00 00 00       ..B.o.o.t."...
         *
         * This is waht we got when Moutain Kitten reboot after the first installation stage:
         * Variable - fAttr=0x07 - '8be4df61-93ca-11d2-aa0d-00e098032b8c:Boot0080' - cb=0xb6
         *   0000: 01 00 00 00 48 00 4d 00-61 00 63 00 20 00 4f 00 ....H.M.a.c. .O.
         *   0010: 53 00 20 00 58 00 00 00-02 01 0c 00 d0 41 03 0a S. .X........A..
         *   0020: 00 00 00 00 01 01 06 00-02 1f 03 01 08 00 00 01 ................
         *   0030: 00 00 04 01 2a 00 02 00-00 00 28 40 06 00 00 00 ....*.....(@....
         *   0040: 00 00 98 b5 53 02 00 00-00 00 a5 9a 97 b7 33 60 ....S.........3`
         *   0050: 2d 47 bf 1a fa 14 fe c5-f2 b8 02 02 7f ff 04 00 -G..............
         *   0060: 63 00 6f 00 6e 00 66 00-69 00 67 00 3d 00 22 00 c.o.n.f.i.g.=.".
         *   0070: 5c 00 4f 00 53 00 20 00-58 00 20 00 49 00 6e 00 \.O.S. .X. .I.n.
         *   0080: 73 00 74 00 61 00 6c 00-6c 00 20 00 44 00 61 00 s.t.a.l.l. .D.a.
         *   0090: 74 00 61 00 5c 00 63 00-6f 00 6d 00 2e 00 61 00 t.a.\.c.o.m...a.
         *   00a0: 70 00 70 00 6c 00 65 00-2e 00 42 00 6f 00 6f 00 p.p.l.e...B.o.o.
         *   00b0: 74 00 22 00 00 00                               t."...
         */
        VBoxLogFlowFuncMarkVar(Option->LoadOptions, "%s");
        if (StrnCmp(L"config=", (CHAR16 *)Option->LoadOptions, 7) == 0) {
          EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Volume;
          CHAR16 *pszConfigValue = &((CHAR16 *)Option->LoadOptions)[7];
          CHAR16 *pszFolder = NULL;
          CHAR16 *pszTemp0 = NULL;
          CHAR16 *pszTemp1 = NULL;
          BOOLEAN fQuoted = (pszConfigValue[0] == L'\"');
          pszTemp0 = &pszConfigValue[fQuoted ? 2 : 1];
          pszTemp1 = pszTemp0;
          VBoxLogFlowFuncMarkVar(pszTemp0, "%s");
          VBoxLogFlowFuncMarkVar(pszTemp1, "%s");
          pszFolder = AllocateZeroPool((StrLen(pszConfigValue) + StrLen(L"\\boot.efi") + 1) * sizeof(UINT16));
          while (pszTemp0) {
              VBoxLogFlowFuncMarkVar(pszTemp0, "%s");
              VBoxLogFlowFuncMarkVar(pszTemp1, "%s");
              pszFolder = StrnCat(pszFolder, pszTemp1, StrLen(pszTemp1) - StrLen(pszTemp0));
              pszTemp1 = pszTemp0;
              pszFolder = StrCat(pszFolder, L"\\");
              VBoxLogFlowFuncMarkVar(pszFolder, "%s");
              VBoxLogFlowFuncMarkVar(pszTemp0, "%s");
              VBoxLogFlowFuncMarkVar(pszTemp1, "%s");
              pszTemp0 = StrStr(&pszTemp0[1], L"\\");
              VBoxLogFlowFuncMarkVar(pszTemp0, "%s");
          }
          pszFolder = StrCat(pszFolder, L"boot.efi");
          VBoxLogFlowFuncMarkVar(pszFolder,"%s");

          Status = gBS->HandleProtocol(Handle, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Volume);
          VBoxLogFlowFuncMarkRC(Status);
          if (!EFI_ERROR(Status)) {
            FilePath = FileDevicePath((EFI_HANDLE)Handle, pszFolder);
            if (FilePath) {
              VBoxLogFlowFuncMarkVar(DevicePathToStr(FilePath), "%s");
              REPORT_STATUS_CODE(EFI_PROGRESS_CODE, PcdGet32 (PcdProgressCodeOsLoaderLoad));
              Status = gBS->LoadImage(TRUE, gImageHandle, FilePath, NULL, 0, &ImageHandle );
              VBoxLogFlowFuncMarkRC(Status);
              /** @todo Probably leaking memory here! */
            }
          }
        }
      }

      //
      // Try load any of the default boot files.
      //
      if (EFI_ERROR(Status)) {
        UINT32 IdxFile;
        for (IdxFile = 0; IdxFile < sizeof(maBdsLibBootFiles) / sizeof(maBdsLibBootFiles[0]); IdxFile++) {
          FilePath = FileDevicePath(Handle, maBdsLibBootFiles[IdxFile]);
          if (FilePath) {
            VBoxLog(("BdsLibBootViaBootOption: Trying to load '%s'...\n", DevicePathToStr(FilePath)));
            REPORT_STATUS_CODE(EFI_PROGRESS_CODE, PcdGet32(PcdProgressCodeOsLoaderLoad));
            Status = gBS->LoadImage(TRUE, gImageHandle, FilePath, NULL, 0, &ImageHandle );
            if (!EFI_ERROR(Status)) {
              VBoxLog(("BdsLibBootViaBootOption: Successfully loaded boot file: %s\n", DevicePathToStr(FilePath)));
              break;
            }
            VBoxLog(("BdsLibBootViaBootOption: Failed loading '%s': 0x%x(%r)\n", DevicePathToStr(FilePath), Status, Status));
            FilePath = NULL; /** @todo Probably leaking memory here! */
          }
        }
      }
#else  /* !VBOX */
      //
      // Load the default boot file \EFI\BOOT\boot{machinename}.EFI from removable Media
      //  machinename is ia32, ia64, x64, ...
      //
      FilePath = FileDevicePath (Handle, EFI_REMOVABLE_MEDIA_FILE_NAME);
      if (FilePath != NULL) {
        REPORT_STATUS_CODE (EFI_PROGRESS_CODE, PcdGet32 (PcdProgressCodeOsLoaderLoad));
        Status = gBS->LoadImage (
                        TRUE,
                        gImageHandle,
                        FilePath,
                        NULL,
                        0,
                        &ImageHandle
                        );
       if (EFI_ERROR (Status)) {
          //
          // The DevicePath failed, and it's not a valid
          // removable media device.
          //
          goto Done;
        }
      }
#endif /* !VBOX */
    }

    if (EFI_ERROR (Status)) {
      //
      // It there is any error from the Boot attempt exit now.
      //
      goto Done;
    }
  }
  //
  // Provide the image with it's load options
  //
  if (ImageHandle == NULL) {
    goto Done;
  }
  Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **) &ImageInfo);
  ASSERT_EFI_ERROR (Status);

  if (Option->LoadOptionsSize != 0) {
    ImageInfo->LoadOptionsSize  = Option->LoadOptionsSize;
    ImageInfo->LoadOptions      = Option->LoadOptions;
  }
  //
  // Before calling the image, enable the Watchdog Timer for
  // the 5 Minute period
  //
  gBS->SetWatchdogTimer (5 * 60, 0x0000, 0x00, NULL);

  //
  // Write boot to OS performance data for UEFI boot
  //
  PERF_CODE (
    WriteBootToOsPerformanceData ();
  );

  //
  // Report status code for OS Loader StartImage.
  //
  REPORT_STATUS_CODE (EFI_PROGRESS_CODE, PcdGet32 (PcdProgressCodeOsLoaderStart));

  Status = gBS->StartImage (ImageHandle, ExitDataSize, ExitData);
  DEBUG ((DEBUG_INFO | DEBUG_LOAD, "Image Return Status = %r\n", Status));

  //
  // Clear the Watchdog Timer after the image returns
  //
  gBS->SetWatchdogTimer (0x0000, 0x0000, 0x0000, NULL);

Done:
  //
  // Set Logo status invalid after trying one boot option
  //
  BootLogo = NULL;
  StatusLogo = gBS->LocateProtocol (&gEfiBootLogoProtocolGuid, NULL, (VOID **) &BootLogo);
  if (!EFI_ERROR (StatusLogo) && (BootLogo != NULL)) {
    BootLogo->SetBootLogo (BootLogo, NULL, 0, 0, 0, 0);
  }

  //
  // Clear Boot Current
  //
  gRT->SetVariable (
        L"BootCurrent",
        &gEfiGlobalVariableGuid,
        EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
        0,
        &Option->BootCurrent
        );

  return Status;
}


/**
  Expand a device path that starts with a hard drive media device path node to be a
  full device path that includes the full hardware path to the device. We need
  to do this so it can be booted. As an optimization the front match (the part point
  to the partition node. E.g. ACPI() /PCI()/ATA()/Partition() ) is saved in a variable
  so a connect all is not required on every boot. All successful history device path
  which point to partition node (the front part) will be saved.

  @param  HardDriveDevicePath    EFI Device Path to boot, if it starts with a hard
                                 drive media device path.
  @return A Pointer to the full device path or NULL if a valid Hard Drive devic path
          cannot be found.

**/
EFI_DEVICE_PATH_PROTOCOL *
EFIAPI
BdsExpandPartitionPartialDevicePathToFull (
  IN  HARDDRIVE_DEVICE_PATH      *HardDriveDevicePath
  )
{
  EFI_STATUS                Status;
  UINTN                     BlockIoHandleCount;
  EFI_HANDLE                *BlockIoBuffer;
  EFI_DEVICE_PATH_PROTOCOL  *FullDevicePath;
  EFI_DEVICE_PATH_PROTOCOL  *BlockIoDevicePath;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  UINTN                     Index;
  UINTN                     InstanceNum;
  EFI_DEVICE_PATH_PROTOCOL  *CachedDevicePath;
  EFI_DEVICE_PATH_PROTOCOL  *TempNewDevicePath;
  UINTN                     CachedDevicePathSize;
  BOOLEAN                   DeviceExist;
  BOOLEAN                   NeedAdjust;
  EFI_DEVICE_PATH_PROTOCOL  *Instance;
  UINTN                     Size;

  FullDevicePath = NULL;
  //
  // Check if there is prestore HD_BOOT_DEVICE_PATH_VARIABLE_NAME variable.
  // If exist, search the front path which point to partition node in the variable instants.
  // If fail to find or HD_BOOT_DEVICE_PATH_VARIABLE_NAME not exist, reconnect all and search in all system
  //
  CachedDevicePath = BdsLibGetVariableAndSize (
                      HD_BOOT_DEVICE_PATH_VARIABLE_NAME,
                      &gHdBootDevicePathVariablGuid,
                      &CachedDevicePathSize
                      );

  if (CachedDevicePath != NULL) {
    TempNewDevicePath = CachedDevicePath;
    DeviceExist = FALSE;
    NeedAdjust = FALSE;
    do {
      //
      // Check every instance of the variable
      // First, check whether the instance contain the partition node, which is needed for distinguishing  multi
      // partial partition boot option. Second, check whether the instance could be connected.
      //
      Instance  = GetNextDevicePathInstance (&TempNewDevicePath, &Size);
      if (MatchPartitionDevicePathNode (Instance, HardDriveDevicePath)) {
        //
        // Connect the device path instance, the device path point to hard drive media device path node
        // e.g. ACPI() /PCI()/ATA()/Partition()
        //
        Status = BdsLibConnectDevicePath (Instance);
        if (!EFI_ERROR (Status)) {
          DeviceExist = TRUE;
          break;
        }
      }
      //
      // Come here means the first instance is not matched
      //
      NeedAdjust = TRUE;
      FreePool(Instance);
    } while (TempNewDevicePath != NULL);

    if (DeviceExist) {
      //
      // Find the matched device path.
      // Append the file path information from the boot option and return the fully expanded device path.
      //
      DevicePath     = NextDevicePathNode ((EFI_DEVICE_PATH_PROTOCOL *) HardDriveDevicePath);
      FullDevicePath = AppendDevicePath (Instance, DevicePath);

      //
      // Adjust the HD_BOOT_DEVICE_PATH_VARIABLE_NAME instances sequence if the matched one is not first one.
      //
      if (NeedAdjust) {
        //
        // First delete the matched instance.
        //
        TempNewDevicePath = CachedDevicePath;
        CachedDevicePath  = BdsLibDelPartMatchInstance (CachedDevicePath, Instance );
        FreePool (TempNewDevicePath);

        //
        // Second, append the remaining path after the matched instance
        //
        TempNewDevicePath = CachedDevicePath;
        CachedDevicePath = AppendDevicePathInstance (Instance, CachedDevicePath );
        FreePool (TempNewDevicePath);
        //
        // Save the matching Device Path so we don't need to do a connect all next time
        //
        Status = gRT->SetVariable (
                        HD_BOOT_DEVICE_PATH_VARIABLE_NAME,
                        &gHdBootDevicePathVariablGuid,
                        EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_NON_VOLATILE,
                        GetDevicePathSize (CachedDevicePath),
                        CachedDevicePath
                        );
      }

      FreePool (Instance);
      FreePool (CachedDevicePath);
      return FullDevicePath;
    }
  }

  //
  // If we get here we fail to find or HD_BOOT_DEVICE_PATH_VARIABLE_NAME not exist, and now we need
  // to search all devices in the system for a matched partition
  //
  BdsLibConnectAllDriversToAllControllers ();
  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiBlockIoProtocolGuid, NULL, &BlockIoHandleCount, &BlockIoBuffer);
  if (EFI_ERROR (Status) || BlockIoHandleCount == 0 || BlockIoBuffer == NULL) {
    //
    // If there was an error or there are no device handles that support
    // the BLOCK_IO Protocol, then return.
    //
    return NULL;
  }
  //
  // Loop through all the device handles that support the BLOCK_IO Protocol
  //
  for (Index = 0; Index < BlockIoHandleCount; Index++) {

    Status = gBS->HandleProtocol (BlockIoBuffer[Index], &gEfiDevicePathProtocolGuid, (VOID *) &BlockIoDevicePath);
    if (EFI_ERROR (Status) || BlockIoDevicePath == NULL) {
      continue;
    }

    if (MatchPartitionDevicePathNode (BlockIoDevicePath, HardDriveDevicePath)) {
      //
      // Find the matched partition device path
      //
      DevicePath    = NextDevicePathNode ((EFI_DEVICE_PATH_PROTOCOL *) HardDriveDevicePath);
      FullDevicePath = AppendDevicePath (BlockIoDevicePath, DevicePath);

      //
      // Save the matched partition device path in HD_BOOT_DEVICE_PATH_VARIABLE_NAME variable
      //
      if (CachedDevicePath != NULL) {
        //
        // Save the matched partition device path as first instance of HD_BOOT_DEVICE_PATH_VARIABLE_NAME variable
        //
        if (BdsLibMatchDevicePaths (CachedDevicePath, BlockIoDevicePath)) {
          TempNewDevicePath = CachedDevicePath;
          CachedDevicePath = BdsLibDelPartMatchInstance (CachedDevicePath, BlockIoDevicePath);
          FreePool(TempNewDevicePath);

          TempNewDevicePath = CachedDevicePath;
          CachedDevicePath = AppendDevicePathInstance (BlockIoDevicePath, CachedDevicePath);
          if (TempNewDevicePath != NULL) {
            FreePool(TempNewDevicePath);
          }
        } else {
          TempNewDevicePath = CachedDevicePath;
          CachedDevicePath = AppendDevicePathInstance (BlockIoDevicePath, CachedDevicePath);
          FreePool(TempNewDevicePath);
        }
        //
        // Here limit the device path instance number to 12, which is max number for a system support 3 IDE controller
        // If the user try to boot many OS in different HDs or partitions, in theory,
        // the HD_BOOT_DEVICE_PATH_VARIABLE_NAME variable maybe become larger and larger.
        //
        InstanceNum = 0;
        ASSERT (CachedDevicePath != NULL);
        TempNewDevicePath = CachedDevicePath;
        while (!IsDevicePathEnd (TempNewDevicePath)) {
          TempNewDevicePath = NextDevicePathNode (TempNewDevicePath);
          //
          // Parse one instance
          //
          while (!IsDevicePathEndType (TempNewDevicePath)) {
            TempNewDevicePath = NextDevicePathNode (TempNewDevicePath);
          }
          InstanceNum++;
          //
          // If the CachedDevicePath variable contain too much instance, only remain 12 instances.
          //
          if (InstanceNum >= 12) {
            SetDevicePathEndNode (TempNewDevicePath);
            break;
          }
        }
      } else {
        CachedDevicePath = DuplicateDevicePath (BlockIoDevicePath);
      }

      //
      // Save the matching Device Path so we don't need to do a connect all next time
      //
      Status = gRT->SetVariable (
                      HD_BOOT_DEVICE_PATH_VARIABLE_NAME,
                      &gHdBootDevicePathVariablGuid,
                      EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_NON_VOLATILE,
                      GetDevicePathSize (CachedDevicePath),
                      CachedDevicePath
                      );

      break;
    }
  }

  if (CachedDevicePath != NULL) {
    FreePool (CachedDevicePath);
  }
  if (BlockIoBuffer != NULL) {
    FreePool (BlockIoBuffer);
  }
  return FullDevicePath;
}

/**
  Check whether there is a instance in BlockIoDevicePath, which contain multi device path
  instances, has the same partition node with HardDriveDevicePath device path

  @param  BlockIoDevicePath      Multi device path instances which need to check
  @param  HardDriveDevicePath    A device path which starts with a hard drive media
                                 device path.

  @retval TRUE                   There is a matched device path instance.
  @retval FALSE                  There is no matched device path instance.

**/
BOOLEAN
EFIAPI
MatchPartitionDevicePathNode (
  IN  EFI_DEVICE_PATH_PROTOCOL   *BlockIoDevicePath,
  IN  HARDDRIVE_DEVICE_PATH      *HardDriveDevicePath
  )
{
  HARDDRIVE_DEVICE_PATH     *TmpHdPath;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  BOOLEAN                   Match;
  EFI_DEVICE_PATH_PROTOCOL  *BlockIoHdDevicePathNode;

  if ((BlockIoDevicePath == NULL) || (HardDriveDevicePath == NULL)) {
    return FALSE;
  }

  //
  // Make PreviousDevicePath == the device path node before the end node
  //
  DevicePath              = BlockIoDevicePath;
  BlockIoHdDevicePathNode = NULL;

  //
  // find the partition device path node
  //
  while (!IsDevicePathEnd (DevicePath)) {
    if ((DevicePathType (DevicePath) == MEDIA_DEVICE_PATH) &&
        (DevicePathSubType (DevicePath) == MEDIA_HARDDRIVE_DP)
        ) {
      BlockIoHdDevicePathNode = DevicePath;
      break;
    }

    DevicePath = NextDevicePathNode (DevicePath);
  }

  if (BlockIoHdDevicePathNode == NULL) {
    return FALSE;
  }
  //
  // See if the harddrive device path in blockio matches the orig Hard Drive Node
  //
  TmpHdPath = (HARDDRIVE_DEVICE_PATH *) BlockIoHdDevicePathNode;
  Match = FALSE;

  //
  // Check for the match
  //
  if ((TmpHdPath->MBRType == HardDriveDevicePath->MBRType) &&
      (TmpHdPath->SignatureType == HardDriveDevicePath->SignatureType)) {
    switch (TmpHdPath->SignatureType) {
    case SIGNATURE_TYPE_GUID:
      Match = CompareGuid ((EFI_GUID *)TmpHdPath->Signature, (EFI_GUID *)HardDriveDevicePath->Signature);
      break;
    case SIGNATURE_TYPE_MBR:
      Match = (BOOLEAN)(*((UINT32 *)(&(TmpHdPath->Signature[0]))) == ReadUnaligned32((UINT32 *)(&(HardDriveDevicePath->Signature[0]))));
      break;
    default:
      Match = FALSE;
      break;
    }
  }

  return Match;
}

/**
  Delete the boot option associated with the handle passed in.

  @param  Handle                 The handle which present the device path to create
                                 boot option

  @retval EFI_SUCCESS            Delete the boot option success
  @retval EFI_NOT_FOUND          If the Device Path is not found in the system
  @retval EFI_OUT_OF_RESOURCES   Lack of memory resource
  @retval Other                  Error return value from SetVariable()

**/
EFI_STATUS
BdsLibDeleteOptionFromHandle (
  IN  EFI_HANDLE                 Handle
  )
{
  UINT16                    *BootOrder;
  UINT8                     *BootOptionVar;
  UINTN                     BootOrderSize;
  UINTN                     BootOptionSize;
  EFI_STATUS                Status;
  UINTN                     Index;
  UINT16                    BootOption[BOOT_OPTION_MAX_CHAR];
  UINTN                     DevicePathSize;
  UINTN                     OptionDevicePathSize;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  EFI_DEVICE_PATH_PROTOCOL  *OptionDevicePath;
  UINT8                     *TempPtr;

  Status        = EFI_SUCCESS;
  BootOrder     = NULL;
  BootOrderSize = 0;

  //
  // Check "BootOrder" variable, if no, means there is no any boot order.
  //
  BootOrder = BdsLibGetVariableAndSize (
                L"BootOrder",
                &gEfiGlobalVariableGuid,
                &BootOrderSize
                );
  if (BootOrder == NULL) {
    return EFI_NOT_FOUND;
  }

  //
  // Convert device handle to device path protocol instance
  //
  DevicePath = DevicePathFromHandle (Handle);
  if (DevicePath == NULL) {
    return EFI_NOT_FOUND;
  }
  DevicePathSize = GetDevicePathSize (DevicePath);

  //
  // Loop all boot order variable and find the matching device path
  //
  Index = 0;
  while (Index < BootOrderSize / sizeof (UINT16)) {
    UnicodeSPrint (BootOption, sizeof (BootOption), L"Boot%04x", BootOrder[Index]);
    BootOptionVar = BdsLibGetVariableAndSize (
                      BootOption,
                      &gEfiGlobalVariableGuid,
                      &BootOptionSize
                      );

    if (BootOptionVar == NULL) {
      FreePool (BootOrder);
      return EFI_OUT_OF_RESOURCES;
    }

    if (!ValidateOption(BootOptionVar, BootOptionSize)) {
      BdsDeleteBootOption (BootOrder[Index], BootOrder, &BootOrderSize);
      FreePool (BootOptionVar);
      Index++;
      continue;
    }

    TempPtr = BootOptionVar;
    TempPtr += sizeof (UINT32) + sizeof (UINT16);
    TempPtr += StrSize ((CHAR16 *) TempPtr);
    OptionDevicePath = (EFI_DEVICE_PATH_PROTOCOL *) TempPtr;
    OptionDevicePathSize = GetDevicePathSize (OptionDevicePath);

    //
    // Check whether the device path match
    //
    if ((OptionDevicePathSize == DevicePathSize) &&
        (CompareMem (DevicePath, OptionDevicePath, DevicePathSize) == 0)) {
      BdsDeleteBootOption (BootOrder[Index], BootOrder, &BootOrderSize);
      FreePool (BootOptionVar);
      break;
    }

    FreePool (BootOptionVar);
    Index++;
  }

  //
  // Adjust number of boot option for "BootOrder" variable.
  //
  Status = gRT->SetVariable (
                  L"BootOrder",
                  &gEfiGlobalVariableGuid,
                  EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_NON_VOLATILE,
                  BootOrderSize,
                  BootOrder
                  );

  FreePool (BootOrder);

  return Status;
}


/**
  Delete all invalid EFI boot options.

  @retval EFI_SUCCESS            Delete all invalid boot option success
  @retval EFI_NOT_FOUND          Variable "BootOrder" is not found
  @retval EFI_OUT_OF_RESOURCES   Lack of memory resource
  @retval Other                  Error return value from SetVariable()

**/
EFI_STATUS
BdsDeleteAllInvalidEfiBootOption (
  VOID
  )
{
  UINT16                    *BootOrder;
  UINT8                     *BootOptionVar;
  UINTN                     BootOrderSize;
  UINTN                     BootOptionSize;
  EFI_STATUS                Status;
  UINTN                     Index;
  UINTN                     Index2;
  UINT16                    BootOption[BOOT_OPTION_MAX_CHAR];
  EFI_DEVICE_PATH_PROTOCOL  *OptionDevicePath;
  UINT8                     *TempPtr;
  CHAR16                    *Description;
  BOOLEAN                   Corrupted;

  Status           = EFI_SUCCESS;
  BootOrder        = NULL;
  Description      = NULL;
  OptionDevicePath = NULL;
  BootOrderSize    = 0;
  Corrupted        = FALSE;

  //
  // Check "BootOrder" variable firstly, this variable hold the number of boot options
  //
  VBoxLogFlowFuncEnter();
  BootOrder = BdsLibGetVariableAndSize (
                L"BootOrder",
                &gEfiGlobalVariableGuid,
                &BootOrderSize
                );
  if (NULL == BootOrder) {
    VBoxLogFlowFuncLeaveRC(EFI_NOT_FOUND);
    return EFI_NOT_FOUND;
  }

  Index = 0;
  while (Index < BootOrderSize / sizeof (UINT16)) {
    UnicodeSPrint (BootOption, sizeof (BootOption), L"Boot%04x", BootOrder[Index]);
    VBoxLogFlowFuncMarkVar(BootOption, "%s");
    BootOptionVar = BdsLibGetVariableAndSize (
                      BootOption,
                      &gEfiGlobalVariableGuid,
                      &BootOptionSize
                      );
    if (NULL == BootOptionVar) {
      FreePool (BootOrder);
      VBoxLogFlowFuncLeaveRC(EFI_OUT_OF_RESOURCES);
      return EFI_OUT_OF_RESOURCES;
    }

    VBoxLogFlowFuncMarkVar(BootOption, "%s");
    if (!ValidateOption(BootOptionVar, BootOptionSize)) {
      Corrupted = TRUE;
    } else {
      TempPtr = BootOptionVar;
      TempPtr += sizeof (UINT32) + sizeof (UINT16);
      Description = (CHAR16 *) TempPtr;
      TempPtr += StrSize ((CHAR16 *) TempPtr);
      OptionDevicePath = (EFI_DEVICE_PATH_PROTOCOL *) TempPtr;

      //
      // Skip legacy boot option (BBS boot device)
      //
      if ((DevicePathType (OptionDevicePath) == BBS_DEVICE_PATH) &&
          (DevicePathSubType (OptionDevicePath) == BBS_BBS_DP)) {
        FreePool (BootOptionVar);
        Index++;
        continue;
      }
    }

    if (Corrupted || !BdsLibIsValidEFIBootOptDevicePathExt (OptionDevicePath, FALSE, Description)) {
      //
      // Delete this invalid boot option "Boot####"
      //
      VBoxLogFlowFuncMarkVar(BootOption, "DELETE %s");
      Status = gRT->SetVariable (
                      BootOption,
                      &gEfiGlobalVariableGuid,
                      EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_NON_VOLATILE,
                      0,
                      NULL
                      );
      //
      // Mark this boot option in boot order as deleted
      //
      VBoxLogFlowFuncMarkVar(BootOption, "%s");
      VBoxLogFlowFuncMarkRC(Status);
      BootOrder[Index] = 0xffff;
      Corrupted        = FALSE;
    }

    FreePool (BootOptionVar);
    Index++;
  }

  //
  // Adjust boot order array
  //
  Index2 = 0;
  for (Index = 0; Index < BootOrderSize / sizeof (UINT16); Index++) {
    if (BootOrder[Index] != 0xffff) {
      BootOrder[Index2] = BootOrder[Index];
      Index2 ++;
    }
  }
  Status = gRT->SetVariable (
                  L"BootOrder",
                  &gEfiGlobalVariableGuid,
                  EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_NON_VOLATILE,
                  Index2 * sizeof (UINT16),
                  BootOrder
                  );
  VBoxLogFlowFuncMarkRC(Status);
  FreePool (BootOrder);

  VBoxLogFlowFuncLeaveRC(Status);
  return Status;
}


/**
  For EFI boot option, BDS separate them as six types:
  1. Network - The boot option points to the SimpleNetworkProtocol device.
               Bds will try to automatically create this type boot option when enumerate.
  2. Shell   - The boot option points to internal flash shell.
               Bds will try to automatically create this type boot option when enumerate.
  3. Removable BlockIo      - The boot option only points to the removable media
                              device, like USB flash disk, DVD, Floppy etc.
                              These device should contain a *removable* blockIo
                              protocol in their device handle.
                              Bds will try to automatically create this type boot option
                              when enumerate.
  4. Fixed BlockIo          - The boot option only points to a Fixed blockIo device,
                              like HardDisk.
                              These device should contain a *fixed* blockIo
                              protocol in their device handle.
                              BDS will skip fixed blockIo devices, and NOT
                              automatically create boot option for them. But BDS
                              will help to delete those fixed blockIo boot option,
                              whose description rule conflict with other auto-created
                              boot options.
  5. Non-BlockIo Simplefile - The boot option points to a device whose handle
                              has SimpleFileSystem Protocol, but has no blockio
                              protocol. These devices do not offer blockIo
                              protocol, but BDS still can get the
                              \EFI\BOOT\boot{machinename}.EFI by SimpleFileSystem
                              Protocol.
  6. File    - The boot option points to a file. These boot options are usually
               created by user manually or OS loader. BDS will not delete or modify
               these boot options.

  This function will enumerate all possible boot device in the system, and
  automatically create boot options for Network, Shell, Removable BlockIo,
  and Non-BlockIo Simplefile devices.
  It will only execute once of every boot.

  @param  BdsBootOptionList      The header of the link list which indexed all
                                 current boot options

  @retval EFI_SUCCESS            Finished all the boot device enumerate and create
                                 the boot option base on that boot device

  @retval EFI_OUT_OF_RESOURCES   Failed to enumerate the boot device and create the boot option list
**/
EFI_STATUS
EFIAPI
BdsLibEnumerateAllBootOption (
  IN OUT LIST_ENTRY          *BdsBootOptionList
  )
{
  EFI_STATUS                    Status;
  UINT16                        FloppyNumber;
  UINT16                        HarddriveNumber;
  UINT16                        CdromNumber;
  UINT16                        UsbNumber;
  UINT16                        MiscNumber;
  UINT16                        ScsiNumber;
  UINT16                        NonBlockNumber;
  UINTN                         NumberBlockIoHandles;
  EFI_HANDLE                    *BlockIoHandles;
  EFI_BLOCK_IO_PROTOCOL         *BlkIo;
  BOOLEAN                       Removable[2];
  UINTN                         RemovableIndex;
  UINTN                         Index;
  UINTN                         NumOfLoadFileHandles;
  EFI_HANDLE                    *LoadFileHandles;
  UINTN                         FvHandleCount;
  EFI_HANDLE                    *FvHandleBuffer;
  EFI_FV_FILETYPE               Type;
  UINTN                         Size;
  EFI_FV_FILE_ATTRIBUTES        Attributes;
  UINT32                        AuthenticationStatus;
  EFI_FIRMWARE_VOLUME2_PROTOCOL *Fv;
  EFI_DEVICE_PATH_PROTOCOL      *DevicePath;
  UINTN                         DevicePathType;
  CHAR16                        Buffer[40];
  EFI_HANDLE                    *FileSystemHandles;
  UINTN                         NumberFileSystemHandles;
  BOOLEAN                       NeedDelete;
  EFI_IMAGE_DOS_HEADER          DosHeader;
  CHAR8                         *PlatLang;
  CHAR8                         *LastLang;
  EFI_IMAGE_OPTIONAL_HEADER_UNION       HdrData;
  EFI_IMAGE_OPTIONAL_HEADER_PTR_UNION   Hdr = {0}; /* VBox: VS2010 thinks it may be used uninitialized. */

  FloppyNumber    = 0;
  HarddriveNumber = 0;
  CdromNumber     = 0;
  UsbNumber       = 0;
  MiscNumber      = 0;
  ScsiNumber      = 0;
  PlatLang        = NULL;
  LastLang        = NULL;
  ZeroMem (Buffer, sizeof (Buffer));
  VBoxLogFlowFuncEnter();

  //
  // If the boot device enumerate happened, just get the boot
  // device from the boot order variable
  //
  if (mEnumBootDevice) {
    LastLang = GetVariable (LAST_ENUM_LANGUAGE_VARIABLE_NAME, &gLastEnumLangGuid);
    PlatLang = GetEfiGlobalVariable (L"PlatformLang");
    ASSERT (PlatLang != NULL);
    if ((LastLang != NULL) && (AsciiStrCmp (LastLang, PlatLang) == 0)) {
      Status = BdsLibBuildOptionFromVar (BdsBootOptionList, L"BootOrder");
      FreePool (LastLang);
      FreePool (PlatLang);
      VBoxLogFlowFuncLeaveRC(Status);
      return Status;
    } else {
      Status = gRT->SetVariable (
        LAST_ENUM_LANGUAGE_VARIABLE_NAME,
        &gLastEnumLangGuid,
        EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_NON_VOLATILE,
        AsciiStrSize (PlatLang),
        PlatLang
        );
      ASSERT_EFI_ERROR (Status);

      if (LastLang != NULL) {
        FreePool (LastLang);
      }
      FreePool (PlatLang);
    }
  }

  //
  // Notes: this dirty code is to get the legacy boot option from the
  // BBS table and create to variable as the EFI boot option, it should
  // be removed after the CSM can provide legacy boot option directly
  //
  REFRESH_LEGACY_BOOT_OPTIONS;

  //
  // Delete invalid boot option
  //
  BdsDeleteAllInvalidEfiBootOption ();

  //
  // Parse removable media followed by fixed media.
  // The Removable[] array is used by the for-loop below to create removable media boot options
  // at first, and then to create fixed media boot options.
  //
  Removable[0]  = FALSE;
  Removable[1]  = TRUE;

  gBS->LocateHandleBuffer (
        ByProtocol,
        &gEfiBlockIoProtocolGuid,
        NULL,
        &NumberBlockIoHandles,
        &BlockIoHandles
        );

  for (RemovableIndex = 0; RemovableIndex < 2; RemovableIndex++) {
    for (Index = 0; Index < NumberBlockIoHandles; Index++) {
      Status = gBS->HandleProtocol (
                      BlockIoHandles[Index],
                      &gEfiBlockIoProtocolGuid,
                      (VOID **) &BlkIo
                      );
      //
      // skip the fixed block io then the removable block io
      //
      if (EFI_ERROR (Status) || (BlkIo->Media->RemovableMedia == Removable[RemovableIndex])) {
        continue;
      }
      DevicePath  = DevicePathFromHandle (BlockIoHandles[Index]);
      DevicePathType = BdsGetBootTypeFromDevicePath (DevicePath);

      switch (DevicePathType) {
      case BDS_EFI_ACPI_FLOPPY_BOOT:
        if (FloppyNumber != 0) {
          UnicodeSPrint (Buffer, sizeof (Buffer), L"%s %d", BdsLibGetStringById (STRING_TOKEN (STR_DESCRIPTION_FLOPPY)), FloppyNumber);
        } else {
          UnicodeSPrint (Buffer, sizeof (Buffer), L"%s", BdsLibGetStringById (STRING_TOKEN (STR_DESCRIPTION_FLOPPY)));
        }
        BdsLibBuildOptionFromHandle (BlockIoHandles[Index], BdsBootOptionList, Buffer);
        FloppyNumber++;
        break;

      //
      // Assume a removable SATA device should be the DVD/CD device, a fixed SATA device should be the Hard Drive device.
      //
      case BDS_EFI_MESSAGE_ATAPI_BOOT:
      case BDS_EFI_MESSAGE_SATA_BOOT:
        if (BlkIo->Media->RemovableMedia) {
          if (CdromNumber != 0) {
            UnicodeSPrint (Buffer, sizeof (Buffer), L"%s %d", BdsLibGetStringById (STRING_TOKEN (STR_DESCRIPTION_CD_DVD)), CdromNumber);
          } else {
            UnicodeSPrint (Buffer, sizeof (Buffer), L"%s", BdsLibGetStringById (STRING_TOKEN (STR_DESCRIPTION_CD_DVD)));
          }
          CdromNumber++;
        } else {
          if (HarddriveNumber != 0) {
            UnicodeSPrint (Buffer, sizeof (Buffer), L"%s %d", BdsLibGetStringById (STRING_TOKEN (STR_DESCRIPTION_HARDDRIVE)), HarddriveNumber);
          } else {
            UnicodeSPrint (Buffer, sizeof (Buffer), L"%s", BdsLibGetStringById (STRING_TOKEN (STR_DESCRIPTION_HARDDRIVE)));
          }
          HarddriveNumber++;
        }
        DEBUG ((DEBUG_INFO | DEBUG_LOAD, "Buffer: %S\n", Buffer));
        BdsLibBuildOptionFromHandle (BlockIoHandles[Index], BdsBootOptionList, Buffer);
        break;

      case BDS_EFI_MESSAGE_USB_DEVICE_BOOT:
        if (UsbNumber != 0) {
          UnicodeSPrint (Buffer, sizeof (Buffer), L"%s %d", BdsLibGetStringById (STRING_TOKEN (STR_DESCRIPTION_USB)), UsbNumber);
        } else {
          UnicodeSPrint (Buffer, sizeof (Buffer), L"%s", BdsLibGetStringById (STRING_TOKEN (STR_DESCRIPTION_USB)));
        }
        BdsLibBuildOptionFromHandle (BlockIoHandles[Index], BdsBootOptionList, Buffer);
        UsbNumber++;
        break;

      case BDS_EFI_MESSAGE_SCSI_BOOT:
        if (ScsiNumber != 0) {
          UnicodeSPrint (Buffer, sizeof (Buffer), L"%s %d", BdsLibGetStringById (STRING_TOKEN (STR_DESCRIPTION_SCSI)), ScsiNumber);
        } else {
          UnicodeSPrint (Buffer, sizeof (Buffer), L"%s", BdsLibGetStringById (STRING_TOKEN (STR_DESCRIPTION_SCSI)));
        }
        BdsLibBuildOptionFromHandle (BlockIoHandles[Index], BdsBootOptionList, Buffer);
        ScsiNumber++;
        break;

      case BDS_EFI_MESSAGE_MISC_BOOT:
        if (MiscNumber != 0) {
          UnicodeSPrint (Buffer, sizeof (Buffer), L"%s %d", BdsLibGetStringById (STRING_TOKEN (STR_DESCRIPTION_MISC)), MiscNumber);
        } else {
          UnicodeSPrint (Buffer, sizeof (Buffer), L"%s", BdsLibGetStringById (STRING_TOKEN (STR_DESCRIPTION_MISC)));
        }
        BdsLibBuildOptionFromHandle (BlockIoHandles[Index], BdsBootOptionList, Buffer);
        MiscNumber++;
        break;

      default:
        break;
      }
    }
  }

  if (NumberBlockIoHandles != 0) {
    FreePool (BlockIoHandles);
  }

  //
  // If there is simple file protocol which does not consume block Io protocol, create a boot option for it here.
  //
  NonBlockNumber = 0;
  gBS->LocateHandleBuffer (
        ByProtocol,
        &gEfiSimpleFileSystemProtocolGuid,
        NULL,
        &NumberFileSystemHandles,
        &FileSystemHandles
        );
  for (Index = 0; Index < NumberFileSystemHandles; Index++) {
    Status = gBS->HandleProtocol (
                    FileSystemHandles[Index],
                    &gEfiBlockIoProtocolGuid,
                    (VOID **) &BlkIo
                    );
    if (!EFI_ERROR (Status)) {
      //
      //  Skip if the file system handle supports a BlkIo protocol,
      //
      continue;
    }

    //
    // Do the removable Media thing. \EFI\BOOT\boot{machinename}.EFI
    //  machinename is ia32, ia64, x64, ...
    //
#ifdef VBOX /* We try serveral variants because of apple. */
    {
      UINT32 IdxFile;
      NeedDelete = TRUE;
      for (IdxFile = 0; IdxFile < sizeof(maBdsLibBootFiles) / sizeof(maBdsLibBootFiles[0]); IdxFile++) {
        Hdr.Union  = &HdrData;
        Status = BdsLibGetImageHeader(FileSystemHandles[Index], (CHAR16 *)maBdsLibBootFiles[IdxFile], &DosHeader, Hdr);
        if (!EFI_ERROR(Status)) {
          break;
        }
      }
    }
#else  /* !VBOX */
    Hdr.Union  = &HdrData;
    NeedDelete = TRUE;
    Status     = BdsLibGetImageHeader (
                   FileSystemHandles[Index],
                   EFI_REMOVABLE_MEDIA_FILE_NAME,
                   &DosHeader,
                   Hdr
                   );
#endif /* !VBOX */
    if (!EFI_ERROR (Status) &&
        EFI_IMAGE_MACHINE_TYPE_SUPPORTED (Hdr.Pe32->FileHeader.Machine) &&
        Hdr.Pe32->OptionalHeader.Subsystem == EFI_IMAGE_SUBSYSTEM_EFI_APPLICATION) {
      NeedDelete = FALSE;
    }

    if (NeedDelete) {
      //
      // No such file or the file is not a EFI application, delete this boot option
      //
      BdsLibDeleteOptionFromHandle (FileSystemHandles[Index]);
    } else {
      if (NonBlockNumber != 0) {
        UnicodeSPrint (Buffer, sizeof (Buffer), L"%s %d", BdsLibGetStringById (STRING_TOKEN (STR_DESCRIPTION_NON_BLOCK)), NonBlockNumber);
      } else {
        UnicodeSPrint (Buffer, sizeof (Buffer), L"%s", BdsLibGetStringById (STRING_TOKEN (STR_DESCRIPTION_NON_BLOCK)));
      }
      BdsLibBuildOptionFromHandle (FileSystemHandles[Index], BdsBootOptionList, Buffer);
      NonBlockNumber++;
    }
  }

  if (NumberFileSystemHandles != 0) {
    FreePool (FileSystemHandles);
  }

  //
  // Parse Network Boot Device
  //
  NumOfLoadFileHandles = 0;
  //
  // Search Load File protocol for PXE boot option.
  //
  gBS->LocateHandleBuffer (
        ByProtocol,
        &gEfiLoadFileProtocolGuid,
        NULL,
        &NumOfLoadFileHandles,
        &LoadFileHandles
        );

  for (Index = 0; Index < NumOfLoadFileHandles; Index++) {
    if (Index != 0) {
      UnicodeSPrint (Buffer, sizeof (Buffer), L"%s %d", BdsLibGetStringById (STRING_TOKEN (STR_DESCRIPTION_NETWORK)), Index);
    } else {
      UnicodeSPrint (Buffer, sizeof (Buffer), L"%s", BdsLibGetStringById (STRING_TOKEN (STR_DESCRIPTION_NETWORK)));
    }
    BdsLibBuildOptionFromHandle (LoadFileHandles[Index], BdsBootOptionList, Buffer);
  }

  if (NumOfLoadFileHandles != 0) {
    FreePool (LoadFileHandles);
  }

  //
  // Check if we have on flash shell
  //
  gBS->LocateHandleBuffer (
        ByProtocol,
        &gEfiFirmwareVolume2ProtocolGuid,
        NULL,
        &FvHandleCount,
        &FvHandleBuffer
        );
  for (Index = 0; Index < FvHandleCount; Index++) {
    gBS->HandleProtocol (
          FvHandleBuffer[Index],
          &gEfiFirmwareVolume2ProtocolGuid,
          (VOID **) &Fv
          );

    Status = Fv->ReadFile (
                  Fv,
                  PcdGetPtr(PcdShellFile),
                  NULL,
                  &Size,
                  &Type,
                  &Attributes,
                  &AuthenticationStatus
                  );
    if (EFI_ERROR (Status)) {
      //
      // Skip if no shell file in the FV
      //
      continue;
    }
    //
    // Build the shell boot option
    //
    BdsLibBuildOptionFromShell (FvHandleBuffer[Index], BdsBootOptionList);
  }

  if (FvHandleCount != 0) {
    FreePool (FvHandleBuffer);
  }
  //
  // Make sure every boot only have one time
  // boot device enumerate
  //
  Status = BdsLibBuildOptionFromVar (BdsBootOptionList, L"BootOrder");
  mEnumBootDevice = TRUE;

  VBoxLogFlowFuncLeaveRC(Status);
  return Status;
}

/**
  Build the boot option with the handle parsed in

  @param  Handle                 The handle which present the device path to create
                                 boot option
  @param  BdsBootOptionList      The header of the link list which indexed all
                                 current boot options
  @param  String                 The description of the boot option.

**/
VOID
EFIAPI
BdsLibBuildOptionFromHandle (
  IN  EFI_HANDLE                 Handle,
  IN  LIST_ENTRY                 *BdsBootOptionList,
  IN  CHAR16                     *String
  )
{
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  VBoxLogFlowFuncEnter();
  VBoxLogFlowFuncMarkVar(String, "%s");

  DevicePath = DevicePathFromHandle (Handle);

  //
  // Create and register new boot option
  //
  BdsLibRegisterNewOption (BdsBootOptionList, DevicePath, String, L"BootOrder");
  VBoxLogFlowFuncLeave();
}


/**
  Build the on flash shell boot option with the handle parsed in.

  @param  Handle                 The handle which present the device path to create
                                 on flash shell boot option
  @param  BdsBootOptionList      The header of the link list which indexed all
                                 current boot options

**/
VOID
EFIAPI
BdsLibBuildOptionFromShell (
  IN EFI_HANDLE                  Handle,
  IN OUT LIST_ENTRY              *BdsBootOptionList
  )
{
  EFI_DEVICE_PATH_PROTOCOL          *DevicePath;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH ShellNode;

  DevicePath = DevicePathFromHandle (Handle);

  //
  // Build the shell device path
  //
  EfiInitializeFwVolDevicepathNode (&ShellNode, PcdGetPtr(PcdShellFile));

  DevicePath = AppendDevicePathNode (DevicePath, (EFI_DEVICE_PATH_PROTOCOL *) &ShellNode);

  //
  // Create and register the shell boot option
  //
  BdsLibRegisterNewOption (BdsBootOptionList, DevicePath, L"EFI Internal Shell", L"BootOrder");

}

/**
  Boot from the UEFI spec defined "BootNext" variable.

**/
VOID
EFIAPI
BdsLibBootNext (
  VOID
  )
{
  UINT16            *BootNext;
  UINTN             BootNextSize;
  CHAR16            Buffer[20];
  BDS_COMMON_OPTION *BootOption;
  LIST_ENTRY        TempList;
  UINTN             ExitDataSize;
  CHAR16            *ExitData;

  //
  // Init the boot option name buffer and temp link list
  //
  InitializeListHead (&TempList);
  ZeroMem (Buffer, sizeof (Buffer));

  BootNext = BdsLibGetVariableAndSize (
              L"BootNext",
              &gEfiGlobalVariableGuid,
              &BootNextSize
              );

  //
  // Clear the boot next variable first
  //
  if (BootNext != NULL) {
    gRT->SetVariable (
          L"BootNext",
          &gEfiGlobalVariableGuid,
          EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS | EFI_VARIABLE_NON_VOLATILE,
          0,
          BootNext
          );

    //
    // Start to build the boot option and try to boot
    //
    UnicodeSPrint (Buffer, sizeof (Buffer), L"Boot%04x", *BootNext);
    BootOption = BdsLibVariableToOption (&TempList, Buffer);
    ASSERT (BootOption != NULL);
    BdsLibConnectDevicePath (BootOption->DevicePath);
    BdsLibBootViaBootOption (BootOption, BootOption->DevicePath, &ExitDataSize, &ExitData);
  }

}

/**
  Return the bootable media handle.
  First, check the device is connected
  Second, check whether the device path point to a device which support SimpleFileSystemProtocol,
  Third, detect the the default boot file in the Media, and return the removable Media handle.

  @param  DevicePath  Device Path to a  bootable device

  @return  The bootable media handle. If the media on the DevicePath is not bootable, NULL will return.

**/
EFI_HANDLE
EFIAPI
BdsLibGetBootableHandle (
  IN  EFI_DEVICE_PATH_PROTOCOL      *DevicePath
  )
{
  EFI_STATUS                      Status;
  EFI_TPL                         OldTpl;
  EFI_DEVICE_PATH_PROTOCOL        *UpdatedDevicePath;
  EFI_DEVICE_PATH_PROTOCOL        *DupDevicePath;
  EFI_HANDLE                      Handle;
  EFI_BLOCK_IO_PROTOCOL           *BlockIo;
  VOID                            *Buffer;
  EFI_DEVICE_PATH_PROTOCOL        *TempDevicePath;
  UINTN                           Size;
  UINTN                           TempSize;
  EFI_HANDLE                      ReturnHandle;
  EFI_HANDLE                      *SimpleFileSystemHandles;

  UINTN                           NumberSimpleFileSystemHandles;
  UINTN                           Index;
  EFI_IMAGE_DOS_HEADER            DosHeader;
  EFI_IMAGE_OPTIONAL_HEADER_UNION       HdrData;
  EFI_IMAGE_OPTIONAL_HEADER_PTR_UNION   Hdr;

  UpdatedDevicePath = DevicePath;
  VBoxLogFlowFuncEnter();
  DEBUG((DEBUG_INFO, "%a:%d DevicePath:%s\n", __FUNCTION__, __LINE__, DevicePathToStr(DevicePath))); /* VBox */

  //
  // Enter to critical section to protect the acquired BlockIo instance
  // from getting released due to the USB mass storage hotplug event
  //
  OldTpl = gBS->RaiseTPL (TPL_CALLBACK);

  //
  // Check whether the device is connected
  //
  Status = gBS->LocateDevicePath (&gEfiBlockIoProtocolGuid, &UpdatedDevicePath, &Handle);
  VBoxLogFlowFuncMarkRC(Status);
  if (EFI_ERROR (Status)) {
    //
    // Skip the case that the boot option point to a simple file protocol which does not consume block Io protocol,
    //
    Status = gBS->LocateDevicePath (&gEfiSimpleFileSystemProtocolGuid, &UpdatedDevicePath, &Handle);
    VBoxLogFlowFuncMarkRC(Status);
    if (EFI_ERROR (Status)) {
      //
      // Fail to find the proper BlockIo and simple file protocol, maybe because device not present,  we need to connect it firstly
      //
      UpdatedDevicePath = DevicePath;
      Status            = gBS->LocateDevicePath (&gEfiDevicePathProtocolGuid, &UpdatedDevicePath, &Handle);
      VBoxLogFlowFuncMarkRC(Status);
      gBS->ConnectController (Handle, NULL, NULL, TRUE);
    }
  } else {
    //
    // For removable device boot option, its contained device path only point to the removable device handle,
    // should make sure all its children handles (its child partion or media handles) are created and connected.
    //
    gBS->ConnectController (Handle, NULL, NULL, TRUE);
    //
    // Get BlockIo protocol and check removable attribute
    //
    Status = gBS->HandleProtocol (Handle, &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo);
    ASSERT_EFI_ERROR (Status);

    //
    // Issue a dummy read to the device to check for media change.
    // When the removable media is changed, any Block IO read/write will
    // cause the BlockIo protocol be reinstalled and EFI_MEDIA_CHANGED is
    // returned. After the Block IO protocol is reinstalled, subsequent
    // Block IO read/write will success.
    //
    Buffer = AllocatePool (BlockIo->Media->BlockSize);
    if (Buffer != NULL) {
      BlockIo->ReadBlocks (
               BlockIo,
               BlockIo->Media->MediaId,
               0,
               BlockIo->Media->BlockSize,
               Buffer
               );
      FreePool(Buffer);
    }
  }

  //
  // Detect the the default boot file from removable Media
  //

  //
  // If fail to get bootable handle specified by a USB boot option, the BDS should try to find other bootable device in the same USB bus
  // Try to locate the USB node device path first, if fail then use its previous PCI node to search
  //
  DupDevicePath = DuplicateDevicePath (DevicePath);
  ASSERT (DupDevicePath != NULL);

  UpdatedDevicePath = DupDevicePath;
  Status = gBS->LocateDevicePath (&gEfiDevicePathProtocolGuid, &UpdatedDevicePath, &Handle);
  VBoxLogFlowFuncMarkRC(Status);
  //
  // if the resulting device path point to a usb node, and the usb node is a dummy node, should only let device path only point to the previous Pci node
  // Acpi()/Pci()/Usb() --> Acpi()/Pci()
  //
  if ((DevicePathType (UpdatedDevicePath) == MESSAGING_DEVICE_PATH) &&
      (DevicePathSubType (UpdatedDevicePath) == MSG_USB_DP)) {
    //
    // Remove the usb node, let the device path only point to PCI node
    //
    SetDevicePathEndNode (UpdatedDevicePath);
    UpdatedDevicePath = DupDevicePath;
  } else {
    UpdatedDevicePath = DevicePath;
  }

  //
  // Get the device path size of boot option
  //
  Size = GetDevicePathSize(UpdatedDevicePath) - sizeof (EFI_DEVICE_PATH_PROTOCOL); // minus the end node
  ReturnHandle = NULL;
  gBS->LocateHandleBuffer (
      ByProtocol,
      &gEfiSimpleFileSystemProtocolGuid,
      NULL,
      &NumberSimpleFileSystemHandles,
      &SimpleFileSystemHandles
      );
  VBoxLogFlowFuncMarkVar(NumberSimpleFileSystemHandles, "%d");
  for (Index = 0; Index < NumberSimpleFileSystemHandles; Index++) {
    //
    // Get the device path size of SimpleFileSystem handle
    //
    TempDevicePath = DevicePathFromHandle (SimpleFileSystemHandles[Index]);
    TempSize = GetDevicePathSize (TempDevicePath)- sizeof (EFI_DEVICE_PATH_PROTOCOL); // minus the end node
    DEBUG((DEBUG_INFO, "%a:%d TempDevicePath:%s\n", __FUNCTION__, __LINE__, DevicePathToStr(TempDevicePath))); /* VBox */
    //
    // Check whether the device path of boot option is part of the  SimpleFileSystem handle's device path
    //
    if (Size <= TempSize && CompareMem (TempDevicePath, UpdatedDevicePath, Size)==0) {
#ifdef VBOX
      //
      // Try detect image headers for one of the standard boot files.
      //
      UINT32 IdxFile;
      Hdr.Union = &HdrData;
      for (IdxFile = 0; IdxFile < sizeof(maBdsLibBootFiles) / sizeof(maBdsLibBootFiles[0]); IdxFile++) {
        Status = BdsLibGetImageHeader(SimpleFileSystemHandles[Index], (CHAR16 *)maBdsLibBootFiles[IdxFile], &DosHeader, Hdr);
        VBoxLogFlowFuncMarkRC(Status);
        if (!EFI_ERROR(Status)) {
          break;
        }
        Hdr.Union = &HdrData;
      }
#else  /* !VBOX */
      //
      // Load the default boot file \EFI\BOOT\boot{machinename}.EFI from removable Media
      //  machinename is ia32, ia64, x64, ...
      //
      Hdr.Union = &HdrData;
      Status = BdsLibGetImageHeader (
                 SimpleFileSystemHandles[Index],
                 EFI_REMOVABLE_MEDIA_FILE_NAME,
                 &DosHeader,
                 Hdr
                 );
#endif /* !VBOX */
      if (!EFI_ERROR (Status) &&
        EFI_IMAGE_MACHINE_TYPE_SUPPORTED (Hdr.Pe32->FileHeader.Machine) &&
        Hdr.Pe32->OptionalHeader.Subsystem == EFI_IMAGE_SUBSYSTEM_EFI_APPLICATION) {
        ReturnHandle = SimpleFileSystemHandles[Index];
        break;
      }
    }
#ifdef VBOX
    else
    {
        /* Grrr, sometimes Mac registers Device Path for HD not equal on startting */
# if 1
        EFI_DEVICE_PATH_PROTOCOL *pNode0;
        EFI_DEVICE_PATH_PROTOCOL *pNode1;
        EFI_DEVICE_PATH_PROTOCOL *pTarget = NULL;
        EFI_DEVICE_PATH_PROTOCOL *pFs = NULL;

        pNode0 = NextDevicePathNode(TempDevicePath);
        while (!IsDevicePathEndType (pNode0)) {
            pFs = pNode0;
            pNode0 = NextDevicePathNode (pNode0);
            VBoxLogFlowFuncMarkVar(DevicePathToStr(pNode0), "%s");
            VBoxLogFlowFuncMarkVar(DevicePathType(pNode0), "%x");
            VBoxLogFlowFuncMarkVar(DevicePathSubType(pNode0), "%x");
        }

        pNode1 = NextDevicePathNode(DevicePath);
        while (!IsDevicePathEndType (pNode1)) {
            pTarget = pNode1;
            pNode1 = NextDevicePathNode (pNode1);
            VBoxLogFlowFuncMarkVar(DevicePathToStr(pNode1), "%s");
            VBoxLogFlowFuncMarkVar(DevicePathType(pNode1), "%x");
            VBoxLogFlowFuncMarkVar(DevicePathSubType(pNode1), "%x");
        }
        if (   pTarget
            && pFs
            && DevicePathType(pFs) == DevicePathType(pTarget)
            && DevicePathType(pTarget) == MEDIA_DEVICE_PATH
            && DevicePathSubType(pFs) == DevicePathSubType(pTarget))
        {
            if (DevicePathSubType(pTarget) == MEDIA_HARDDRIVE_DP)
            {
                HARDDRIVE_DEVICE_PATH *pTargetHd = (HARDDRIVE_DEVICE_PATH *)pTarget;
                HARDDRIVE_DEVICE_PATH *pFsHd = (HARDDRIVE_DEVICE_PATH *)pFs;
                /* @todo: is it enough? */
                VBoxLogFlowFuncMarkVar(pTargetHd->PartitionNumber, "%x");
                VBoxLogFlowFuncMarkVar(pTargetHd->PartitionStart, "%lx");
                VBoxLogFlowFuncMarkVar(pTargetHd->PartitionSize, "%lx");
                VBoxLogFlowFuncMarkVar(pFsHd->PartitionNumber, "%x");
                VBoxLogFlowFuncMarkVar(pFsHd->PartitionStart, "%lx");
                VBoxLogFlowFuncMarkVar(pFsHd->PartitionSize, "%lx");
                if (pTargetHd->PartitionNumber == pFsHd->PartitionNumber)
                {
                    ReturnHandle = SimpleFileSystemHandles[Index];
                    break;
                }
            }
            else
            {
                ASSERT(!"DEBUG!!!");
            }
        }
# endif
    }
#endif /* VBOX */
  }

  FreePool(DupDevicePath);

  if (SimpleFileSystemHandles != NULL) {
    FreePool(SimpleFileSystemHandles);
  }

  gBS->RestoreTPL (OldTpl);

  VBoxLogFlowFuncMarkVar(ReturnHandle, "%p");
  VBoxLogFlowFuncLeave();
  return ReturnHandle;
}

/**
  Check to see if the network cable is plugged in. If the DevicePath is not
  connected it will be connected.

  @param  DevicePath             Device Path to check

  @retval TRUE                   DevicePath points to an Network that is connected
  @retval FALSE                  DevicePath does not point to a bootable network

**/
BOOLEAN
BdsLibNetworkBootWithMediaPresent (
  IN  EFI_DEVICE_PATH_PROTOCOL      *DevicePath
  )
{
  EFI_STATUS                      Status;
  EFI_DEVICE_PATH_PROTOCOL        *UpdatedDevicePath;
  EFI_HANDLE                      Handle;
  EFI_SIMPLE_NETWORK_PROTOCOL     *Snp;
  BOOLEAN                         MediaPresent;
  UINT32                          InterruptStatus;

  MediaPresent = FALSE;

  UpdatedDevicePath = DevicePath;
  //
  // Locate Load File Protocol for PXE boot option first
  //
  Status = gBS->LocateDevicePath (&gEfiLoadFileProtocolGuid, &UpdatedDevicePath, &Handle);
  if (EFI_ERROR (Status)) {
    //
    // Device not present so see if we need to connect it
    //
    Status = BdsLibConnectDevicePath (DevicePath);
    if (!EFI_ERROR (Status)) {
      //
      // This one should work after we did the connect
      //
      Status = gBS->LocateDevicePath (&gEfiLoadFileProtocolGuid, &UpdatedDevicePath, &Handle);
    }
  }

  if (!EFI_ERROR (Status)) {
    Status = gBS->HandleProtocol (Handle, &gEfiSimpleNetworkProtocolGuid, (VOID **)&Snp);
    if (EFI_ERROR (Status)) {
      //
      // Failed to open SNP from this handle, try to get SNP from parent handle
      //
      UpdatedDevicePath = DevicePathFromHandle (Handle);
      if (UpdatedDevicePath != NULL) {
        Status = gBS->LocateDevicePath (&gEfiSimpleNetworkProtocolGuid, &UpdatedDevicePath, &Handle);
        if (!EFI_ERROR (Status)) {
          //
          // SNP handle found, get SNP from it
          //
          Status = gBS->HandleProtocol (Handle, &gEfiSimpleNetworkProtocolGuid, (VOID **) &Snp);
        }
      }
    }

    if (!EFI_ERROR (Status)) {
      if (Snp->Mode->MediaPresentSupported) {
        if (Snp->Mode->State == EfiSimpleNetworkInitialized) {
          //
          // Invoke Snp->GetStatus() to refresh the media status
          //
          Snp->GetStatus (Snp, &InterruptStatus, NULL);

          //
          // In case some one else is using the SNP check to see if it's connected
          //
          MediaPresent = Snp->Mode->MediaPresent;
        } else {
          //
          // No one is using SNP so we need to Start and Initialize so
          // MediaPresent will be valid.
          //
          Status = Snp->Start (Snp);
          if (!EFI_ERROR (Status)) {
            Status = Snp->Initialize (Snp, 0, 0);
            if (!EFI_ERROR (Status)) {
              MediaPresent = Snp->Mode->MediaPresent;
              Snp->Shutdown (Snp);
            }
            Snp->Stop (Snp);
          }
        }
      } else {
        MediaPresent = TRUE;
      }
    }
  }

  return MediaPresent;
}

/**
  For a bootable Device path, return its boot type.

  @param  DevicePath                      The bootable device Path to check

  @retval BDS_EFI_MEDIA_HD_BOOT           If given device path contains MEDIA_DEVICE_PATH type device path node
                                          which subtype is MEDIA_HARDDRIVE_DP
  @retval BDS_EFI_MEDIA_CDROM_BOOT        If given device path contains MEDIA_DEVICE_PATH type device path node
                                          which subtype is MEDIA_CDROM_DP
  @retval BDS_EFI_ACPI_FLOPPY_BOOT        If given device path contains ACPI_DEVICE_PATH type device path node
                                          which HID is floppy device.
  @retval BDS_EFI_MESSAGE_ATAPI_BOOT      If given device path contains MESSAGING_DEVICE_PATH type device path node
                                          and its last device path node's subtype is MSG_ATAPI_DP.
  @retval BDS_EFI_MESSAGE_SCSI_BOOT       If given device path contains MESSAGING_DEVICE_PATH type device path node
                                          and its last device path node's subtype is MSG_SCSI_DP.
  @retval BDS_EFI_MESSAGE_USB_DEVICE_BOOT If given device path contains MESSAGING_DEVICE_PATH type device path node
                                          and its last device path node's subtype is MSG_USB_DP.
  @retval BDS_EFI_MESSAGE_MISC_BOOT       If the device path not contains any media device path node,  and
                                          its last device path node point to a message device path node.
  @retval BDS_LEGACY_BBS_BOOT             If given device path contains BBS_DEVICE_PATH type device path node.
  @retval BDS_EFI_UNSUPPORT               An EFI Removable BlockIO device path not point to a media and message device,

**/
UINT32
EFIAPI
BdsGetBootTypeFromDevicePath (
  IN  EFI_DEVICE_PATH_PROTOCOL     *DevicePath
  )
{
  ACPI_HID_DEVICE_PATH          *Acpi;
  EFI_DEVICE_PATH_PROTOCOL      *TempDevicePath;
  EFI_DEVICE_PATH_PROTOCOL      *LastDeviceNode;
  UINT32                        BootType;

  if (NULL == DevicePath) {
    return BDS_EFI_UNSUPPORT;
  }

  TempDevicePath = DevicePath;

  while (!IsDevicePathEndType (TempDevicePath)) {
    switch (DevicePathType (TempDevicePath)) {
      case BBS_DEVICE_PATH:
         return BDS_LEGACY_BBS_BOOT;
      case MEDIA_DEVICE_PATH:
        if (DevicePathSubType (TempDevicePath) == MEDIA_HARDDRIVE_DP) {
          return BDS_EFI_MEDIA_HD_BOOT;
        } else if (DevicePathSubType (TempDevicePath) == MEDIA_CDROM_DP) {
          return BDS_EFI_MEDIA_CDROM_BOOT;
        }
        break;
      case ACPI_DEVICE_PATH:
        Acpi = (ACPI_HID_DEVICE_PATH *) TempDevicePath;
        if (EISA_ID_TO_NUM (Acpi->HID) == 0x0604) {
          return BDS_EFI_ACPI_FLOPPY_BOOT;
        }
        break;
      case MESSAGING_DEVICE_PATH:
        //
        // Get the last device path node
        //
        LastDeviceNode = NextDevicePathNode (TempDevicePath);
        if (DevicePathSubType(LastDeviceNode) == MSG_DEVICE_LOGICAL_UNIT_DP) {
          //
          // if the next node type is Device Logical Unit, which specify the Logical Unit Number (LUN),
          // skip it
          //
          LastDeviceNode = NextDevicePathNode (LastDeviceNode);
        }
        //
        // if the device path not only point to driver device, it is not a messaging device path,
        //
        if (!IsDevicePathEndType (LastDeviceNode)) {
          break;
        }

        switch (DevicePathSubType (TempDevicePath)) {
        case MSG_ATAPI_DP:
          BootType = BDS_EFI_MESSAGE_ATAPI_BOOT;
          break;

        case MSG_USB_DP:
          BootType = BDS_EFI_MESSAGE_USB_DEVICE_BOOT;
          break;

        case MSG_SCSI_DP:
          BootType = BDS_EFI_MESSAGE_SCSI_BOOT;
          break;

        case MSG_SATA_DP:
          BootType = BDS_EFI_MESSAGE_SATA_BOOT;
          break;

        case MSG_MAC_ADDR_DP:
        case MSG_VLAN_DP:
        case MSG_IPv4_DP:
        case MSG_IPv6_DP:
          BootType = BDS_EFI_MESSAGE_MAC_BOOT;
          break;

        default:
          BootType = BDS_EFI_MESSAGE_MISC_BOOT;
          break;
        }
        return BootType;

      default:
        break;
    }
    TempDevicePath = NextDevicePathNode (TempDevicePath);
  }

  return BDS_EFI_UNSUPPORT;
}

/**
  Check whether the Device path in a boot option point to a valid bootable device,
  And if CheckMedia is true, check the device is ready to boot now.

  @param  DevPath     the Device path in a boot option
  @param  CheckMedia  if true, check the device is ready to boot now.

  @retval TRUE        the Device path  is valid
  @retval FALSE       the Device path  is invalid .

**/
BOOLEAN
EFIAPI
BdsLibIsValidEFIBootOptDevicePath (
  IN EFI_DEVICE_PATH_PROTOCOL     *DevPath,
  IN BOOLEAN                      CheckMedia
  )
{
  return BdsLibIsValidEFIBootOptDevicePathExt (DevPath, CheckMedia, NULL);
}

/**
  Check whether the Device path in a boot option point to a valid bootable device,
  And if CheckMedia is true, check the device is ready to boot now.
  If Description is not NULL and the device path point to a fixed BlockIo
  device, check the description whether conflict with other auto-created
  boot options.

  @param  DevPath     the Device path in a boot option
  @param  CheckMedia  if true, check the device is ready to boot now.
  @param  Description the description in a boot option

  @retval TRUE        the Device path  is valid
  @retval FALSE       the Device path  is invalid .

**/
BOOLEAN
EFIAPI
BdsLibIsValidEFIBootOptDevicePathExt (
  IN EFI_DEVICE_PATH_PROTOCOL     *DevPath,
  IN BOOLEAN                      CheckMedia,
  IN CHAR16                       *Description
  )
{
  EFI_STATUS                Status;
  EFI_HANDLE                Handle;
  EFI_DEVICE_PATH_PROTOCOL  *TempDevicePath;
  EFI_DEVICE_PATH_PROTOCOL  *LastDeviceNode;
  EFI_BLOCK_IO_PROTOCOL     *BlockIo;

  VBoxLog(("BdsLibIsValidEFIBootOptDevicePathExt: DevPath=%s CheckMedia=%d Description=%s\n",
           DevicePathToStr(DevPath), CheckMedia, Description));

  TempDevicePath = DevPath;
  LastDeviceNode = DevPath;

  //
  // Check if it's a valid boot option for network boot device.
  // Check if there is EfiLoadFileProtocol installed.
  // If yes, that means there is a boot option for network.
  //
  Status = gBS->LocateDevicePath (
                  &gEfiLoadFileProtocolGuid,
                  &TempDevicePath,
                  &Handle
                  );
  VBoxLogFlowFuncMarkRC(Status);
  if (EFI_ERROR (Status)) {
    //
    // Device not present so see if we need to connect it
    //
    TempDevicePath = DevPath;
    BdsLibConnectDevicePath (TempDevicePath);
    Status = gBS->LocateDevicePath (
                    &gEfiLoadFileProtocolGuid,
                    &TempDevicePath,
                    &Handle
                    );
  }

  VBoxLogFlowFuncMarkRC(Status);
  if (!EFI_ERROR (Status)) {
    if (!IsDevicePathEnd (TempDevicePath)) {
      //
      // LoadFile protocol is not installed on handle with exactly the same DevPath
      //
      VBoxLog(("BdsLibIsValidEFIBootOptDevicePathExt: returns FALSE (#1)\n"));
      return FALSE;
    }

    if (CheckMedia) {
      //
      // Test if it is ready to boot now
      //
      if (BdsLibNetworkBootWithMediaPresent(DevPath)) {
        VBoxLog(("BdsLibIsValidEFIBootOptDevicePathExt: returns TRUE (#2)\n"));
        return TRUE;
      }
    } else {
      VBoxLog(("BdsLibIsValidEFIBootOptDevicePathExt: returns TRUE (#3)\n"));
      return TRUE;
    }
  }

  //
  // If the boot option point to a file, it is a valid EFI boot option,
  // and assume it is ready to boot now
  //
  while (!IsDevicePathEnd (TempDevicePath)) {
    //
    // If there is USB Class or USB WWID device path node, treat it as valid EFI
    // Boot Option. BdsExpandUsbShortFormDevicePath () will be used to expand it
    // to full device path.
    //
    if ((DevicePathType (TempDevicePath) == MESSAGING_DEVICE_PATH) &&
        ((DevicePathSubType (TempDevicePath) == MSG_USB_CLASS_DP) ||
         (DevicePathSubType (TempDevicePath) == MSG_USB_WWID_DP))) {
      VBoxLog(("BdsLibIsValidEFIBootOptDevicePathExt: returns TRUE (#4)\n"));
      return TRUE;
    }

    LastDeviceNode = TempDevicePath;
    TempDevicePath = NextDevicePathNode (TempDevicePath);
  }
  if ((DevicePathType (LastDeviceNode) == MEDIA_DEVICE_PATH) &&
    (DevicePathSubType (LastDeviceNode) == MEDIA_FILEPATH_DP)) {
    VBoxLog(("BdsLibIsValidEFIBootOptDevicePathExt: returns TRUE (#5)\n"));
    return TRUE;
  }

  //
  // Check if it's a valid boot option for internal FV application
  //
  if (EfiGetNameGuidFromFwVolDevicePathNode ((MEDIA_FW_VOL_FILEPATH_DEVICE_PATH *) LastDeviceNode) != NULL) {
    //
    // If the boot option point to internal FV application, make sure it is valid
    //
    TempDevicePath = DevPath;
    Status = BdsLibUpdateFvFileDevicePath (
               &TempDevicePath,
               EfiGetNameGuidFromFwVolDevicePathNode ((MEDIA_FW_VOL_FILEPATH_DEVICE_PATH *) LastDeviceNode)
               );
    if (Status == EFI_ALREADY_STARTED) {
      VBoxLog(("BdsLibIsValidEFIBootOptDevicePathExt: returns TRUE (#6)\n"));
      return TRUE;
    } else {
      if (Status == EFI_SUCCESS) {
        FreePool (TempDevicePath);
      }
      VBoxLog(("BdsLibIsValidEFIBootOptDevicePathExt: returns FALSE (#7)\n"));
      return FALSE;
    }
  }

  //
  // If the boot option point to a blockIO device:
  //    if it is a removable blockIo device, it is valid.
  //    if it is a fixed blockIo device, check its description confliction.
  //
  TempDevicePath = DevPath;
  Status = gBS->LocateDevicePath (&gEfiBlockIoProtocolGuid, &TempDevicePath, &Handle);
  VBoxLogFlowFuncMarkRC(Status);
  if (EFI_ERROR (Status)) {
    //
    // Device not present so see if we need to connect it
    //
    Status = BdsLibConnectDevicePath (DevPath);
    VBoxLogFlowFuncMarkRC(Status);
    if (!EFI_ERROR (Status)) {
      //
      // Try again to get the Block Io protocol after we did the connect
      //
      TempDevicePath = DevPath;
      Status = gBS->LocateDevicePath (&gEfiBlockIoProtocolGuid, &TempDevicePath, &Handle);
      VBoxLogFlowFuncMarkRC(Status);
    }
  }

  VBoxLogFlowFuncMarkRC(Status);
  if (!EFI_ERROR (Status)) {
    Status = gBS->HandleProtocol (Handle, &gEfiBlockIoProtocolGuid, (VOID **)&BlockIo);
    VBoxLogFlowFuncMarkRC(Status);
    if (!EFI_ERROR (Status)) {
      if (CheckMedia) {
        //
        // Test if it is ready to boot now
        //
        if (BdsLibGetBootableHandle (DevPath) != NULL) {
          VBoxLog(("BdsLibIsValidEFIBootOptDevicePathExt: returns TRUE (#8)\n"));
          return TRUE;
        }
      } else {
        VBoxLog(("BdsLibIsValidEFIBootOptDevicePathExt: returns TRUE (#9)\n"));
        return TRUE;
      }
    }
  } else {
    //
    // if the boot option point to a simple file protocol which does not consume block Io protocol, it is also a valid EFI boot option,
    //
    Status = gBS->LocateDevicePath (&gEfiSimpleFileSystemProtocolGuid, &TempDevicePath, &Handle);
    VBoxLogFlowFuncMarkRC(Status);
    if (!EFI_ERROR (Status)) {
      if (CheckMedia) {
        //
        // Test if it is ready to boot now
        //
        if (BdsLibGetBootableHandle (DevPath) != NULL) {
          VBoxLog(("BdsLibIsValidEFIBootOptDevicePathExt: returns TRUE (#10)\n"));
          return TRUE;
        }
      } else {
        VBoxLog(("BdsLibIsValidEFIBootOptDevicePathExt: returns TRUE (#10)\n"));
        return TRUE;
      }
    }
  }

#ifndef VBOX
  return FALSE;
#else
  VBoxLog(("BdsLibIsValidEFIBootOptDevicePathExt: returns TRUE (#11) - Status 0x%x(%r)\n", Status, Status));
  return TRUE;
#endif
}


/**
  According to a file guild, check a Fv file device path is valid. If it is invalid,
  try to return the valid device path.
  FV address maybe changes for memory layout adjust from time to time, use this function
  could promise the Fv file device path is right.

  @param  DevicePath             on input, the Fv file device path need to check on
                                 output, the updated valid Fv file device path
  @param  FileGuid               the Fv file guild

  @retval EFI_INVALID_PARAMETER  the input DevicePath or FileGuid is invalid
                                 parameter
  @retval EFI_UNSUPPORTED        the input DevicePath does not contain Fv file
                                 guild at all
  @retval EFI_ALREADY_STARTED    the input DevicePath has pointed to Fv file, it is
                                 valid
  @retval EFI_SUCCESS            has successfully updated the invalid DevicePath,
                                 and return the updated device path in DevicePath

**/
EFI_STATUS
EFIAPI
BdsLibUpdateFvFileDevicePath (
  IN  OUT EFI_DEVICE_PATH_PROTOCOL      ** DevicePath,
  IN  EFI_GUID                          *FileGuid
  )
{
  EFI_DEVICE_PATH_PROTOCOL      *TempDevicePath;
  EFI_DEVICE_PATH_PROTOCOL      *LastDeviceNode;
  EFI_STATUS                    Status;
  EFI_GUID                      *GuidPoint;
  UINTN                         Index;
  UINTN                         FvHandleCount;
  EFI_HANDLE                    *FvHandleBuffer;
  EFI_FV_FILETYPE               Type;
  UINTN                         Size;
  EFI_FV_FILE_ATTRIBUTES        Attributes;
  UINT32                        AuthenticationStatus;
  BOOLEAN                       FindFvFile;
  EFI_LOADED_IMAGE_PROTOCOL     *LoadedImage;
  EFI_FIRMWARE_VOLUME2_PROTOCOL *Fv;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH FvFileNode;
  EFI_HANDLE                    FoundFvHandle;
  EFI_DEVICE_PATH_PROTOCOL      *NewDevicePath;

  if ((DevicePath == NULL) || (*DevicePath == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  if (FileGuid == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Check whether the device path point to the default the input Fv file
  //
  TempDevicePath = *DevicePath;
  LastDeviceNode = TempDevicePath;
  while (!IsDevicePathEnd (TempDevicePath)) {
     LastDeviceNode = TempDevicePath;
     TempDevicePath = NextDevicePathNode (TempDevicePath);
  }
  GuidPoint = EfiGetNameGuidFromFwVolDevicePathNode (
                (MEDIA_FW_VOL_FILEPATH_DEVICE_PATH *) LastDeviceNode
                );
  if (GuidPoint == NULL) {
    //
    // if this option does not points to a Fv file, just return EFI_UNSUPPORTED
    //
    return EFI_UNSUPPORTED;
  }
  if (!CompareGuid (GuidPoint, FileGuid)) {
    //
    // If the Fv file is not the input file guid, just return EFI_UNSUPPORTED
    //
    return EFI_UNSUPPORTED;
  }

  //
  // Check whether the input Fv file device path is valid
  //
  TempDevicePath = *DevicePath;
  FoundFvHandle = NULL;
  Status = gBS->LocateDevicePath (
                  &gEfiFirmwareVolume2ProtocolGuid,
                  &TempDevicePath,
                  &FoundFvHandle
                  );
  if (!EFI_ERROR (Status)) {
    Status = gBS->HandleProtocol (
                    FoundFvHandle,
                    &gEfiFirmwareVolume2ProtocolGuid,
                    (VOID **) &Fv
                    );
    if (!EFI_ERROR (Status)) {
      //
      // Set FV ReadFile Buffer as NULL, only need to check whether input Fv file exist there
      //
      Status = Fv->ReadFile (
                    Fv,
                    FileGuid,
                    NULL,
                    &Size,
                    &Type,
                    &Attributes,
                    &AuthenticationStatus
                    );
      if (!EFI_ERROR (Status)) {
        return EFI_ALREADY_STARTED;
      }
    }
  }

  //
  // Look for the input wanted FV file in current FV
  // First, try to look for in Bds own FV. Bds and input wanted FV file usually are in the same FV
  //
  FindFvFile = FALSE;
  FoundFvHandle = NULL;
  Status = gBS->HandleProtocol (
             gImageHandle,
             &gEfiLoadedImageProtocolGuid,
             (VOID **) &LoadedImage
             );
  if (!EFI_ERROR (Status)) {
    Status = gBS->HandleProtocol (
                    LoadedImage->DeviceHandle,
                    &gEfiFirmwareVolume2ProtocolGuid,
                    (VOID **) &Fv
                    );
    if (!EFI_ERROR (Status)) {
      Status = Fv->ReadFile (
                    Fv,
                    FileGuid,
                    NULL,
                    &Size,
                    &Type,
                    &Attributes,
                    &AuthenticationStatus
                    );
      if (!EFI_ERROR (Status)) {
        FindFvFile = TRUE;
        FoundFvHandle = LoadedImage->DeviceHandle;
      }
    }
  }
  //
  // Second, if fail to find, try to enumerate all FV
  //
  if (!FindFvFile) {
    FvHandleBuffer = NULL;
    gBS->LocateHandleBuffer (
          ByProtocol,
          &gEfiFirmwareVolume2ProtocolGuid,
          NULL,
          &FvHandleCount,
          &FvHandleBuffer
          );
    for (Index = 0; Index < FvHandleCount; Index++) {
      gBS->HandleProtocol (
            FvHandleBuffer[Index],
            &gEfiFirmwareVolume2ProtocolGuid,
            (VOID **) &Fv
            );

      Status = Fv->ReadFile (
                    Fv,
                    FileGuid,
                    NULL,
                    &Size,
                    &Type,
                    &Attributes,
                    &AuthenticationStatus
                    );
      if (EFI_ERROR (Status)) {
        //
        // Skip if input Fv file not in the FV
        //
        continue;
      }
      FindFvFile = TRUE;
      FoundFvHandle = FvHandleBuffer[Index];
      break;
    }

    if (FvHandleBuffer != NULL) {
      FreePool (FvHandleBuffer);
    }
  }

  if (FindFvFile) {
    //
    // Build the shell device path
    //
    NewDevicePath = DevicePathFromHandle (FoundFvHandle);
    EfiInitializeFwVolDevicepathNode (&FvFileNode, FileGuid);
    NewDevicePath = AppendDevicePathNode (NewDevicePath, (EFI_DEVICE_PATH_PROTOCOL *) &FvFileNode);
    *DevicePath = NewDevicePath;
    return EFI_SUCCESS;
  }
  return EFI_NOT_FOUND;
}
