/** @file
  This is the code for Boot Script Executer module.

  This driver is dispatched by Dxe core and the driver will reload itself to ACPI NVS memory
  in the entry point. The functionality is to interpret and restore the S3 boot script

Copyright (c) 2006 - 2011, Intel Corporation. All rights reserved.<BR>

This program and the accompanying materials
are licensed and made available under the terms and conditions of the BSD License
which accompanies this distribution.  The full text of the license may be found at
http://opensource.org/licenses/bsd-license.php

THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#include "ScriptExecute.h"

EFI_PHYSICAL_ADDRESS  mPerfDataMemAddress;
UINT64                mS3BootScriptEntryTick;
UINT64                mScriptStartTick;
UINT64                mScriptEndTick;

EFI_GUID              mBootScriptExecutorImageGuid = {
  0x9a8d3433, 0x9fe8, 0x42b6, 0x87, 0xb, 0x1e, 0x31, 0xc8, 0x4e, 0xbe, 0x3b
};

/**
  The event callback is used to get the base address of boot performance data structure on
  LegacyBoot event and ExitBootServices event.

  @param  Event    The event handle.
  @param  Context  The event context.

**/
VOID
EFIAPI
OnBootEvent (
  IN EFI_EVENT        Event,
  IN VOID             *Context
  )
{
  EFI_STATUS Status;
  UINTN      VarSize;

  VarSize = sizeof (EFI_PHYSICAL_ADDRESS);
  Status = gRT->GetVariable (
                  L"PerfDataMemAddr",
                  &gPerformanceProtocolGuid,
                  NULL,
                  &VarSize,
                  &mPerfDataMemAddress
                  );
  if (EFI_ERROR (Status)) {
    mPerfDataMemAddress = 0;
  }
}

/**
  Record S3 Script execution time and adjust total S3 resume time for script running.
**/
VOID
WriteToOsS3PerformanceData (
  VOID
  )
{
  UINT64               Ticker;
  UINT64               StartValue;
  UINT64               EndValue;
  UINT64               Freq;
  UINT64               ScriptExecuteTicks;
  PERF_HEADER          *PerfHeader;
  PERF_DATA            *PerfData;

  Ticker = GetPerformanceCounter ();

  PerfHeader = (PERF_HEADER *)(UINTN)mPerfDataMemAddress;
  if (PerfHeader == NULL) {
    return;
  }

  Freq   = GetPerformanceCounterProperties (&StartValue, &EndValue);
  Freq   = DivU64x32 (Freq, 1000);

  if (EndValue >= StartValue) {
    ScriptExecuteTicks = mScriptEndTick - mScriptStartTick;
    PerfHeader->S3Resume += Ticker - mS3BootScriptEntryTick;
  } else {
    ScriptExecuteTicks = mScriptStartTick - mScriptEndTick;
    PerfHeader->S3Resume += mS3BootScriptEntryTick - Ticker;
  }
  if (PerfHeader->S3EntryNum < PERF_PEI_ENTRY_MAX_NUM) {
    PerfData = &PerfHeader->S3Entry[PerfHeader->S3EntryNum];
    PerfData->Duration = (UINT32) DivU64x32 (ScriptExecuteTicks, (UINT32) Freq);;
    AsciiStrnCpy (PerfData->Token, "ScriptExec", PERF_TOKEN_LENGTH);
    PerfHeader->S3EntryNum++;
  }
}

