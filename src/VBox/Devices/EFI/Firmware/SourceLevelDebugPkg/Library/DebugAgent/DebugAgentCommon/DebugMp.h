/** @file
  Header file for Multi-Processor support.

  Copyright (c) 2010, Intel Corporation. All rights reserved.<BR>
  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD License
  which accompanies this distribution.  The full text of the license may be found at
  http://opensource.org/licenses/bsd-license.php.

  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#ifndef _DEBUG_MP_H_
#define _DEBUG_MP_H_

#define DEBUG_CPU_MAX_COUNT             256

typedef struct {
  UINT32    CpuCount;                            ///< Processor count
  UINT16    ApicID[DEBUG_CPU_MAX_COUNT];         ///< Record the local apic id for each processor
} DEBUG_CPU_DATA;

typedef struct {
  SPIN_LOCK                 MpContextSpinLock;   ///< Lock for writting MP context
  SPIN_LOCK                 DebugPortSpinLock;   ///< Lock for access debug port
  UINT8                     CpuBreakMask[DEBUG_CPU_MAX_COUNT/8];        ///< Bitmask of all breaking CPUs
  UINT8                     CpuStopStatusMask[DEBUG_CPU_MAX_COUNT/8];   ///< Bitmask of CPU stop status
  UINT32                    ViewPointIndex;      ///< Current view point to be debugged
  UINT32                    BspIndex;            ///< Processor index value of BSP
  UINT32                    BreakAtCpuIndex;     ///< Processor index value of the current breaking CPU
  UINT32                    DebugTimerInitCount; ///< Record BSP's init timer count
  BOOLEAN                   IpiSentByAp;         ///< TRUR: IPI is sent by AP. TALSE: IPI is sent by BSP
  BOOLEAN                   RunCommandSet;       ///< TRUE: RUN commmand is not executed. FALSE : RUN command is executed.
} DEBUG_MP_CONTEXT;

extern CONST BOOLEAN               MultiProcessorDebugSupport;
extern DEBUG_MP_CONTEXT volatile   mDebugMpContext;
extern DEBUG_CPU_DATA   volatile   mDebugCpuData;

/**
  Break the other processor by send IPI.

  @param[in] CurrentProcessorIndex  Current processor index value.

**/
VOID
HaltOtherProcessors (
  IN UINT32             CurrentProcessorIndex
  );

/**
  Get the current processor's index.

  @return Processor index value.

**/
UINT32
GetProcessorIndex (
  VOID
  );

/**
  Acquire access control on MP context.

  It will block in the function if cannot get the access control.

**/
VOID
AcquireMpContextControl (
  VOID
  );

/**
  Release access control on MP context.

**/
VOID
ReleaseMpContextControl (
  VOID
  );

/**
  Acquire access control on debug port.

  It will block in the function if cannot get the access control.

**/
VOID
AcquireDebugPortControl (
  VOID
  );

/**
  Release access control on debug port.

**/
VOID
ReleaseDebugPortControl (
  VOID
  );

/**
  Check if the specified processor is BSP or not.

  @param[in] ProcessorIndex Processor index value.

  @retval TRUE    It is BSP.
  @retval FALSE   It isn't BSP.

**/
BOOLEAN
IsBsp (
  IN UINT32             ProcessorIndex
  );

/**
  Set processor stop flag bitmask in MP context.

  @param[in] ProcessorIndex Processor index value.
  @param[in] StopFlag       TRUE means set stop flag.
                            FALSE means clean break flag.

**/
VOID
SetCpuStopFlagByIndex (
  IN UINT32             ProcessorIndex,
  IN BOOLEAN            StopFlag
  );

/**
  Set processor break flag bitmask in MP context.

  @param[in] ProcessorIndex Processor index value.
  @param[in] BreakFlag      TRUE means set break flag.
                            FALSE means clean break flag.

**/
VOID
SetCpuBreakFlagByIndex (
  IN UINT32             ProcessorIndex,
  IN BOOLEAN            BreakFlag
  );

/**
  Check if processor is stopped already.

  @param[in] ProcessorIndex   Processor index value.

  @retval TRUE        Processor is stopped already.
  @retval FALSE       Processor isn't stopped.

**/
BOOLEAN
IsCpuStopped (
  IN UINT32              ProcessorIndex
  );

/**
  Set the run command flag.

  @param[in] RunningFlag   TRUE means run command flag is set.
                           FALSE means run command flag is cleared.

**/
VOID
SetCpuRunningFlag (
  IN BOOLEAN            RunningFlag
  );

/**
  Set the current view point to be debugged.

  @param[in] ProcessorIndex   Processor index value.

**/
VOID
SetDebugViewPoint (
  IN UINT32             ProcessorIndex
  );

/**
  Initialize debug timer.

  @param[in] IpiSentByApFlag   TRUE means this IPI is sent by AP.
                               FALSE means this IPI is sent by BSP.

**/
VOID
SetIpiSentByApFlag (
  IN BOOLEAN            IpiSentByApFlag
  );

/**
  Check if any processor breaks.

  @retval others      There is at least one processor broken, the minimum
                      index number of Processor returned.
  @retval -1          No any processor broken.

**/
UINT32
FindCpuNotRunning (
  VOID
  );
  
/**
  Check if all processors are in running status.

  @retval TRUE        All processors run.
  @retval FALSE       At least one processor does not run.

**/
BOOLEAN
IsAllCpuRunning (
  VOID
  );

#endif

