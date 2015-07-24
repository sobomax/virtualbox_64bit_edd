/** @file
  Platform BDS customizations.

  Copyright (c) 2004 - 2012, Intel Corporation. All rights reserved.<BR>
  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#include "BdsPlatform.h"
#ifdef VBOX
#include "VBoxPkg.h"
#include "DevEFI.h"
#include "iprt/asm.h"
#include "Library/PrintLib.h"
/* Hmmm Uga Draw wasn't pointed in BDS, so perhaps it's totally dead */
#include <Protocol/UgaDraw.h>
#include <Guid/EventGroup.h> /* gEfiEventReadyToBootGuid */
#endif


//
// Global data
//

VOID          *mEfiDevPathNotifyReg;
EFI_EVENT     mEfiDevPathEvent;
VOID          *mEmuVariableEventReg;
EFI_EVENT     mEmuVariableEvent;
BOOLEAN       mDetectVgaOnly;

#ifdef VBOX
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *TextOutputProtocol;
static EFI_GRAPHICS_OUTPUT_PROTOCOL    *Gop;
static EFI_UGA_DRAW_PROTOCOL           *Uga;

/*
 *   @todo move this function to the library.
 */
static UINT32
GetVmVariable(UINT32 Variable, CHAR8* Buffer, UINT32 Size )
{
    UINT32 VarLen, i;


    ASMOutU32(EFI_INFO_PORT, Variable);
    VarLen = ASMInU32(EFI_INFO_PORT);

    for (i=0; i < VarLen && i < Size; i++)
    {
        Buffer[i] = ASMInU8(EFI_INFO_PORT);
    }

    return VarLen;
}

static VOID
EFIAPI
VBoxConsoleSwitchMode (
  IN EFI_EVENT                Event,
  IN VOID                     *Context
  )
{
    EFI_STATUS r = EFI_NOT_FOUND; /* Neither GOP nor UGA is found*/
    EFI_TPL               OldTpl;
    OldTpl = gBS->RaiseTPL (TPL_NOTIFY);
    if (Gop)
    {
        UINT32 mode = 2;
        GetVmVariable(EFI_INFO_INDEX_GOP_MODE, (CHAR8 *)&mode, sizeof(UINT32));
        r = Gop->SetMode(Gop, mode);
    }
    else if (Uga)
    {
        UINT32 H = 1027;
        UINT32 V = 768;
        GetVmVariable(EFI_INFO_INDEX_UGA_HORISONTAL_RESOLUTION, (CHAR8 *)&H, sizeof(UINT32));
        GetVmVariable(EFI_INFO_INDEX_UGA_VERTICAL_RESOLUTION, (CHAR8 *)&V, sizeof(UINT32));
        r = Uga->SetMode(Uga, H, V, 32, 60);
    }

    if(EFI_ERROR(r))
        goto done;

    r = TextOutputProtocol->SetMode(TextOutputProtocol, TextOutputProtocol->Mode->MaxMode);
    if(EFI_ERROR(r))
        goto done;
    done:
    gBS->RestoreTPL (OldTpl);
    return;
}

EFI_STATUS
EFIAPI
VBoxConsoleInit()
{
    EFI_EVENT event;

    gBS->LocateProtocol(&gEfiSimpleTextOutProtocolGuid, NULL, (VOID **)&TextOutputProtocol);
    gBS->LocateProtocol(&gEfiUgaDrawProtocolGuid, NULL, (VOID **)&Uga);
    gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
    gBS->CreateEventEx(EVT_NOTIFY_SIGNAL, TPL_NOTIFY, VBoxConsoleSwitchMode, NULL, &gEfiEventReadyToBootGuid, &event);
    return EFI_SUCCESS;
}
#endif /* !VBOX */


//
// Type definitions
//

typedef
EFI_STATUS
(EFIAPI *PROTOCOL_INSTANCE_CALLBACK)(
  IN EFI_HANDLE           Handle,
  IN VOID                 *Instance,
  IN VOID                 *Context
  );

/**
  @param[in]  Handle - Handle of PCI device instance
  @param[in]  PciIo - PCI IO protocol instance
  @param[in]  Pci - PCI Header register block
**/
typedef
EFI_STATUS
(EFIAPI *VISIT_PCI_INSTANCE_CALLBACK)(
  IN EFI_HANDLE           Handle,
  IN EFI_PCI_IO_PROTOCOL  *PciIo,
  IN PCI_TYPE00           *Pci
  );


//
// Function prototypes
//

EFI_STATUS
VisitAllInstancesOfProtocol (
  IN EFI_GUID                    *Id,
  IN PROTOCOL_INSTANCE_CALLBACK  CallBackFunction,
  IN VOID                        *Context
  );

EFI_STATUS
VisitAllPciInstancesOfProtocol (
  IN VISIT_PCI_INSTANCE_CALLBACK CallBackFunction
  );

VOID
InstallDevicePathCallback (
  VOID
  );

#ifndef VBOX
STATIC
VOID
LoadVideoRom (
  VOID
  );


STATIC
EFI_STATUS
PciRomLoadEfiDriversFromRomImage (
  IN EFI_PHYSICAL_ADDRESS    Rom,
  IN UINTN                   RomSize
  );
#endif
//
// BDS Platform Functions
//
VOID
EFIAPI
PlatformBdsInit (
  VOID
  )
/*++

Routine Description:

  Platform Bds init. Incude the platform firmware vendor, revision
  and so crc check.

Arguments:

Returns:

  None.

--*/
{
  DEBUG ((EFI_D_INFO, "PlatformBdsInit\n"));
  InstallDevicePathCallback ();
#ifndef VBOX
  LoadVideoRom ();
#endif
}


EFI_STATUS
ConnectRootBridge (
  VOID
  )
