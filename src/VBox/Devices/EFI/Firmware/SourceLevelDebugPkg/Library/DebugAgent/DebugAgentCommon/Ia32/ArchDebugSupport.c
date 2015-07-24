/** @file
  Public include file for Debug Port Library.

  Copyright (c) 2010, Intel Corporation. All rights reserved.<BR>
  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php.

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#include "DebugAgent.h"

/**
  Read the offset of FP / MMX / XMM registers by register index.

  @param[in]  Index    Register index.
  @param[out] Width    Register width returned.

  @return Offset in register address range.

**/
UINT16
ArchReadFxStatOffset (
  IN  UINT8                     Index,
  OUT UINT8                     *Width
  )
{
  if (Index < SOFT_DEBUGGER_REGISTER_ST0) {
    switch (Index) {
    case SOFT_DEBUGGER_REGISTER_FP_FCW:
      *Width = (UINT8) sizeof (UINT16);
      return (UINT16)OFFSET_OF(DEBUG_DATA_IA32_FX_SAVE_STATE, Fcw);

    case SOFT_DEBUGGER_REGISTER_FP_FSW:
  	  *Width = (UINT8) sizeof (UINT16);
      return (UINT16)OFFSET_OF(DEBUG_DATA_IA32_FX_SAVE_STATE, Fsw);

    case SOFT_DEBUGGER_REGISTER_FP_FTW:
      *Width = (UINT8) sizeof (UINT16);
      return (UINT16)OFFSET_OF(DEBUG_DATA_IA32_FX_SAVE_STATE, Ftw);

    case SOFT_DEBUGGER_REGISTER_FP_OPCODE:
      *Width = (UINT8) sizeof (UINT16);
      return (UINT16)OFFSET_OF(DEBUG_DATA_IA32_FX_SAVE_STATE, Opcode);

    case SOFT_DEBUGGER_REGISTER_FP_EIP:
      *Width = (UINT8) sizeof (UINTN);
      return (UINT16)OFFSET_OF(DEBUG_DATA_IA32_FX_SAVE_STATE, Eip);

    case SOFT_DEBUGGER_REGISTER_FP_CS:
      *Width = (UINT8) sizeof (UINT16);
      return (UINT16)OFFSET_OF(DEBUG_DATA_IA32_FX_SAVE_STATE, Cs);

    case SOFT_DEBUGGER_REGISTER_FP_DATAOFFSET:
      *Width = (UINT8) sizeof (UINTN);
      return (UINT16)OFFSET_OF(DEBUG_DATA_IA32_FX_SAVE_STATE, DataOffset);

    case SOFT_DEBUGGER_REGISTER_FP_DS:
      *Width = (UINT8) sizeof (UINT16);
      return (UINT16)OFFSET_OF(DEBUG_DATA_IA32_FX_SAVE_STATE, Ds);

    case SOFT_DEBUGGER_REGISTER_FP_MXCSR:
      *Width = (UINT8) sizeof (UINTN);
      return (UINT16)OFFSET_OF(DEBUG_DATA_IA32_FX_SAVE_STATE, Mxcsr);

    case SOFT_DEBUGGER_REGISTER_FP_MXCSR_MASK:
      *Width = (UINT8) sizeof (UINTN);
      return (UINT16)OFFSET_OF(DEBUG_DATA_IA32_FX_SAVE_STATE, Mxcsr_Mask);
    }
  }

  if (Index < SOFT_DEBUGGER_REGISTER_XMM0) {
    *Width = 10;
  } else if (Index < SOFT_DEBUGGER_REGISTER_MM0 ) {
    *Width = 16;
  } else {
    *Width = 8;
    Index -= SOFT_DEBUGGER_REGISTER_MM0 - SOFT_DEBUGGER_REGISTER_ST0;
  }

  return (UINT16)(OFFSET_OF(DEBUG_DATA_IA32_FX_SAVE_STATE, St0Mm0) + (Index - SOFT_DEBUGGER_REGISTER_ST0) * 16);
}

/**
  Write specified register into save CPU context.

  @param[in] CpuContext         Pointer to saved CPU context.
  @param[in] Index              Register index value.
  @param[in] Offset             Offset in register address range.
  @param[in] Width              Data width to read.
  @param[in] RegisterBuffer     Pointer to input buffer with data.

**/
VOID
ArchWriteRegisterBuffer (
  IN DEBUG_CPU_CONTEXT               *CpuContext,
  IN UINT8                           Index,
  IN UINT8                           Offset,
  IN UINT8                           Width,
  IN UINT8                           *RegisterBuffer
  )
{
  UINT8           *Buffer;
  if (Index < SOFT_DEBUGGER_REGISTER_FP_BASE) {
    Buffer = (UINT8 *) CpuContext + sizeof (DEBUG_DATA_IA32_FX_SAVE_STATE) + Index * 4;
  } else {
    //
    // If it is MMX register, adjust its index position
    //
    if (Index >= SOFT_DEBUGGER_REGISTER_MM0) {
      Index -= SOFT_DEBUGGER_REGISTER_MM0 - SOFT_DEBUGGER_REGISTER_ST0;
    }
    //
    // FPU/MMX/XMM registers
    //
    Buffer = (UINT8 *) CpuContext + ArchReadFxStatOffset (Index, &Width);
  }

  CopyMem (Buffer + Offset, RegisterBuffer, Width);
}

