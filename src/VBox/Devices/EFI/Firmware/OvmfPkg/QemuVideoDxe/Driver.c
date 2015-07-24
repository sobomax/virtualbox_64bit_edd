/** @file
  This driver is a sample implementation of the Graphics Output Protocol for
  the QEMU (Cirrus Logic 5446) video controller.

  Copyright (c) 2006 - 2010, Intel Corporation. All rights reserved.<BR>

  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution. The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#include "Qemu.h"

EFI_DRIVER_BINDING_PROTOCOL gQemuVideoDriverBinding = {
  QemuVideoControllerDriverSupported,
  QemuVideoControllerDriverStart,
  QemuVideoControllerDriverStop,
  0x10,
  NULL,
  NULL
};

/**
  Check if this device is supported.

  @param  This                   The driver binding protocol.
  @param  Controller             The controller handle to check.
  @param  RemainingDevicePath    The remaining device path.

  @retval EFI_SUCCESS            The bus supports this controller.
  @retval EFI_UNSUPPORTED        This device isn't supported.

**/
EFI_STATUS
EFIAPI
QemuVideoControllerDriverSupported (
  IN EFI_DRIVER_BINDING_PROTOCOL    *This,
  IN EFI_HANDLE                     Controller,
  IN EFI_DEVICE_PATH_PROTOCOL       *RemainingDevicePath
  )
{
  EFI_STATUS          Status;
  EFI_PCI_IO_PROTOCOL *PciIo;
  PCI_TYPE00          Pci;
  EFI_DEV_PATH        *Node;

  //
  // Open the PCI I/O Protocol
  //
  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiPciIoProtocolGuid,
                  (VOID **) &PciIo,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Read the PCI Configuration Header from the PCI Device
  //
  Status = PciIo->Pci.Read (
                        PciIo,
                        EfiPciIoWidthUint32,
                        0,
                        sizeof (Pci) / sizeof (UINT32),
                        &Pci
                        );
  if (EFI_ERROR (Status)) {
    goto Done;
  }

  Status = EFI_UNSUPPORTED;
  //
  // See if the I/O enable is on.  Most systems only allow one VGA device to be turned on
  // at a time, so see if this is one that is turned on.
  //
  //  if (((Pci.Hdr.Command & 0x01) == 0x01)) {
  //
  // See if this is a Cirrus Logic PCI controller
  //
  if (Pci.Hdr.VendorId == CIRRUS_LOGIC_VENDOR_ID) {
    //
    // See if this is a 5430 or a 5446 PCI controller
    //
    if (Pci.Hdr.DeviceId == CIRRUS_LOGIC_5430_DEVICE_ID || 
        Pci.Hdr.DeviceId == CIRRUS_LOGIC_5430_ALTERNATE_DEVICE_ID ||
        Pci.Hdr.DeviceId == CIRRUS_LOGIC_5446_DEVICE_ID) {
        
      Status = EFI_SUCCESS;
      //
      // If this is an Intel 945 graphics controller,
      // go further check RemainingDevicePath validation
      //
      if (RemainingDevicePath != NULL) {
        Node = (EFI_DEV_PATH *) RemainingDevicePath;
        //
        // Check if RemainingDevicePath is the End of Device Path Node, 
        // if yes, return EFI_SUCCESS
        //
        if (!IsDevicePathEnd (Node)) {
          //
          // If RemainingDevicePath isn't the End of Device Path Node,
          // check its validation
          //
          if (Node->DevPath.Type != ACPI_DEVICE_PATH ||
              Node->DevPath.SubType != ACPI_ADR_DP ||
              DevicePathNodeLength(&Node->DevPath) != sizeof(ACPI_ADR_DEVICE_PATH)) {
            Status = EFI_UNSUPPORTED;
          }
        }
      }
    }
  }

Done:
  //
  // Close the PCI I/O Protocol
  //
  gBS->CloseProtocol (
        Controller,
        &gEfiPciIoProtocolGuid,
        This->DriverBindingHandle,
        Controller
        );

  return Status;
}