/**
  Entry function of Boot script exector. This function will be executed in
  S3 boot path.
  This function should not return, because it is invoked by switch stack.

  @param  AcpiS3Context    a pointer to a structure of ACPI_S3_CONTEXT
  @param  PeiS3ResumeState a pointer to a structure of PEI_S3_RESUME_STATE

  @retval EFI_INVALID_PARAMETER - OS waking vector not found
  @retval EFI_UNSUPPORTED - something wrong when we resume to OS
**/
EFI_STATUS
EFIAPI
S3BootScriptExecutorEntryFunction (
  IN ACPI_S3_CONTEXT       *AcpiS3Context,
  IN PEI_S3_RESUME_STATE   *PeiS3ResumeState
  )
{
  EFI_ACPI_4_0_FIRMWARE_ACPI_CONTROL_STRUCTURE  *Facs;
  EFI_STATUS                                    Status;
  UINTN                                         TempStackTop;
  UINTN                                         TempStack[0x10];
  UINTN                                         AsmTransferControl16Address;

  PERF_CODE (
    mS3BootScriptEntryTick = GetPerformanceCounter ();
    );

  //
  // Disable interrupt of Debug timer, since new IDT table cannot handle it.
  //
  SaveAndSetDebugTimerInterrupt (FALSE);

  //
  // Restore IDT for debug
  //
  SetIdtEntry (AcpiS3Context);

  //
  // Initialize Debug Agent to support source level debug in S3 path.
  //
  InitializeDebugAgent (DEBUG_AGENT_INIT_S3, NULL, NULL);

  //
  // Because not install BootScriptExecute PPI(used just in this module), So just pass NULL
  // for that parameter.
  //
  PERF_CODE (
    mScriptStartTick = GetPerformanceCounter ();
    );
  Status = S3BootScriptExecute ();
  PERF_CODE (
    mScriptEndTick = GetPerformanceCounter ();
    );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  AsmWbinvd ();

  //
  // Get ACPI Table Address
  //
  Facs = (EFI_ACPI_4_0_FIRMWARE_ACPI_CONTROL_STRUCTURE *) ((UINTN) (AcpiS3Context->AcpiFacsTable));

  if ((Facs == NULL) ||
      (Facs->Signature != EFI_ACPI_4_0_FIRMWARE_ACPI_CONTROL_STRUCTURE_SIGNATURE) ||
      ((Facs->FirmwareWakingVector == 0) && (Facs->XFirmwareWakingVector == 0)) ) {
    CpuDeadLoop();
    return EFI_INVALID_PARAMETER;
  }

  //
  // We need turn back to S3Resume - install boot script done ppi and report status code on S3resume.
  //
  if (PeiS3ResumeState != 0) {
    if (FeaturePcdGet (PcdDxeIplSwitchToLongMode)) {
      //
      // X64 S3 Resume
      //
      DEBUG ((EFI_D_ERROR, "Call AsmDisablePaging64() to return to S3 Resume in PEI Phase\n"));
      PeiS3ResumeState->AsmTransferControl = (EFI_PHYSICAL_ADDRESS)(UINTN)AsmTransferControl32;

      //
      // more step needed - because relative address is handled differently between X64 and IA32.
      //
      AsmTransferControl16Address = (UINTN)AsmTransferControl16;
      AsmFixAddress16 = (UINT32)AsmTransferControl16Address;
      AsmJmpAddr32 = (UINT32)((Facs->FirmwareWakingVector & 0xF) | ((Facs->FirmwareWakingVector & 0xFFFF0) << 12));

      AsmDisablePaging64 (
        PeiS3ResumeState->ReturnCs,
        (UINT32)PeiS3ResumeState->ReturnEntryPoint,
        (UINT32)(UINTN)AcpiS3Context,
        (UINT32)(UINTN)PeiS3ResumeState,
        (UINT32)PeiS3ResumeState->ReturnStackPointer
        );
    } else {
      //
      // IA32 S3 Resume
      //
      DEBUG ((EFI_D_ERROR, "Call SwitchStack() to return to S3 Resume in PEI Phase\n"));
      PeiS3ResumeState->AsmTransferControl = (EFI_PHYSICAL_ADDRESS)(UINTN)AsmTransferControl;

      SwitchStack (
        (SWITCH_STACK_ENTRY_POINT)(UINTN)PeiS3ResumeState->ReturnEntryPoint,
        (VOID *)(UINTN)AcpiS3Context,
        (VOID *)(UINTN)PeiS3ResumeState,
        (VOID *)(UINTN)PeiS3ResumeState->ReturnStackPointer
        );
    }

    //
    // Never run to here
    //
    CpuDeadLoop();
    return EFI_UNSUPPORTED;
  }

  PERF_CODE (
    WriteToOsS3PerformanceData ();
    );

  if (Facs->XFirmwareWakingVector != 0) {
    //
    // Switch to native waking vector
    //
    TempStackTop = (UINTN)&TempStack + sizeof(TempStack);
    if ((Facs->Version == EFI_ACPI_4_0_FIRMWARE_ACPI_CONTROL_STRUCTURE_VERSION) &&
        ((Facs->Flags & EFI_ACPI_4_0_64BIT_WAKE_SUPPORTED_F) != 0) &&
        ((Facs->Flags & EFI_ACPI_4_0_OSPM_64BIT_WAKE__F) != 0)) {
      //
      // X64 long mode waking vector
      //
      DEBUG (( EFI_D_ERROR, "Transfer to 64bit OS waking vector - %x\r\n", (UINTN)Facs->XFirmwareWakingVector));
      if (FeaturePcdGet (PcdDxeIplSwitchToLongMode)) {
        SwitchStack (
          (SWITCH_STACK_ENTRY_POINT)(UINTN)Facs->XFirmwareWakingVector,
          NULL,
          NULL,
          (VOID *)(UINTN)TempStackTop
          );
      } else {
        // Unsupported for 32bit DXE, 64bit OS vector
        DEBUG (( EFI_D_ERROR, "Unsupported for 32bit DXE transfer to 64bit OS waking vector!\r\n"));
        ASSERT (FALSE);
      }
    } else {
      //
      // IA32 protected mode waking vector (Page disabled)
      //
      DEBUG (( EFI_D_ERROR, "Transfer to 32bit OS waking vector - %x\r\n", (UINTN)Facs->XFirmwareWakingVector));
      if (FeaturePcdGet (PcdDxeIplSwitchToLongMode)) {
        AsmDisablePaging64 (
          0x10,
          (UINT32)Facs->XFirmwareWakingVector,
          0,
          0,
          (UINT32)TempStackTop
          );
      } else {
        SwitchStack (
          (SWITCH_STACK_ENTRY_POINT)(UINTN)Facs->XFirmwareWakingVector,
          NULL,
          NULL,
          (VOID *)(UINTN)TempStackTop
          );
      }
    }
  } else {
    //
    // 16bit Realmode waking vector
    //
    DEBUG (( EFI_D_ERROR, "Transfer to 16bit OS waking vector - %x\r\n", (UINTN)Facs->FirmwareWakingVector));
    AsmTransferControl (Facs->FirmwareWakingVector, 0x0);
  }

  //
  // Never run to here
  //
  CpuDeadLoop();
  return EFI_UNSUPPORTED;
}
/**
  Entrypoint of Boot script exector driver, this function will be executed in
  normal boot phase and invoked by DXE dispatch.

  @param[in] ImageHandle    The firmware allocated handle for the EFI image.
  @param[in] SystemTable    A pointer to the EFI System Table.

  @retval EFI_SUCCESS       The entry point is executed successfully.
  @retval other             Some error occurs when executing this entry point.
**/
EFI_STATUS
EFIAPI
BootScriptExecutorEntryPoint (
  IN EFI_HANDLE           ImageHandle,
  IN EFI_SYSTEM_TABLE     *SystemTable
  )
{
  UINT8                                         *Buffer;
  UINTN                                         BufferSize;
  UINTN                                         Pages;
  EFI_PHYSICAL_ADDRESS                          FfsBuffer;
  PE_COFF_LOADER_IMAGE_CONTEXT                  ImageContext;
  BOOT_SCRIPT_EXECUTOR_VARIABLE                 *EfiBootScriptExecutorVariable;
  EFI_PHYSICAL_ADDRESS                          BootScriptExecutorBuffer;
  EFI_STATUS                                    Status;
  VOID                                          *DevicePath;
  EFI_HANDLE                                    NewImageHandle;

  //
  // Test if the gEfiCallerIdGuid of this image is already installed. if not, the entry
  // point is loaded by DXE code which is the first time loaded. or else, it is already
  // be reloaded be itself.This is a work-around
  //
  Status = gBS->LocateProtocol (&gEfiCallerIdGuid, NULL, &DevicePath);
  if (EFI_ERROR (Status)) {

      //
      // This is the first-time loaded by DXE core. reload itself to NVS mem
      //
      //
      // A workarouond: Here we install a dummy handle
      //
      NewImageHandle = NULL;
      Status = gBS->InstallProtocolInterface (
                  &NewImageHandle,
                  &gEfiCallerIdGuid,
                  EFI_NATIVE_INTERFACE,
                  NULL
                  );

      Status = GetSectionFromAnyFv  (
                 &gEfiCallerIdGuid,
                 EFI_SECTION_PE32,
                 0,
                 (VOID **) &Buffer,
                 &BufferSize
                 );
      ImageContext.Handle    = Buffer;
      ImageContext.ImageRead = PeCoffLoaderImageReadFromMemory;
      //
      // Get information about the image being loaded
      //
      Status = PeCoffLoaderGetImageInfo (&ImageContext);
      if (EFI_ERROR (Status)) {
        return Status;
      }
      Pages = EFI_SIZE_TO_PAGES(BufferSize + ImageContext.SectionAlignment);
      FfsBuffer = 0xFFFFFFFF;
      Status = gBS->AllocatePages (
                    AllocateMaxAddress,
                    EfiACPIMemoryNVS,
                    Pages,
                    &FfsBuffer
                    );
      if (EFI_ERROR (Status)) {
        return EFI_OUT_OF_RESOURCES;
      }
      ImageContext.ImageAddress = (PHYSICAL_ADDRESS)(UINTN)FfsBuffer;
      //
      // Align buffer on section boundry
      //
      ImageContext.ImageAddress += ImageContext.SectionAlignment - 1;
      ImageContext.ImageAddress &= ~(ImageContext.SectionAlignment - 1);
      //
      // Load the image to our new buffer
      //
      Status = PeCoffLoaderLoadImage (&ImageContext);
      if (EFI_ERROR (Status)) {
        gBS->FreePages (FfsBuffer, Pages);
        return Status;
      }

      //
      // Relocate the image in our new buffer
      //
      Status = PeCoffLoaderRelocateImage (&ImageContext);

      if (EFI_ERROR (Status)) {
        PeCoffLoaderUnloadImage (&ImageContext);
        gBS->FreePages (FfsBuffer, Pages);
        return Status;
      }
      //
      // Flush the instruction cache so the image data is written before we execute it
      //
      InvalidateInstructionCacheRange ((VOID *)(UINTN)ImageContext.ImageAddress, (UINTN)ImageContext.ImageSize);
      Status = ((EFI_IMAGE_ENTRY_POINT)(UINTN)(ImageContext.EntryPoint)) (NewImageHandle, SystemTable);
      if (EFI_ERROR (Status)) {
        gBS->FreePages (FfsBuffer, Pages);
        return Status;
      }
      //
      // Additional step for BootScript integrity
      // Save BootScriptExecutor image
      //
      Status = SaveLockBox (
                 &mBootScriptExecutorImageGuid,
                 (VOID *)(UINTN)ImageContext.ImageAddress,
                 (UINTN)ImageContext.ImageSize
                 );
      ASSERT_EFI_ERROR (Status);

      Status = SetLockBoxAttributes (&mBootScriptExecutorImageGuid, LOCK_BOX_ATTRIBUTE_RESTORE_IN_PLACE);
      ASSERT_EFI_ERROR (Status);

    } else {
      //
      // the entry point is invoked after reloading. following code only run in  ACPI NVS
      //
      BufferSize = sizeof (BOOT_SCRIPT_EXECUTOR_VARIABLE);

      BootScriptExecutorBuffer = 0xFFFFFFFF;
      Pages = EFI_SIZE_TO_PAGES(BufferSize);
      Status = gBS->AllocatePages (
                      AllocateMaxAddress,
                      EfiACPIMemoryNVS,
                      Pages,
                      &BootScriptExecutorBuffer
                      );
      if (EFI_ERROR (Status)) {
        return EFI_OUT_OF_RESOURCES;
      }

      EfiBootScriptExecutorVariable = (BOOT_SCRIPT_EXECUTOR_VARIABLE *)(UINTN)BootScriptExecutorBuffer;
      EfiBootScriptExecutorVariable->BootScriptExecutorEntrypoint = (UINTN) S3BootScriptExecutorEntryFunction ;

      Status = SaveLockBox (
                 &gEfiBootScriptExecutorVariableGuid,
                 &BootScriptExecutorBuffer,
                 sizeof(BootScriptExecutorBuffer)
                 );
      ASSERT_EFI_ERROR (Status);

      //
      // Additional step for BootScript integrity
      // Save BootScriptExecutor context
      //
      Status = SaveLockBox (
                 &gEfiBootScriptExecutorContextGuid,
                 EfiBootScriptExecutorVariable,
                 sizeof(*EfiBootScriptExecutorVariable)
                 );
      ASSERT_EFI_ERROR (Status);

      Status = SetLockBoxAttributes (&gEfiBootScriptExecutorContextGuid, LOCK_BOX_ATTRIBUTE_RESTORE_IN_PLACE);
      ASSERT_EFI_ERROR (Status);

      PERF_CODE (
        EFI_EVENT Event;

        gBS->CreateEventEx (
              EVT_NOTIFY_SIGNAL,
              TPL_NOTIFY,
              OnBootEvent,
              NULL,
              &gEfiEventExitBootServicesGuid,
              &Event
              );

        EfiCreateEventLegacyBootEx(
          TPL_NOTIFY,
          OnBootEvent,
          NULL,
          &Event
          );
        );
    }

    return EFI_SUCCESS;
}