/**
  Read register value from saved CPU context.

  @param[in] CpuContext         Pointer to saved CPU context.
  @param[in] Index              Register index value.
  @param[in] Offset             Offset in register address range
  @param[in] Width              Data width to read.

  @return The address of register value.

**/
UINT8 *
ArchReadRegisterBuffer (
  IN DEBUG_CPU_CONTEXT               *CpuContext,
  IN UINT8                           Index,
  IN UINT8                           Offset,
  IN UINT8                           *Width
  )
{
  UINT8           *Buffer;

  if (Index < SOFT_DEBUGGER_REGISTER_FP_BASE) {
    Buffer = (UINT8 *) CpuContext + sizeof (DEBUG_DATA_IA32_FX_SAVE_STATE) + Index * 4;
    if (*Width == 0) {
      *Width = (UINT8) sizeof (UINTN);
    }
  } else {
    //
    // FPU/MMX/XMM registers
    //
    Buffer = (UINT8 *) CpuContext + ArchReadFxStatOffset (Index, Width);
  }

  return Buffer;
}

/**
  Read group register of common registers.

  @param[in] CpuContext           Pointer to saved CPU context.
  @param[in] RegisterGroup        Pointer to Group registers.

**/
VOID
ReadRegisterGroup (
  IN DEBUG_CPU_CONTEXT                       *CpuContext,
  IN DEBUG_DATA_REPONSE_READ_REGISTER_GROUP  *RegisterGroup
  )
{
  RegisterGroup->Cs     = (UINT16) CpuContext->Cs;
  RegisterGroup->Ds     = (UINT16) CpuContext->Ds;
  RegisterGroup->Es     = (UINT16) CpuContext->Es;
  RegisterGroup->Fs     = (UINT16) CpuContext->Fs;
  RegisterGroup->Gs     = (UINT16) CpuContext->Gs;
  RegisterGroup->Ss     = (UINT16) CpuContext->Ss;
  RegisterGroup->Eflags = CpuContext->Eflags;
  RegisterGroup->Ebp    = CpuContext->Ebp;
  RegisterGroup->Eip    = CpuContext->Eip;
  RegisterGroup->Esp    = CpuContext->Esp;
  RegisterGroup->Eax    = CpuContext->Eax;
  RegisterGroup->Ebx    = CpuContext->Ebx;
  RegisterGroup->Ecx    = CpuContext->Ecx;
  RegisterGroup->Edx    = CpuContext->Edx;
  RegisterGroup->Esi    = CpuContext->Esi;
  RegisterGroup->Edi    = CpuContext->Edi;
  RegisterGroup->Dr0    = CpuContext->Dr0;
  RegisterGroup->Dr1    = CpuContext->Dr1;
  RegisterGroup->Dr2    = CpuContext->Dr2;
  RegisterGroup->Dr3    = CpuContext->Dr3;
  RegisterGroup->Dr6    = CpuContext->Dr6;
  RegisterGroup->Dr7    = CpuContext->Dr7;
}

/**
  Initialize IDT entries to support source level debug.

**/
VOID
InitializeDebugIdt (
  VOID
  )
{
  IA32_IDT_GATE_DESCRIPTOR   *IdtEntry;
  UINTN                      InterruptHandler;
  IA32_DESCRIPTOR            IdtDescriptor;
  UINTN                      Index;
  UINT16                     CodeSegment;

  AsmReadIdtr (&IdtDescriptor);

  //
  // Use current CS as the segment selector of interrupt gate in IDT
  //
  CodeSegment = AsmReadCs ();

  IdtEntry = (IA32_IDT_GATE_DESCRIPTOR *) IdtDescriptor.Base;

  for (Index = 0; Index < 20; Index ++) {
    if ((PcdGet32 (PcdExceptionsIgnoredByDebugger) & (1 << Index)) != 0) {
      //
      // If the exception is masked to be reserved, skip it
      //
      continue;
    }
    InterruptHandler = (UINTN)&Exception0Handle + Index * ExceptionStubHeaderSize;
    IdtEntry[Index].Bits.OffsetLow  = (UINT16)(UINTN)InterruptHandler;
    IdtEntry[Index].Bits.OffsetHigh = (UINT16)((UINTN)InterruptHandler >> 16);
    IdtEntry[Index].Bits.Selector        = CodeSegment;
    IdtEntry[Index].Bits.GateType        = IA32_IDT_GATE_TYPE_INTERRUPT_32;
  }

  InterruptHandler = (UINTN) &TimerInterruptHandle;
  IdtEntry[DEBUG_TIMER_VECTOR].Bits.OffsetLow  = (UINT16)(UINTN)InterruptHandler;
  IdtEntry[DEBUG_TIMER_VECTOR].Bits.OffsetHigh = (UINT16)((UINTN)InterruptHandler >> 16);
  IdtEntry[Index].Bits.Selector        = CodeSegment;
  IdtEntry[Index].Bits.GateType        = IA32_IDT_GATE_TYPE_INTERRUPT_32;
}