/**
  Start to process the controller.

  @param  This                   The USB bus driver binding instance.
  @param  Controller             The controller to check.
  @param  RemainingDevicePath    The remaining device patch.

  @retval EFI_SUCCESS            The controller is controlled by the usb bus.
  @retval EFI_ALREADY_STARTED    The controller is already controlled by the usb
                                 bus.
  @retval EFI_OUT_OF_RESOURCES   Failed to allocate resources.

**/
EFI_STATUS
EFIAPI
QemuVideoControllerDriverStart (
  IN EFI_DRIVER_BINDING_PROTOCOL    *This,
  IN EFI_HANDLE                     Controller,
  IN EFI_DEVICE_PATH_PROTOCOL       *RemainingDevicePath
  )
{
  EFI_STATUS                      Status;
  QEMU_VIDEO_PRIVATE_DATA  *Private;
  BOOLEAN                         PciAttributesSaved;
  EFI_DEVICE_PATH_PROTOCOL        *ParentDevicePath;
  ACPI_ADR_DEVICE_PATH            AcpiDeviceNode;

  PciAttributesSaved = FALSE;
  //
  // Allocate Private context data for GOP inteface.
  //
  Private = AllocateZeroPool (sizeof (QEMU_VIDEO_PRIVATE_DATA));
  if (Private == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto Error;
  }

  //
  // Set up context record
  //
  Private->Signature  = QEMU_VIDEO_PRIVATE_DATA_SIGNATURE;
  Private->Handle     = NULL;

  //
  // Open PCI I/O Protocol
  //
  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiPciIoProtocolGuid,
                  (VOID **) &Private->PciIo,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    goto Error;
  }

  //
  // Save original PCI attributes
  //
  Status = Private->PciIo->Attributes (
                    Private->PciIo,
                    EfiPciIoAttributeOperationGet,
                    0,
                    &Private->OriginalPciAttributes
                    );

  if (EFI_ERROR (Status)) {
    goto Error;
  }
  PciAttributesSaved = TRUE;

  Status = Private->PciIo->Attributes (
                            Private->PciIo,
                            EfiPciIoAttributeOperationEnable,
                            EFI_PCI_DEVICE_ENABLE | EFI_PCI_IO_ATTRIBUTE_VGA_MEMORY | EFI_PCI_IO_ATTRIBUTE_VGA_IO,
                            NULL
                            );
  if (EFI_ERROR (Status)) {
    goto Error;
  }

  //
  // Get ParentDevicePath
  //
  Status = gBS->HandleProtocol (
                  Controller,
                  &gEfiDevicePathProtocolGuid,
                  (VOID **) &ParentDevicePath
                  );
  if (EFI_ERROR (Status)) {
    goto Error;
  }

  //
  // Set Gop Device Path
  //
  if (RemainingDevicePath == NULL) {
    ZeroMem (&AcpiDeviceNode, sizeof (ACPI_ADR_DEVICE_PATH));
    AcpiDeviceNode.Header.Type = ACPI_DEVICE_PATH;
    AcpiDeviceNode.Header.SubType = ACPI_ADR_DP;
    AcpiDeviceNode.ADR = ACPI_DISPLAY_ADR (1, 0, 0, 1, 0, ACPI_ADR_DISPLAY_TYPE_VGA, 0, 0);
    SetDevicePathNodeLength (&AcpiDeviceNode.Header, sizeof (ACPI_ADR_DEVICE_PATH));

    Private->GopDevicePath = AppendDevicePathNode (
                                        ParentDevicePath,
                                        (EFI_DEVICE_PATH_PROTOCOL *) &AcpiDeviceNode
                                        );
  } else if (!IsDevicePathEnd (RemainingDevicePath)) {
    //
    // If RemainingDevicePath isn't the End of Device Path Node, 
    // only scan the specified device by RemainingDevicePath
    //
    Private->GopDevicePath = AppendDevicePathNode (ParentDevicePath, RemainingDevicePath);
  } else {
    //
    // If RemainingDevicePath is the End of Device Path Node, 
    // don't create child device and return EFI_SUCCESS
    //
    Private->GopDevicePath = NULL;
  }
    
  if (Private->GopDevicePath != NULL) {
    //
    // Creat child handle and device path protocol firstly
    //
    Private->Handle = NULL;
    Status = gBS->InstallMultipleProtocolInterfaces (
                    &Private->Handle,
                    &gEfiDevicePathProtocolGuid,
                    Private->GopDevicePath,
                    NULL
                    );
  }

  //
  // Construct video mode buffer
  //
  Status = QemuVideoVideoModeSetup (Private);
  if (EFI_ERROR (Status)) {
    goto Error;
  }

  if (Private->GopDevicePath == NULL) {
    //
    // If RemainingDevicePath is the End of Device Path Node, 
    // don't create child device and return EFI_SUCCESS
    //
    Status = EFI_SUCCESS;
  } else {

    //
    // Start the GOP software stack.
    //
    Status = QemuVideoGraphicsOutputConstructor (Private);
    ASSERT_EFI_ERROR (Status);

    Status = gBS->InstallMultipleProtocolInterfaces (
                    &Private->Handle,
                    &gEfiGraphicsOutputProtocolGuid,
                    &Private->GraphicsOutput,
                    NULL
                    );
  }