/*++

Routine Description:

  Connect RootBridge

Arguments:

  None.

Returns:

  EFI_SUCCESS             - Connect RootBridge successfully.
  EFI_STATUS              - Connect RootBridge fail.

--*/
{
  EFI_STATUS                Status;
  EFI_HANDLE                RootHandle;

  //
  // Make all the PCI_IO protocols on PCI Seg 0 show up
  //
  BdsLibConnectDevicePath (gPlatformRootBridges[0]);

  Status = gBS->LocateDevicePath (
                  &gEfiDevicePathProtocolGuid,
                  &gPlatformRootBridges[0],
                  &RootHandle
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gBS->ConnectController (RootHandle, NULL, NULL, FALSE);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return EFI_SUCCESS;
}


EFI_STATUS
PrepareLpcBridgeDevicePath (
  IN EFI_HANDLE                DeviceHandle
  )
/*++

Routine Description:

  Add IsaKeyboard to ConIn,
  add IsaSerial to ConOut, ConIn, ErrOut.
  LPC Bridge: 06 01 00

Arguments:

  DeviceHandle            - Handle of PCIIO protocol.

Returns:

  EFI_SUCCESS             - LPC bridge is added to ConOut, ConIn, and ErrOut.
  EFI_STATUS              - No LPC bridge is added.

--*/
{
  EFI_STATUS                Status;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  EFI_DEVICE_PATH_PROTOCOL  *TempDevicePath;
  CHAR16                    *DevPathStr;

  DevicePath = NULL;
  Status = gBS->HandleProtocol (
                  DeviceHandle,
                  &gEfiDevicePathProtocolGuid,
                  (VOID*)&DevicePath
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  TempDevicePath = DevicePath;

  //
  // Register Keyboard
  //
  DevicePath = AppendDevicePathNode (DevicePath, (EFI_DEVICE_PATH_PROTOCOL *)&gPnpPs2KeyboardDeviceNode);

  BdsLibUpdateConsoleVariable (VarConsoleInp, DevicePath, NULL);

  //
  // Register COM1
  //
  DevicePath = TempDevicePath;
  gPnp16550ComPortDeviceNode.UID = 0;

  DevicePath = AppendDevicePathNode (DevicePath, (EFI_DEVICE_PATH_PROTOCOL *)&gPnp16550ComPortDeviceNode);
  DevicePath = AppendDevicePathNode (DevicePath, (EFI_DEVICE_PATH_PROTOCOL *)&gUartDeviceNode);
  DevicePath = AppendDevicePathNode (DevicePath, (EFI_DEVICE_PATH_PROTOCOL *)&gTerminalTypeDeviceNode);

  //
  // Print Device Path
  //
  DevPathStr = DevicePathToStr(DevicePath);
  DEBUG((
    EFI_D_INFO,
    "BdsPlatform.c+%d: COM%d DevPath: %s\n",
    __LINE__,
    gPnp16550ComPortDeviceNode.UID + 1,
    DevPathStr
    ));
  FreePool(DevPathStr);

  BdsLibUpdateConsoleVariable (VarConsoleOut, DevicePath, NULL);
  BdsLibUpdateConsoleVariable (VarConsoleInp, DevicePath, NULL);
  BdsLibUpdateConsoleVariable (VarErrorOut, DevicePath, NULL);

  //
  // Register COM2
  //
  DevicePath = TempDevicePath;
  gPnp16550ComPortDeviceNode.UID = 1;

  DevicePath = AppendDevicePathNode (DevicePath, (EFI_DEVICE_PATH_PROTOCOL *)&gPnp16550ComPortDeviceNode);
  DevicePath = AppendDevicePathNode (DevicePath, (EFI_DEVICE_PATH_PROTOCOL *)&gUartDeviceNode);
  DevicePath = AppendDevicePathNode (DevicePath, (EFI_DEVICE_PATH_PROTOCOL *)&gTerminalTypeDeviceNode);

  //
  // Print Device Path
  //
  DevPathStr = DevicePathToStr(DevicePath);
  DEBUG((
    EFI_D_INFO,
    "BdsPlatform.c+%d: COM%d DevPath: %s\n",
    __LINE__,
    gPnp16550ComPortDeviceNode.UID + 1,
    DevPathStr
    ));
  FreePool(DevPathStr);

  BdsLibUpdateConsoleVariable (VarConsoleOut, DevicePath, NULL);
  BdsLibUpdateConsoleVariable (VarConsoleInp, DevicePath, NULL);
  BdsLibUpdateConsoleVariable (VarErrorOut, DevicePath, NULL);

  return EFI_SUCCESS;
}

EFI_STATUS
GetGopDevicePath (
   IN  EFI_DEVICE_PATH_PROTOCOL *PciDevicePath,
   OUT EFI_DEVICE_PATH_PROTOCOL **GopDevicePath
   )
{
  UINTN                           Index;
  EFI_STATUS                      Status;
  EFI_HANDLE                      PciDeviceHandle;
  EFI_DEVICE_PATH_PROTOCOL        *TempDevicePath;
  EFI_DEVICE_PATH_PROTOCOL        *TempPciDevicePath;
  UINTN                           GopHandleCount;
  EFI_HANDLE                      *GopHandleBuffer;

  if (PciDevicePath == NULL || GopDevicePath == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  //
  // Initialize the GopDevicePath to be PciDevicePath
  //
  *GopDevicePath    = PciDevicePath;
  TempPciDevicePath = PciDevicePath;

  Status = gBS->LocateDevicePath (
                  &gEfiDevicePathProtocolGuid,
                  &TempPciDevicePath,
                  &PciDeviceHandle
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Try to connect this handle, so that GOP dirver could start on this
  // device and create child handles with GraphicsOutput Protocol installed
  // on them, then we get device paths of these child handles and select
  // them as possible console device.
  //
  gBS->ConnectController (PciDeviceHandle, NULL, NULL, FALSE);

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiGraphicsOutputProtocolGuid,
                  NULL,
                  &GopHandleCount,
                  &GopHandleBuffer
                  );
  if (!EFI_ERROR (Status)) {
    //
    // Add all the child handles as possible Console Device
    //
    for (Index = 0; Index < GopHandleCount; Index++) {
      Status = gBS->HandleProtocol (GopHandleBuffer[Index], &gEfiDevicePathProtocolGuid, (VOID*)&TempDevicePath);
      if (EFI_ERROR (Status)) {
        continue;
      }
      if (CompareMem (
            PciDevicePath,
            TempDevicePath,
            GetDevicePathSize (PciDevicePath) - END_DEVICE_PATH_LENGTH
            ) == 0) {
        //
        // In current implementation, we only enable one of the child handles
        // as console device, i.e. sotre one of the child handle's device
        // path to variable "ConOut"
        // In futhure, we could select all child handles to be console device
        //

        *GopDevicePath = TempDevicePath;

        //
        // Delete the PCI device's path that added by GetPlugInPciVgaDevicePath()
        // Add the integrity GOP device path.
        //
        BdsLibUpdateConsoleVariable (VarConsoleOutDev, NULL, PciDevicePath);
        BdsLibUpdateConsoleVariable (VarConsoleOutDev, TempDevicePath, NULL);
      }
    }
    gBS->FreePool (GopHandleBuffer);
  }

  return EFI_SUCCESS;
}

EFI_STATUS
PreparePciVgaDevicePath (
  IN EFI_HANDLE                DeviceHandle
  )
/*++

Routine Description:

  Add PCI VGA to ConOut.
  PCI VGA: 03 00 00

Arguments:

  DeviceHandle            - Handle of PCIIO protocol.

Returns:

  EFI_SUCCESS             - PCI VGA is added to ConOut.
  EFI_STATUS              - No PCI VGA device is added.

--*/
{
  EFI_STATUS                Status;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  EFI_DEVICE_PATH_PROTOCOL  *GopDevicePath;

  DevicePath = NULL;
  Status = gBS->HandleProtocol (
                  DeviceHandle,
                  &gEfiDevicePathProtocolGuid,
                  (VOID*)&DevicePath
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  GetGopDevicePath (DevicePath, &GopDevicePath);
  DevicePath = GopDevicePath;

  BdsLibUpdateConsoleVariable (VarConsoleOut, DevicePath, NULL);

  return EFI_SUCCESS;
}

EFI_STATUS
PreparePciSerialDevicePath (
  IN EFI_HANDLE                DeviceHandle
  )
/*++

Routine Description:

  Add PCI Serial to ConOut, ConIn, ErrOut.
  PCI Serial: 07 00 02

Arguments:

  DeviceHandle            - Handle of PCIIO protocol.

Returns:

  EFI_SUCCESS             - PCI Serial is added to ConOut, ConIn, and ErrOut.
  EFI_STATUS              - No PCI Serial device is added.

--*/
{
  EFI_STATUS                Status;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;

  DevicePath = NULL;
  Status = gBS->HandleProtocol (
                  DeviceHandle,
                  &gEfiDevicePathProtocolGuid,
                  (VOID*)&DevicePath
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  DevicePath = AppendDevicePathNode (DevicePath, (EFI_DEVICE_PATH_PROTOCOL *)&gUartDeviceNode);
  DevicePath = AppendDevicePathNode (DevicePath, (EFI_DEVICE_PATH_PROTOCOL *)&gTerminalTypeDeviceNode);

  BdsLibUpdateConsoleVariable (VarConsoleOut, DevicePath, NULL);
  BdsLibUpdateConsoleVariable (VarConsoleInp, DevicePath, NULL);
  BdsLibUpdateConsoleVariable (VarErrorOut, DevicePath, NULL);

  return EFI_SUCCESS;
}

EFI_STATUS
VisitAllInstancesOfProtocol (
  IN EFI_GUID                    *Id,
  IN PROTOCOL_INSTANCE_CALLBACK  CallBackFunction,
  IN VOID                        *Context
  )
{
  EFI_STATUS                Status;
  UINTN                     HandleCount;
  EFI_HANDLE                *HandleBuffer;
  UINTN                     Index;
  VOID                      *Instance;

  //
  // Start to check all the PciIo to find all possible device
  //
  HandleCount = 0;
  HandleBuffer = NULL;
  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  Id,
                  NULL,
                  &HandleCount,
                  &HandleBuffer
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    Status = gBS->HandleProtocol (HandleBuffer[Index], Id, &Instance);
    if (EFI_ERROR (Status)) {
      continue;
    }

    Status = (*CallBackFunction) (
               HandleBuffer[Index],
               Instance,
               Context
               );
  }

  gBS->FreePool (HandleBuffer);

  return EFI_SUCCESS;
}


EFI_STATUS
EFIAPI
VisitingAPciInstance (
  IN EFI_HANDLE  Handle,
  IN VOID        *Instance,
  IN VOID        *Context
  )
{
  EFI_STATUS                Status;
  EFI_PCI_IO_PROTOCOL       *PciIo;
  PCI_TYPE00                Pci;

  PciIo = (EFI_PCI_IO_PROTOCOL*) Instance;

  //
  // Check for all PCI device
  //
  Status = PciIo->Pci.Read (
                    PciIo,
                    EfiPciIoWidthUint32,
                    0,
                    sizeof (Pci) / sizeof (UINT32),
                    &Pci
                    );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return (*(VISIT_PCI_INSTANCE_CALLBACK)(UINTN) Context) (
           Handle,
           PciIo,
           &Pci
           );

}



EFI_STATUS
VisitAllPciInstances (
  IN VISIT_PCI_INSTANCE_CALLBACK CallBackFunction
  )
{
  return VisitAllInstancesOfProtocol (
           &gEfiPciIoProtocolGuid,
           VisitingAPciInstance,
           (VOID*)(UINTN) CallBackFunction
           );
}


/**
  Do platform specific PCI Device check and add them to
  ConOut, ConIn, ErrOut.

  @param[in]  Handle - Handle of PCI device instance
  @param[in]  PciIo - PCI IO protocol instance
  @param[in]  Pci - PCI Header register block

  @retval EFI_SUCCESS - PCI Device check and Console variable update successfully.
  @retval EFI_STATUS - PCI Device check or Console variable update fail.

**/
EFI_STATUS
EFIAPI
DetectAndPreparePlatformPciDevicePath (
  IN EFI_HANDLE           Handle,
  IN EFI_PCI_IO_PROTOCOL  *PciIo,
  IN PCI_TYPE00           *Pci
  )
{
  EFI_STATUS                Status;

  Status = PciIo->Attributes (
    PciIo,
    EfiPciIoAttributeOperationEnable,
    EFI_PCI_DEVICE_ENABLE,
    NULL
    );
  ASSERT_EFI_ERROR (Status);

  if (!mDetectVgaOnly) {
    //
    // Here we decide whether it is LPC Bridge
    //
    if ((IS_PCI_LPC (Pci)) ||
        ((IS_PCI_ISA_PDECODE (Pci)) &&
         (Pci->Hdr.VendorId == 0x8086) &&
         (Pci->Hdr.DeviceId == 0x7000)
        )
       ) {
      //
      // Add IsaKeyboard to ConIn,
      // add IsaSerial to ConOut, ConIn, ErrOut
      //
      DEBUG ((EFI_D_INFO, "Found LPC Bridge device\n"));
      PrepareLpcBridgeDevicePath (Handle);
      return EFI_SUCCESS;
    }
    //
    // Here we decide which Serial device to enable in PCI bus
    //
    if (IS_PCI_16550SERIAL (Pci)) {
      //
      // Add them to ConOut, ConIn, ErrOut.
      //
      DEBUG ((EFI_D_INFO, "Found PCI 16550 SERIAL device\n"));
      PreparePciSerialDevicePath (Handle);
      return EFI_SUCCESS;
    }
  }

  //
  // Here we decide which VGA device to enable in PCI bus
  //
  if (IS_PCI_VGA (Pci)) {
    //
    // Add them to ConOut.
    //
    DEBUG ((EFI_D_INFO, "Found PCI VGA device\n"));
    PreparePciVgaDevicePath (Handle);
    return EFI_SUCCESS;
  }

  return Status;
}


/**
  Do platform specific PCI Device check and add them to ConOut, ConIn, ErrOut

  @param[in]  DetectVgaOnly - Only detect VGA device if it's TRUE.

  @retval EFI_SUCCESS - PCI Device check and Console variable update successfully.
  @retval EFI_STATUS - PCI Device check or Console variable update fail.

**/
EFI_STATUS
DetectAndPreparePlatformPciDevicePaths (
  BOOLEAN DetectVgaOnly
  )
{
  mDetectVgaOnly = DetectVgaOnly;
  return VisitAllPciInstances (DetectAndPreparePlatformPciDevicePath);
}


EFI_STATUS
PlatformBdsConnectConsole (
  IN BDS_CONSOLE_CONNECT_ENTRY   *PlatformConsole
  )
/*++

Routine Description:

  Connect the predefined platform default console device. Always try to find
  and enable the vga device if have.

Arguments:

  PlatformConsole         - Predfined platform default console device array.

Returns:

  EFI_SUCCESS             - Success connect at least one ConIn and ConOut
                            device, there must have one ConOut device is
                            active vga device.

  EFI_STATUS              - Return the status of
                            BdsLibConnectAllDefaultConsoles ()

--*/
{
  EFI_STATUS                         Status;
  UINTN                              Index;
  EFI_DEVICE_PATH_PROTOCOL           *VarConout;
  EFI_DEVICE_PATH_PROTOCOL           *VarConin;
  UINTN                              DevicePathSize;

  //
  // Connect RootBridge
  //
  VarConout = BdsLibGetVariableAndSize (
                VarConsoleOut,
                &gEfiGlobalVariableGuid,
                &DevicePathSize
                );
  VarConin = BdsLibGetVariableAndSize (
               VarConsoleInp,
               &gEfiGlobalVariableGuid,
               &DevicePathSize
               );

  if (VarConout == NULL || VarConin == NULL) {
    //
    // Do platform specific PCI Device check and add them to ConOut, ConIn, ErrOut
    //
    DetectAndPreparePlatformPciDevicePaths (FALSE);

    //
    // Have chance to connect the platform default console,
    // the platform default console is the minimue device group
    // the platform should support
    //
    for (Index = 0; PlatformConsole[Index].DevicePath != NULL; ++Index) {
      //
      // Update the console variable with the connect type
      //
      if ((PlatformConsole[Index].ConnectType & CONSOLE_IN) == CONSOLE_IN) {
        BdsLibUpdateConsoleVariable (VarConsoleInp, PlatformConsole[Index].DevicePath, NULL);
      }
      if ((PlatformConsole[Index].ConnectType & CONSOLE_OUT) == CONSOLE_OUT) {
        BdsLibUpdateConsoleVariable (VarConsoleOut, PlatformConsole[Index].DevicePath, NULL);
      }
      if ((PlatformConsole[Index].ConnectType & STD_ERROR) == STD_ERROR) {
        BdsLibUpdateConsoleVariable (VarErrorOut, PlatformConsole[Index].DevicePath, NULL);
      }
    }
  } else {
    //
    // Only detect VGA device and add them to ConOut
    //
    DetectAndPreparePlatformPciDevicePaths (TRUE);
  }

  //
  // Connect the all the default console with current cosole variable
  //
  Status = BdsLibConnectAllDefaultConsoles ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  return EFI_SUCCESS;
}


VOID
PciInitialization (
  )
{
  //
  // Bus 0, Device 0, Function 0 - Host to PCI Bridge
  //
  PciWrite8 (PCI_LIB_ADDRESS (0, 0, 0, 0x3c), 0x00);

  //
  // Bus 0, Device 1, Function 0 - PCI to ISA Bridge
  //
  PciWrite8 (PCI_LIB_ADDRESS (0, 1, 0, 0x3c), 0x00);
  PciWrite8 (PCI_LIB_ADDRESS (0, 1, 0, 0x60), 0x0b);
  PciWrite8 (PCI_LIB_ADDRESS (0, 1, 0, 0x61), 0x09);
  PciWrite8 (PCI_LIB_ADDRESS (0, 1, 0, 0x62), 0x0b);
  PciWrite8 (PCI_LIB_ADDRESS (0, 1, 0, 0x63), 0x09);

  //
  // Bus 0, Device 1, Function 1 - IDE Controller
  //
  PciWrite8 (PCI_LIB_ADDRESS (0, 1, 1, 0x3c), 0x00);
  PciWrite8 (PCI_LIB_ADDRESS (0, 1, 1, 0x0d), 0x40);

  //
  // Bus 0, Device 1, Function 3 - Power Managment Controller
  //
  PciWrite8 (PCI_LIB_ADDRESS (0, 1, 3, 0x3c), 0x0b);
  PciWrite8 (PCI_LIB_ADDRESS (0, 1, 3, 0x3d), 0x01);

  //
  // Bus 0, Device 2, Function 0 - Video Controller
  //
  PciWrite8 (PCI_LIB_ADDRESS (0, 2, 0, 0x3c), 0x00);

  //
  // Bus 0, Device 3, Function 0 - Network Controller
  //
  PciWrite8 (PCI_LIB_ADDRESS (0, 3, 0, 0x3c), 0x0b);
  PciWrite8 (PCI_LIB_ADDRESS (0, 3, 0, 0x3d), 0x01);

  //
  // Bus 0, Device 4, Function 0 - RAM Memory
  //
  PciWrite8 (PCI_LIB_ADDRESS (0, 4, 0, 0x3c), 0x09);
  PciWrite8 (PCI_LIB_ADDRESS (0, 4, 0, 0x3d), 0x01);
}


EFI_STATUS
EFIAPI
ConnectRecursivelyIfPciMassStorage (
  IN EFI_HANDLE           Handle,
  IN EFI_PCI_IO_PROTOCOL  *Instance,
  IN PCI_TYPE00           *PciHeader
  )
{
  EFI_STATUS                Status;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  CHAR16                    *DevPathStr;

  if (IS_CLASS1 (PciHeader, PCI_CLASS_MASS_STORAGE)) {
    DevicePath = NULL;
    Status = gBS->HandleProtocol (
                    Handle,
                    &gEfiDevicePathProtocolGuid,
                    (VOID*)&DevicePath
                    );
    if (EFI_ERROR (Status)) {
      return Status;
    }

    //
    // Print Device Path
    //
    DevPathStr = DevicePathToStr (DevicePath);
    DEBUG((
      EFI_D_INFO,
      "Found Mass Storage device: %s\n",
      DevPathStr
      ));
    FreePool(DevPathStr);

    Status = gBS->ConnectController (Handle, NULL, NULL, TRUE);
    if (EFI_ERROR (Status)) {
      return Status;
    }

  }

  return EFI_SUCCESS;
}


/**
  This notification function is invoked when the
  EMU Variable FVB has been changed.

  @param  Event                 The event that occured
  @param  Context               For EFI compatiblity.  Not used.

**/
VOID
EFIAPI
EmuVariablesUpdatedCallback (
  IN  EFI_EVENT Event,
  IN  VOID      *Context
  )
{
  DEBUG ((EFI_D_INFO, "EmuVariablesUpdatedCallback\n"));
  UpdateNvVarsOnFileSystem ();
}


EFI_STATUS
EFIAPI
VisitingFileSystemInstance (
  IN EFI_HANDLE  Handle,
  IN VOID        *Instance,
  IN VOID        *Context
  )
{
  EFI_STATUS      Status;
  STATIC BOOLEAN  ConnectedToFileSystem = FALSE;

  if (ConnectedToFileSystem) {
    return EFI_ALREADY_STARTED;
  }

  Status = ConnectNvVarsToFileSystem (Handle);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  ConnectedToFileSystem = TRUE;
  mEmuVariableEvent =
    EfiCreateProtocolNotifyEvent (
      &gEfiDevicePathProtocolGuid,
      TPL_CALLBACK,
      EmuVariablesUpdatedCallback,
      NULL,
      &mEmuVariableEventReg
      );
  PcdSet64 (PcdEmuVariableEvent, (UINT64)(UINTN) mEmuVariableEvent);


  return EFI_SUCCESS;
}


VOID
PlatformBdsRestoreNvVarsFromHardDisk ()
{
  UINTN HandleCount;
  EFI_HANDLE *HandleBuffer;
  EFI_STATUS Status;
  HandleCount = 0;
  HandleBuffer = NULL;
  VBoxLogFlowFuncEnter();

  VisitAllPciInstances (ConnectRecursivelyIfPciMassStorage);
#ifndef VBOX
  VisitAllInstancesOfProtocol (
    &gEfiSimpleFileSystemProtocolGuid,
    VisitingFileSystemInstance,
    NULL
    );
#endif

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiSimpleFileSystemProtocolGuid,
                  NULL,
                  &HandleCount,
                  &HandleBuffer
                  );
  VBoxLogFlowFuncMarkRC(Status);

  VBoxLogFlowFuncLeave();
}


VOID
PlatformBdsConnectSequence (
  VOID
  )
/*++

Routine Description:

  Connect with predeined platform connect sequence,
  the OEM/IBV can customize with their own connect sequence.

Arguments:

  None.

Returns:

  None.

--*/
{
  UINTN Index;

  DEBUG ((EFI_D_INFO, "PlatformBdsConnectSequence\n"));

  Index = 0;

  //
  // Here we can get the customized platform connect sequence
  // Notes: we can connect with new variable which record the
  // last time boots connect device path sequence
  //
  while (gPlatformConnectSequence[Index] != NULL) {
    //
    // Build the platform boot option
    //
    BdsLibConnectDevicePath (gPlatformConnectSequence[Index]);
    Index++;
  }

  //
  // Just use the simple policy to connect all devices
  //
  BdsLibConnectAll ();

  PciInitialization ();

  //
  // Clear the logo after all devices are connected.
  //
  gST->ConOut->ClearScreen (gST->ConOut);
}

VOID
PlatformBdsGetDriverOption (
  IN OUT LIST_ENTRY              *BdsDriverLists
  )
/*++

Routine Description:

  Load the predefined driver option, OEM/IBV can customize this
  to load their own drivers

Arguments:

  BdsDriverLists  - The header of the driver option link list.

Returns:

  None.

--*/
{
  DEBUG ((EFI_D_INFO, "PlatformBdsGetDriverOption\n"));
  return;
}

VOID
PlatformBdsDiagnostics (
  IN EXTENDMEM_COVERAGE_LEVEL    MemoryTestLevel,
  IN BOOLEAN                     QuietBoot,
  IN BASEM_MEMORY_TEST           BaseMemoryTest
  )
/*++

Routine Description:

  Perform the platform diagnostic, such like test memory. OEM/IBV also
  can customize this fuction to support specific platform diagnostic.

Arguments:

  MemoryTestLevel  - The memory test intensive level

  QuietBoot        - Indicate if need to enable the quiet boot

  BaseMemoryTest   - A pointer to BaseMemoryTest()

Returns:

  None.

--*/
{
  EFI_STATUS  Status;

  DEBUG ((EFI_D_INFO, "PlatformBdsDiagnostics\n"));

  //
  // Here we can decide if we need to show
  // the diagnostics screen
  // Notes: this quiet boot code should be remove
  // from the graphic lib
  //
  if (QuietBoot) {
    EnableQuietBoot (PcdGetPtr(PcdLogoFile));
    //
    // Perform system diagnostic
    //
    Status = BaseMemoryTest (MemoryTestLevel);
    if (EFI_ERROR (Status)) {
      DisableQuietBoot ();
    }

    return ;
  }
  //
  // Perform system diagnostic
  //
  Status = BaseMemoryTest (MemoryTestLevel);
}


VOID
EFIAPI
PlatformBdsPolicyBehavior (
  IN OUT LIST_ENTRY                  *DriverOptionList,
  IN OUT LIST_ENTRY                  *BootOptionList,
  IN PROCESS_CAPSULES                ProcessCapsules,
  IN BASEM_MEMORY_TEST               BaseMemoryTest
  )
/*++

Routine Description:

  The function will excute with as the platform policy, current policy
  is driven by boot mode. IBV/OEM can customize this code for their specific
  policy action.

Arguments:

  DriverOptionList - The header of the driver option link list

  BootOptionList   - The header of the boot option link list

  ProcessCapsules  - A pointer to ProcessCapsules()

  BaseMemoryTest   - A pointer to BaseMemoryTest()

Returns:

  None.

--*/
{
  EFI_STATUS                         Status;
  UINT16                             Timeout;
  EFI_EVENT                          UserInputDurationTime;
  LIST_ENTRY                     *Link;
  BDS_COMMON_OPTION                  *BootOption;
  UINTN                              Index;
  EFI_INPUT_KEY                      Key;
  EFI_TPL                            OldTpl;
  EFI_BOOT_MODE                      BootMode;

  VBoxLogFlowFuncEnter();

  ConnectRootBridge ();

  //
  // Try to restore variables from the hard disk early so
  // they can be used for the other BDS connect operations.
  //
  PlatformBdsRestoreNvVarsFromHardDisk ();

  //
  // Init the time out value
  //
  Timeout = PcdGet16 (PcdPlatformBootTimeOut);

  //
  // Load the driver option as the driver option list
  //
  PlatformBdsGetDriverOption (DriverOptionList);

  //
  // Get current Boot Mode
  //
  Status = BdsLibGetBootMode (&BootMode);
  DEBUG ((EFI_D_ERROR, "Boot Mode:%x\n", BootMode));

  //
  // Go the different platform policy with different boot mode
  // Notes: this part code can be change with the table policy
  //
  ASSERT (BootMode == BOOT_WITH_FULL_CONFIGURATION);
  //
  // Connect platform console
  //
  Status = PlatformBdsConnectConsole (gPlatformConsole);
  if (EFI_ERROR (Status)) {
    //
    // Here OEM/IBV can customize with defined action
    //
    PlatformBdsNoConsoleAction ();
  }
  //
  // Create a 300ms duration event to ensure user has enough input time to enter Setup
  //
  Status = gBS->CreateEvent (
                  EVT_TIMER,
                  0,
                  NULL,
                  NULL,
                  &UserInputDurationTime
                  );
  ASSERT (Status == EFI_SUCCESS);
  Status = gBS->SetTimer (UserInputDurationTime, TimerRelative, 3000000);
  ASSERT (Status == EFI_SUCCESS);
  //
  // Memory test and Logo show
  //
  PlatformBdsDiagnostics (IGNORE, TRUE, BaseMemoryTest);

  //
  // Perform some platform specific connect sequence
  //
  PlatformBdsConnectSequence ();

  //
  // Give one chance to enter the setup if we
  // have the time out
  //
  if (Timeout != 0) {
    //PlatformBdsEnterFrontPage (Timeout, FALSE);
  }

  DEBUG ((EFI_D_INFO, "BdsLibConnectAll\n"));
  BdsLibConnectAll ();
#ifdef VBOX
    {
        UINTN cFileSystem = 0;
        EFI_HANDLE *phFileSystem = NULL;
        BDS_COMMON_OPTION *BootOption0080 = NULL;
        EFI_STATUS rc = EFI_SUCCESS;
        DEBUG ((EFI_D_INFO, "------------------ VBox Platform Specific Initialization Start -----------------------\n"));
        BootOption0080 = BdsLibVariableToOption(BootOptionList, L"Boot0080");
        if (!BootOption0080)
        {
            rc = gBS->LocateHandleBuffer (ByProtocol,
                                          &gEfiSimpleFileSystemProtocolGuid,
                                          NULL,
                                          &cFileSystem,
                                          &phFileSystem);
            VBoxLogFlowFuncMarkRC(rc);
            VBoxLogFlowFuncMarkVar(cFileSystem, "%d");
            if (   rc == EFI_SUCCESS
                && cFileSystem > 0)
            {
                EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *pFSVolume;
                EFI_FILE_HANDLE                  hFSRoot;
                EFI_FILE_HANDLE                  hBootEfiFile;
                UINTN iFileSystem = 0;
                /* Ok, we've found several simple file system handles
                 * 1. we should find if '\\System\\Library\\CoreServices\\boot.efi' present
                 * 2. Alter 'BootOrder' to include this file in boot sequence.
                 */
                for (iFileSystem = 0; iFileSystem < cFileSystem; ++iFileSystem)
                {
                    EFI_DEVICE_PATH_PROTOCOL *pDevicePath = NULL;
                    /* mount and look up the boot.efi */
                    rc = gBS->HandleProtocol (phFileSystem[iFileSystem],
                                              &gEfiSimpleFileSystemProtocolGuid,
                                              (VOID *) &pFSVolume);
                    VBoxLogFlowFuncMarkVar(iFileSystem, "%d");
                    VBoxLogFlowFuncMarkRC(rc);
                    if (EFI_ERROR(rc))
                        continue;

                    rc = pFSVolume->OpenVolume(pFSVolume, &hFSRoot);
                    VBoxLogFlowFuncMarkRC(rc);
                    if (EFI_ERROR(rc))
                        continue;

                    rc = hFSRoot->Open(hFSRoot, &hBootEfiFile, L"\\System\\Library\\CoreServices\\boot.efi", EFI_FILE_MODE_READ, 0);
                    VBoxLogFlowFuncMarkRC(rc);
                    if (EFI_ERROR(rc))
                        continue;
                    /* nice file is found and we have to register it */
                    pDevicePath = FileDevicePath(phFileSystem[iFileSystem], L"\\System\\Library\\CoreServices\\boot.efi");
                    VBoxLogFlowFuncMarkVar(pDevicePath,"%p");
                    if (!pDevicePath)
                        continue;
                    rc = BdsLibRegisterNewOption (BootOptionList, pDevicePath, L"Mac Boot", L"BootOrder");
                    VBoxLogFlowFuncMarkRC(rc);
                }
            }
        }
        else
        {
            VBoxLogFlowFuncMarkVar(BootOption0080->LoadOptionsSize, "%d");
            if (BootOption0080->LoadOptionsSize)
                VBoxLogFlowFuncMarkVar(BootOption0080->LoadOptions, "%s");
#if 0
            /* Boot0080 option is found */
            UINT16                    *BootOrder;
            UINTN                     BootOrderSize;
            UINTN                     Index = 0;
            CHAR16                    *BootOptionName;
            ASSERT(BootOption0080->Signature == BDS_LOAD_OPTION_SIGNATURE);
            BootOrder = BdsLibGetVariableAndSize (
                          L"BootOrder",
                          &gEfiGlobalVariableGuid,
                          &BootOrderSize);
            ASSERT(BootOrder);
            BootOptionName = AllocateRuntimePool(256 * sizeof(UINT16));
            UnicodeSPrint(BootOptionName, 256 * sizeof(UINT16), L"Boot%04x", BootOrder[Index]);
            BootOption0080->OptionName = BootOptionName;
            rc = gRT->SetVariable(BootOptionName,
                                  &gEfiGlobalVariableGuid,
                                  EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
                                  sizeof(BDS_COMMON_OPTION),
                                  BootOption0080);
            LogFlowFuncMarkRC(rc);
#if 0
            rc = BdsLibRegisterNewOption (BootOptionList, BootOption0080->DevicePath, L"Mac Boot Temp", L"BootOrder");
#endif
            LogFlowFuncMarkRC(rc);
#endif
        }
        DEBUG ((EFI_D_INFO, "------------------ VBox Platform Specific Initialization End -----------------------\n"));
    }
#endif
  BdsLibEnumerateAllBootOption (BootOptionList);


  //
  // Please uncomment above ConnectAll and EnumerateAll code and remove following first boot
  // checking code in real production tip.
  //
  // In BOOT_WITH_FULL_CONFIGURATION boot mode, should always connect every device
  // and do enumerate all the default boot options. But in development system board, the boot mode
  // cannot be BOOT_ASSUMING_NO_CONFIGURATION_CHANGES because the machine box
  // is always open. So the following code only do the ConnectAll and EnumerateAll at first boot.
  //
  Status = BdsLibBuildOptionFromVar (BootOptionList, L"BootOrder");
  VBoxLogFlowFuncMarkRC(Status);
  if (EFI_ERROR(Status)) {
    //
    // If cannot find "BootOrder" variable,  it may be first boot.
    // Try to connect all devices and enumerate all boot options here.
    //
    BdsLibConnectAll ();
    BdsLibEnumerateAllBootOption (BootOptionList);
  }

  //
  // To give the User a chance to enter Setup here, if user set TimeOut is 0.
  // BDS should still give user a chance to enter Setup
  //
  // Connect first boot option, and then check user input before exit
  //
  for (Link = BootOptionList->ForwardLink; Link != BootOptionList;Link = Link->ForwardLink) {
    BootOption = CR (Link, BDS_COMMON_OPTION, Link, BDS_LOAD_OPTION_SIGNATURE);
    if (!IS_LOAD_OPTION_TYPE (BootOption->Attribute, LOAD_OPTION_ACTIVE)) {
      //
      // skip the header of the link list, becuase it has no boot option
      //
      continue;
    } else {
      //
      // Make sure the boot option device path connected, but ignore the BBS device path
      //
      if (DevicePathType (BootOption->DevicePath) != BBS_DEVICE_PATH) {
        BdsLibConnectDevicePath (BootOption->DevicePath);
      }
      break;
    }
  }
#ifdef VBOX
  VBoxConsoleInit();
#endif

  //
  // Check whether the user input after the duration time has expired
  //
  OldTpl = EfiGetCurrentTpl();
  gBS->RestoreTPL (TPL_APPLICATION);
  gBS->WaitForEvent (1, &UserInputDurationTime, &Index);
  gBS->CloseEvent (UserInputDurationTime);
  Status = gST->ConIn->ReadKeyStroke (gST->ConIn, &Key);
  gBS->RaiseTPL (OldTpl);

  if (!EFI_ERROR (Status)) {
    //
    // Enter Setup if user input
    //
    Timeout = 0xffff;
    PlatformBdsEnterFrontPage (Timeout, FALSE);
  }

  VBoxLogFlowFuncLeave();
  return ;
}

VOID
EFIAPI
PlatformBdsBootSuccess (
  IN  BDS_COMMON_OPTION   *Option
  )
/*++

Routine Description:

  Hook point after a boot attempt succeeds. We don't expect a boot option to
  return, so the EFI 1.0 specification defines that you will default to an
  interactive mode and stop processing the BootOrder list in this case. This
  is alos a platform implementation and can be customized by IBV/OEM.

Arguments:

  Option - Pointer to Boot Option that succeeded to boot.

Returns:

  None.

--*/
{
  CHAR16  *TmpStr;

  DEBUG ((EFI_D_INFO, "PlatformBdsBootSuccess\n"));
  //
  // If Boot returned with EFI_SUCCESS and there is not in the boot device
  // select loop then we need to pop up a UI and wait for user input.
  //
  TmpStr = Option->StatusString;
  if (TmpStr != NULL) {
    BdsLibOutputStrings (gST->ConOut, TmpStr, Option->Description, L"\n\r", NULL);
    FreePool (TmpStr);
  }
}

VOID
EFIAPI
PlatformBdsBootFail (
  IN  BDS_COMMON_OPTION  *Option,
  IN  EFI_STATUS         Status,
  IN  CHAR16             *ExitData,
  IN  UINTN              ExitDataSize
  )
/*++

Routine Description:

  Hook point after a boot attempt fails.

Arguments:

  Option - Pointer to Boot Option that failed to boot.

  Status - Status returned from failed boot.

  ExitData - Exit data returned from failed boot.

  ExitDataSize - Exit data size returned from failed boot.

Returns:

  None.

--*/
{
  CHAR16  *TmpStr;

  DEBUG ((EFI_D_INFO, "PlatformBdsBootFail\n"));

  //
  // If Boot returned with failed status then we need to pop up a UI and wait
  // for user input.
  //
  TmpStr = Option->StatusString;
  if (TmpStr != NULL) {
    BdsLibOutputStrings (gST->ConOut, TmpStr, Option->Description, L"\n\r", NULL);
    FreePool (TmpStr);
  }
}

EFI_STATUS
PlatformBdsNoConsoleAction (
  VOID
  )
/*++

Routine Description:

  This function is remained for IBV/OEM to do some platform action,
  if there no console device can be connected.

Arguments:

  None.

Returns:

  EFI_SUCCESS      - Direct return success now.

--*/
{
  DEBUG ((EFI_D_INFO, "PlatformBdsNoConsoleAction\n"));
  return EFI_SUCCESS;
}

VOID
EFIAPI
PlatformBdsLockNonUpdatableFlash (
  VOID
  )
{
  DEBUG ((EFI_D_INFO, "PlatformBdsLockNonUpdatableFlash\n"));
  return;
}


/**
  This notification function is invoked when an instance of the
  EFI_DEVICE_PATH_PROTOCOL is produced.

  @param  Event                 The event that occured
  @param  Context               For EFI compatiblity.  Not used.

**/
VOID
EFIAPI
NotifyDevPath (
  IN  EFI_EVENT Event,
  IN  VOID      *Context
  )
{
  EFI_HANDLE                            Handle;
  EFI_STATUS                            Status;
  UINTN                                 BufferSize;
  EFI_DEVICE_PATH_PROTOCOL             *DevPathNode;
  ATAPI_DEVICE_PATH                    *Atapi;

  //
  // Examine all new handles
  //
  for (;;) {
    //
    // Get the next handle
    //
    BufferSize = sizeof (Handle);
    Status = gBS->LocateHandle (
              ByRegisterNotify,
              NULL,
              mEfiDevPathNotifyReg,
              &BufferSize,
              &Handle
              );

    //
    // If not found, we're done
    //
    if (EFI_NOT_FOUND == Status) {
      break;
    }

    if (EFI_ERROR (Status)) {
      continue;
    }

    //
    // Get the DevicePath protocol on that handle
    //
    Status = gBS->HandleProtocol (Handle, &gEfiDevicePathProtocolGuid, (VOID **)&DevPathNode);
    ASSERT_EFI_ERROR (Status);

    while (!IsDevicePathEnd (DevPathNode)) {
      //
      // Find the handler to dump this device path node
      //
      if (
           (DevicePathType(DevPathNode) == MESSAGING_DEVICE_PATH) &&
           (DevicePathSubType(DevPathNode) == MSG_ATAPI_DP)
         ) {
        Atapi = (ATAPI_DEVICE_PATH*) DevPathNode;
        PciOr16 (
          PCI_LIB_ADDRESS (
            0,
            1,
            1,
            (Atapi->PrimarySecondary == 1) ? 0x42: 0x40
            ),
          BIT15
          );
      }

      //
      // Next device path node
      //
      DevPathNode = NextDevicePathNode (DevPathNode);
    }
  }

  return;
}


VOID
InstallDevicePathCallback (
  VOID
  )
{
  DEBUG ((EFI_D_INFO, "Registered NotifyDevPath Event\n"));
  mEfiDevPathEvent = EfiCreateProtocolNotifyEvent (
                          &gEfiDevicePathProtocolGuid,
                          TPL_CALLBACK,
                          NotifyDevPath,
                          NULL,
                          &mEfiDevPathNotifyReg
                          );
}

/**
  Lock the ConsoleIn device in system table. All key
  presses will be ignored until the Password is typed in. The only way to
  disable the password is to type it in to a ConIn device.

  @param  Password        Password used to lock ConIn device.

  @retval EFI_SUCCESS     lock the Console In Spliter virtual handle successfully.
  @retval EFI_UNSUPPORTED Password not found

**/
EFI_STATUS
EFIAPI
LockKeyboards (
  IN  CHAR16    *Password
  )
{
    return EFI_UNSUPPORTED;
}


#ifndef VBOX
STATIC
VOID
LoadVideoRom (
  VOID
  )
{
  PCI_DATA_STRUCTURE            *Pcir;
  UINTN                         RomSize;

  //
  // The virtual machines sometimes load the video rom image
  // directly at the legacy video BIOS location of C000:0000,
  // and do not implement the PCI expansion ROM feature.
  //
  Pcir = (PCI_DATA_STRUCTURE *) (UINTN) 0xc0000;
  RomSize = Pcir->ImageLength * 512;
  PciRomLoadEfiDriversFromRomImage (0xc0000, RomSize);
}


STATIC
EFI_STATUS
PciRomLoadEfiDriversFromRomImage (
  IN EFI_PHYSICAL_ADDRESS    Rom,
  IN UINTN                   RomSize
  )
{
  CHAR16                        *FileName;
  EFI_PCI_EXPANSION_ROM_HEADER  *EfiRomHeader;
  PCI_DATA_STRUCTURE            *Pcir;
  UINTN                         ImageIndex;
  UINTN                         RomOffset;
  UINT32                        ImageSize;
  UINT16                        ImageOffset;
  EFI_HANDLE                    ImageHandle;
  EFI_STATUS                    Status;
  EFI_STATUS                    retStatus;
  EFI_DEVICE_PATH_PROTOCOL      *FilePath;
  BOOLEAN                       SkipImage;
  UINT32                        DestinationSize;
  UINT32                        ScratchSize;
  UINT8                         *Scratch;
  VOID                          *ImageBuffer;
  VOID                          *DecompressedImageBuffer;
  UINT32                        ImageLength;
  EFI_DECOMPRESS_PROTOCOL       *Decompress;
  UINT32                        InitializationSize;

  VBoxLogFlowFuncEnter();
  FileName = L"PciRomInMemory";

  //FileName = L"PciRom Addr=0000000000000000";
  //HexToString (&FileName[12], Rom, 16);

  ImageIndex    = 0;
  retStatus     = EFI_NOT_FOUND;
  RomOffset  = (UINTN) Rom;

  do {

    EfiRomHeader = (EFI_PCI_EXPANSION_ROM_HEADER *) (UINTN) RomOffset;

    if (EfiRomHeader->Signature != PCI_EXPANSION_ROM_HEADER_SIGNATURE) {
      return retStatus;
    }

    //
    // If the pointer to the PCI Data Structure is invalid, no further images can be located.
    // The PCI Data Structure must be DWORD aligned.
    //
    if (EfiRomHeader->PcirOffset == 0 ||
        (EfiRomHeader->PcirOffset & 3) != 0 ||
        RomOffset - (UINTN)Rom + EfiRomHeader->PcirOffset + sizeof (PCI_DATA_STRUCTURE) > RomSize) {
      break;
    }
    Pcir      = (PCI_DATA_STRUCTURE *) (UINTN) (RomOffset + EfiRomHeader->PcirOffset);
    //
    // If a valid signature is not present in the PCI Data Structure, no further images can be located.
    //
    if (Pcir->Signature != PCI_DATA_STRUCTURE_SIGNATURE) {
      break;
    }
    ImageSize = Pcir->ImageLength * 512;
    if (RomOffset - (UINTN)Rom + ImageSize > RomSize) {
      break;
    }

    if ((Pcir->CodeType == PCI_CODE_TYPE_EFI_IMAGE) &&
        (EfiRomHeader->EfiSignature == EFI_PCI_EXPANSION_ROM_HEADER_EFISIGNATURE) &&
        ((EfiRomHeader->EfiSubsystem == EFI_IMAGE_SUBSYSTEM_EFI_BOOT_SERVICE_DRIVER) ||
         (EfiRomHeader->EfiSubsystem == EFI_IMAGE_SUBSYSTEM_EFI_RUNTIME_DRIVER))) {

      ImageOffset             = EfiRomHeader->EfiImageHeaderOffset;
      InitializationSize      = EfiRomHeader->InitializationSize * 512;

      if (InitializationSize <= ImageSize && ImageOffset < InitializationSize) {

        ImageBuffer             = (VOID *) (UINTN) (RomOffset + ImageOffset);
        ImageLength             = InitializationSize - ImageOffset;
        DecompressedImageBuffer = NULL;

        //
        // decompress here if needed
        //
        SkipImage = FALSE;
        if (EfiRomHeader->CompressionType > EFI_PCI_EXPANSION_ROM_HEADER_COMPRESSED) {
          SkipImage = TRUE;
        }

        if (EfiRomHeader->CompressionType == EFI_PCI_EXPANSION_ROM_HEADER_COMPRESSED) {
          Status = gBS->LocateProtocol (&gEfiDecompressProtocolGuid, NULL, (VOID **) &Decompress);
          if (EFI_ERROR (Status)) {
            SkipImage = TRUE;
          } else {
            SkipImage = TRUE;
            Status = Decompress->GetInfo (
                                  Decompress,
                                  ImageBuffer,
                                  ImageLength,
                                  &DestinationSize,
                                  &ScratchSize
                                  );
            if (!EFI_ERROR (Status)) {
              DecompressedImageBuffer = NULL;
              DecompressedImageBuffer = AllocatePool (DestinationSize);
              if (DecompressedImageBuffer != NULL) {
                Scratch = AllocatePool (ScratchSize);
                if (Scratch != NULL) {
                  Status = Decompress->Decompress (
                                        Decompress,
                                        ImageBuffer,
                                        ImageLength,
                                        DecompressedImageBuffer,
                                        DestinationSize,
                                        Scratch,
                                        ScratchSize
                                        );
                  if (!EFI_ERROR (Status)) {
                    ImageBuffer = DecompressedImageBuffer;
                    ImageLength = DestinationSize;
                    SkipImage   = FALSE;
                  }

                  gBS->FreePool (Scratch);
                }
              }
            }
          }
        }

        if (!SkipImage) {

          //
          // load image and start image
          //

          FilePath = FileDevicePath (NULL, FileName);

          Status = gBS->LoadImage (
                          FALSE,
                          gImageHandle,
                          FilePath,
                          ImageBuffer,
                          ImageLength,
                          &ImageHandle
                          );
          if (!EFI_ERROR (Status)) {
            Status = gBS->StartImage (ImageHandle, NULL, NULL);
            if (!EFI_ERROR (Status)) {
              retStatus = Status;
            }
          }
          if (FilePath != NULL) {
            gBS->FreePool (FilePath);
          }
        }

        if (DecompressedImageBuffer != NULL) {
          gBS->FreePool (DecompressedImageBuffer);
        }

      }
    }

    RomOffset = RomOffset + ImageSize;
    ImageIndex++;
  } while (((Pcir->Indicator & 0x80) == 0x00) && ((RomOffset - (UINTN) Rom) < RomSize));


  VBoxLogFlowFuncLeaveRC(retStatus);
  return retStatus;
}
#endif /* !VBOX */