Error:
  if (EFI_ERROR (Status)) {
    if (Private) {
      if (Private->PciIo) {
        if (PciAttributesSaved == TRUE) {
          //
          // Restore original PCI attributes
          //
          Private->PciIo->Attributes (
                          Private->PciIo,
                          EfiPciIoAttributeOperationSet,
                          Private->OriginalPciAttributes,
                          NULL
                          );
        }
        //
        // Close the PCI I/O Protocol
        //
        gBS->CloseProtocol (
              Private->Handle,
              &gEfiPciIoProtocolGuid,
              This->DriverBindingHandle,
              Private->Handle
              );
      }

      gBS->FreePool (Private);
    }
  }

  return Status;
}

/**
  Stop this device

  @param  This                   The USB bus driver binding protocol.
  @param  Controller             The controller to release.
  @param  NumberOfChildren       The number of children of this device that
                                 opened the controller BY_CHILD.
  @param  ChildHandleBuffer      The array of child handle.

  @retval EFI_SUCCESS            The controller or children are stopped.
  @retval EFI_DEVICE_ERROR       Failed to stop the driver.

**/
EFI_STATUS
EFIAPI
QemuVideoControllerDriverStop (
  IN EFI_DRIVER_BINDING_PROTOCOL    *This,
  IN EFI_HANDLE                     Controller,
  IN UINTN                          NumberOfChildren,
  IN EFI_HANDLE                     *ChildHandleBuffer
  )
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL    *GraphicsOutput;

  EFI_STATUS                      Status;
  QEMU_VIDEO_PRIVATE_DATA  *Private;

  Status = gBS->OpenProtocol (
                  Controller,
                  &gEfiGraphicsOutputProtocolGuid,
                  (VOID **) &GraphicsOutput,
                  This->DriverBindingHandle,
                  Controller,
                  EFI_OPEN_PROTOCOL_GET_PROTOCOL
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Get our private context information
  //
  Private = QEMU_VIDEO_PRIVATE_DATA_FROM_GRAPHICS_OUTPUT_THIS (GraphicsOutput);

  QemuVideoGraphicsOutputDestructor (Private);
  //
  // Remove the GOP protocol interface from the system
  //
  Status = gBS->UninstallMultipleProtocolInterfaces (
                  Private->Handle,
                  &gEfiGraphicsOutputProtocolGuid,
                  &Private->GraphicsOutput,
                  NULL
                  );

  if (EFI_ERROR (Status)) {
    return Status;
  }

  //
  // Restore original PCI attributes
  //
  Private->PciIo->Attributes (
                  Private->PciIo,
                  EfiPciIoAttributeOperationSet,
                  Private->OriginalPciAttributes,
                  NULL
                  );

  //
  // Close the PCI I/O Protocol
  //
  gBS->CloseProtocol (
        Controller,
        &gEfiPciIoProtocolGuid,
        This->DriverBindingHandle,
        Controller
        );

  //
  // Free our instance data
  //
  gBS->FreePool (Private);

  return EFI_SUCCESS;
}

/**
  TODO: Add function description

  @param  Private TODO: add argument description
  @param  Address TODO: add argument description
  @param  Data TODO: add argument description

  TODO: add return values

**/
VOID
outb (
  QEMU_VIDEO_PRIVATE_DATA  *Private,
  UINTN                           Address,
  UINT8                           Data
  )
{
  Private->PciIo->Io.Write (
                      Private->PciIo,
                      EfiPciIoWidthUint8,
                      EFI_PCI_IO_PASS_THROUGH_BAR,
                      Address,
                      1,
                      &Data
                      );
}

/**
  TODO: Add function description

  @param  Private TODO: add argument description
  @param  Address TODO: add argument description
  @param  Data TODO: add argument description

  TODO: add return values

**/
VOID
outw (
  QEMU_VIDEO_PRIVATE_DATA  *Private,
  UINTN                           Address,
  UINT16                          Data
  )
{
  Private->PciIo->Io.Write (
                      Private->PciIo,
                      EfiPciIoWidthUint16,
                      EFI_PCI_IO_PASS_THROUGH_BAR,
                      Address,
                      1,
                      &Data
                      );
}

/**
  TODO: Add function description

  @param  Private TODO: add argument description
  @param  Address TODO: add argument description

  TODO: add return values

**/
UINT8
inb (
  QEMU_VIDEO_PRIVATE_DATA  *Private,
  UINTN                           Address
  )
{
  UINT8 Data;

  Private->PciIo->Io.Read (
                      Private->PciIo,
                      EfiPciIoWidthUint8,
                      EFI_PCI_IO_PASS_THROUGH_BAR,
                      Address,
                      1,
                      &Data
                      );
  return Data;
}

/**
  TODO: Add function description

  @param  Private TODO: add argument description
  @param  Address TODO: add argument description

  TODO: add return values

**/
UINT16
inw (
  QEMU_VIDEO_PRIVATE_DATA  *Private,
  UINTN                           Address
  )
{
  UINT16  Data;

  Private->PciIo->Io.Read (
                      Private->PciIo,
                      EfiPciIoWidthUint16,
                      EFI_PCI_IO_PASS_THROUGH_BAR,
                      Address,
                      1,
                      &Data
                      );
  return Data;
}

/**
  TODO: Add function description

  @param  Private TODO: add argument description
  @param  Index TODO: add argument description
  @param  Red TODO: add argument description
  @param  Green TODO: add argument description
  @param  Blue TODO: add argument description

  TODO: add return values

**/
VOID
SetPaletteColor (
  QEMU_VIDEO_PRIVATE_DATA  *Private,
  UINTN                           Index,
  UINT8                           Red,
  UINT8                           Green,
  UINT8                           Blue
  )
{
  outb (Private, PALETTE_INDEX_REGISTER, (UINT8) Index);
  outb (Private, PALETTE_DATA_REGISTER, (UINT8) (Red >> 2));
  outb (Private, PALETTE_DATA_REGISTER, (UINT8) (Green >> 2));
  outb (Private, PALETTE_DATA_REGISTER, (UINT8) (Blue >> 2));
}

/**
  TODO: Add function description

  @param  Private TODO: add argument description

  TODO: add return values

**/
VOID
SetDefaultPalette (
  QEMU_VIDEO_PRIVATE_DATA  *Private
  )
{
  UINTN Index;
  UINTN RedIndex;
  UINTN GreenIndex;
  UINTN BlueIndex;

  Index = 0;
  for (RedIndex = 0; RedIndex < 8; RedIndex++) {
    for (GreenIndex = 0; GreenIndex < 8; GreenIndex++) {
      for (BlueIndex = 0; BlueIndex < 4; BlueIndex++) {
        SetPaletteColor (Private, Index, (UINT8) (RedIndex << 5), (UINT8) (GreenIndex << 5), (UINT8) (BlueIndex << 6));
        Index++;
      }
    }
  }
}

/**
  TODO: Add function description

  @param  Private TODO: add argument description

  TODO: add return values

**/
VOID
ClearScreen (
  QEMU_VIDEO_PRIVATE_DATA  *Private
  )
{
  UINT32  Color;

  Color = 0;
  Private->PciIo->Mem.Write (
                        Private->PciIo,
                        EfiPciIoWidthFillUint32,
                        0,
                        0,
                        0x400000 >> 2,
                        &Color
                        );
}

/**
  TODO: Add function description

  @param  Private TODO: add argument description

  TODO: add return values

**/
VOID
DrawLogo (
  QEMU_VIDEO_PRIVATE_DATA  *Private,
  UINTN                           ScreenWidth,
  UINTN                           ScreenHeight
  )
{
}

/**
  TODO: Add function description

  @param  Private TODO: add argument description
  @param  ModeData TODO: add argument description

  TODO: add return values

**/
VOID
InitializeGraphicsMode (
  QEMU_VIDEO_PRIVATE_DATA  *Private,
  QEMU_VIDEO_VIDEO_MODES   *ModeData
  )
{
  UINT8 Byte;
  UINTN Index;
  UINT16 DeviceId;
  EFI_STATUS Status;

  Status = Private->PciIo->Pci.Read (
             Private->PciIo,
             EfiPciIoWidthUint16,
             PCI_DEVICE_ID_OFFSET,
             1,
             &DeviceId
             );
  //
  // Read the PCI Configuration Header from the PCI Device
  //
  ASSERT_EFI_ERROR (Status);

  outw (Private, SEQ_ADDRESS_REGISTER, 0x1206);
  outw (Private, SEQ_ADDRESS_REGISTER, 0x0012);

  for (Index = 0; Index < 15; Index++) {
    outw (Private, SEQ_ADDRESS_REGISTER, ModeData->SeqSettings[Index]);
  }

  if (DeviceId != CIRRUS_LOGIC_5446_DEVICE_ID) {
    outb (Private, SEQ_ADDRESS_REGISTER, 0x0f);
    Byte = (UINT8) ((inb (Private, SEQ_DATA_REGISTER) & 0xc7) ^ 0x30);
    outb (Private, SEQ_DATA_REGISTER, Byte);
  }

  outb (Private, MISC_OUTPUT_REGISTER, ModeData->MiscSetting);
  outw (Private, GRAPH_ADDRESS_REGISTER, 0x0506);
  outw (Private, SEQ_ADDRESS_REGISTER, 0x0300);
  outw (Private, CRTC_ADDRESS_REGISTER, 0x2011);

  for (Index = 0; Index < 28; Index++) {
    outw (Private, CRTC_ADDRESS_REGISTER, (UINT16) ((ModeData->CrtcSettings[Index] << 8) | Index));
  }

  for (Index = 0; Index < 9; Index++) {
    outw (Private, GRAPH_ADDRESS_REGISTER, (UINT16) ((GraphicsController[Index] << 8) | Index));
  }

  inb (Private, INPUT_STATUS_1_REGISTER);

  for (Index = 0; Index < 21; Index++) {
    outb (Private, ATT_ADDRESS_REGISTER, (UINT8) Index);
    outb (Private, ATT_ADDRESS_REGISTER, AttributeController[Index]);
  }

  outb (Private, ATT_ADDRESS_REGISTER, 0x20);

  outw (Private, GRAPH_ADDRESS_REGISTER, 0x0009);
  outw (Private, GRAPH_ADDRESS_REGISTER, 0x000a);
  outw (Private, GRAPH_ADDRESS_REGISTER, 0x000b);
  outb (Private, DAC_PIXEL_MASK_REGISTER, 0xff);

  SetDefaultPalette (Private);
  ClearScreen (Private);
}

EFI_STATUS
EFIAPI
InitializeQemuVideo (
  IN EFI_HANDLE           ImageHandle,
  IN EFI_SYSTEM_TABLE     *SystemTable
  )
{
  EFI_STATUS              Status;

  Status = EfiLibInstallDriverBindingComponentName2 (
             ImageHandle,
             SystemTable,
             &gQemuVideoDriverBinding,
             ImageHandle,
             &gQemuVideoComponentName,
             &gQemuVideoComponentName2
             );
  ASSERT_EFI_ERROR (Status);

  //
  // Install EFI Driver Supported EFI Version Protocol required for
  // EFI drivers that are on PCI and other plug in cards.
  //
  gQemuVideoDriverSupportedEfiVersion.FirmwareVersion = PcdGet32 (PcdDriverSupportedEfiVersion);
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &ImageHandle,
                  &gEfiDriverSupportedEfiVersionProtocolGuid,
                  &gQemuVideoDriverSupportedEfiVersion,
                  NULL
                  );
  ASSERT_EFI_ERROR (Status);

  return Status;
}
