/* $Id: CPUMR3Db.cpp $ */
/** @file
 * CPUM - CPU database part.
 */

/*
 * Copyright (C) 2013 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#define LOG_GROUP LOG_GROUP_CPUM
#include <VBox/vmm/cpum.h>
#include "CPUMInternal.h"
#include <VBox/vmm/vm.h>

#include <VBox/err.h>
#include <iprt/asm-amd64-x86.h>
#include <iprt/mem.h>
#include <iprt/string.h>


/*******************************************************************************
*   Structures and Typedefs                                                    *
*******************************************************************************/
typedef struct CPUMDBENTRY
{
    /** The CPU name. */
    const char     *pszName;
    /** The full CPU name. */
    const char     *pszFullName;
    /** The CPU vendor (CPUMCPUVENDOR). */
    uint8_t         enmVendor;
    /** The CPU family. */
    uint8_t         uFamily;
    /** The CPU model. */
    uint8_t         uModel;
    /** The CPU stepping. */
    uint8_t         uStepping;
    /** The microarchitecture. */
    CPUMMICROARCH   enmMicroarch;
    /** Scalable bus frequency used for reporting other frequencies. */
    uint64_t        uScalableBusFreq;
    /** Flags (TBD). */
    uint32_t        fFlags;
    /** The maximum physical address with of the CPU.  This should correspond to
     * the value in CPUID leaf 0x80000008 when present. */
    uint8_t         cMaxPhysAddrWidth;
    /** Pointer to an array of CPUID leaves.  */
    PCCPUMCPUIDLEAF paCpuIdLeaves;
    /** The number of CPUID leaves in the array paCpuIdLeaves points to. */
    uint32_t        cCpuIdLeaves;
    /** The method used to deal with unknown CPUID leaves. */
    CPUMUKNOWNCPUID enmUnknownCpuId;
    /** The default unknown CPUID value. */
    CPUMCPUID       DefUnknownCpuId;

    /** MSR mask.  Several microarchitectures ignore higher bits of the    */
    uint32_t        fMsrMask;

    /** The number of ranges in the table pointed to b paMsrRanges. */
    uint32_t        cMsrRanges;
    /** MSR ranges for this CPU. */
    PCCPUMMSRRANGE  paMsrRanges;
} CPUMDBENTRY;


/*******************************************************************************
*   Defined Constants And Macros                                               *
*******************************************************************************/

/** @def NULL_ALONE
 * For eliminating an unnecessary data dependency in standalone builds (for
 * VBoxSVC). */
/** @def ZERO_ALONE
 * For eliminating an unnecessary data size dependency in standalone builds (for
 * VBoxSVC). */
#ifndef CPUM_DB_STANDALONE
# define NULL_ALONE(a_aTable)    a_aTable
# define ZERO_ALONE(a_cTable)    a_cTable
#else
# define NULL_ALONE(a_aTable)    NULL
# define ZERO_ALONE(a_cTable)    0
#endif


/** @name Short macros for the MSR range entries.
 *
 * These are rather cryptic, but this is to reduce the attack on the right
 * margin.
 *
 * @{ */
/** Alias one MSR onto another (a_uTarget). */
#define MAL(a_uMsr, a_szName, a_uTarget) \
    RINT(a_uMsr, a_uMsr, kCpumMsrRdFn_MsrAlias, kCpumMsrWrFn_MsrAlias, 0, a_uTarget, 0, 0, a_szName)
/** Functions handles everything. */
#define MFN(a_uMsr, a_szName, a_enmRdFnSuff, a_enmWrFnSuff) \
    RINT(a_uMsr, a_uMsr, kCpumMsrRdFn_##a_enmRdFnSuff, kCpumMsrWrFn_##a_enmWrFnSuff, 0, 0, 0, 0, a_szName)
/** Functions handles everything, with GP mask. */
#define MFG(a_uMsr, a_szName, a_enmRdFnSuff, a_enmWrFnSuff, a_fWrGpMask) \
    RINT(a_uMsr, a_uMsr, kCpumMsrRdFn_##a_enmRdFnSuff, kCpumMsrWrFn_##a_enmWrFnSuff, 0, 0, 0, a_fWrGpMask, a_szName)
/** Function handlers, read-only. */
#define MFO(a_uMsr, a_szName, a_enmRdFnSuff) \
    RINT(a_uMsr, a_uMsr, kCpumMsrRdFn_##a_enmRdFnSuff, kCpumMsrWrFn_ReadOnly, 0, 0, 0, UINT64_MAX, a_szName)
/** Function handlers, ignore all writes. */
#define MFI(a_uMsr, a_szName, a_enmRdFnSuff) \
    RINT(a_uMsr, a_uMsr, kCpumMsrRdFn_##a_enmRdFnSuff, kCpumMsrWrFn_IgnoreWrite, 0, 0, UINT64_MAX, 0, a_szName)
/** Function handlers, with value. */
#define MFV(a_uMsr, a_szName, a_enmRdFnSuff, a_enmWrFnSuff, a_uValue) \
    RINT(a_uMsr, a_uMsr, kCpumMsrRdFn_##a_enmRdFnSuff, kCpumMsrWrFn_##a_enmWrFnSuff, 0, a_uValue, 0, 0, a_szName)
/** Function handlers, with write ignore mask. */
#define MFW(a_uMsr, a_szName, a_enmRdFnSuff, a_enmWrFnSuff, a_fWrIgnMask) \
    RINT(a_uMsr, a_uMsr, kCpumMsrRdFn_##a_enmRdFnSuff, kCpumMsrWrFn_##a_enmWrFnSuff, 0, 0, a_fWrIgnMask, 0, a_szName)
/** Function handlers, extended version. */
#define MFX(a_uMsr, a_szName, a_enmRdFnSuff, a_enmWrFnSuff, a_uValue, a_fWrIgnMask, a_fWrGpMask) \
    RINT(a_uMsr, a_uMsr, kCpumMsrRdFn_##a_enmRdFnSuff, kCpumMsrWrFn_##a_enmWrFnSuff, 0, a_uValue, a_fWrIgnMask, a_fWrGpMask, a_szName)
/** Function handlers, with CPUMCPU storage variable. */
#define MFS(a_uMsr, a_szName, a_enmRdFnSuff, a_enmWrFnSuff, a_CpumCpuMember) \
    RINT(a_uMsr, a_uMsr, kCpumMsrRdFn_##a_enmRdFnSuff, kCpumMsrWrFn_##a_enmWrFnSuff, \
         RT_OFFSETOF(CPUMCPU, a_CpumCpuMember), 0, 0, 0, a_szName)
/** Function handlers, with CPUMCPU storage variable, ignore mask and GP mask. */
#define MFZ(a_uMsr, a_szName, a_enmRdFnSuff, a_enmWrFnSuff, a_CpumCpuMember, a_fWrIgnMask, a_fWrGpMask) \
    RINT(a_uMsr, a_uMsr, kCpumMsrRdFn_##a_enmRdFnSuff, kCpumMsrWrFn_##a_enmWrFnSuff, \
         RT_OFFSETOF(CPUMCPU, a_CpumCpuMember), 0, a_fWrIgnMask, a_fWrGpMask, a_szName)
/** Read-only fixed value. */
#define MVO(a_uMsr, a_szName, a_uValue) \
    RINT(a_uMsr, a_uMsr, kCpumMsrRdFn_FixedValue, kCpumMsrWrFn_ReadOnly, 0, a_uValue, 0, UINT64_MAX, a_szName)
/** Read-only fixed value, ignores all writes. */
#define MVI(a_uMsr, a_szName, a_uValue) \
    RINT(a_uMsr, a_uMsr, kCpumMsrRdFn_FixedValue, kCpumMsrWrFn_IgnoreWrite, 0, a_uValue, UINT64_MAX, 0, a_szName)
/** Read fixed value, ignore writes outside GP mask. */
#define MVG(a_uMsr, a_szName, a_uValue, a_fWrGpMask) \
    RINT(a_uMsr, a_uMsr, kCpumMsrRdFn_FixedValue, kCpumMsrWrFn_IgnoreWrite, 0, a_uValue, 0, a_fWrGpMask, a_szName)
/** Read fixed value, extended version with both GP and ignore masks. */
#define MVX(a_uMsr, a_szName, a_uValue, a_fWrIgnMask, a_fWrGpMask) \
    RINT(a_uMsr, a_uMsr, kCpumMsrRdFn_FixedValue, kCpumMsrWrFn_IgnoreWrite, 0, a_uValue, a_fWrIgnMask, a_fWrGpMask, a_szName)
/** The short form, no CPUM backing. */
#define MSN(a_uMsr, a_szName, a_enmRdFnSuff, a_enmWrFnSuff, a_uInitOrReadValue, a_fWrIgnMask, a_fWrGpMask) \
    RINT(a_uMsr, a_uMsr, kCpumMsrRdFn_##a_enmRdFnSuff, kCpumMsrWrFn_##a_enmWrFnSuff, 0, \
         a_uInitOrReadValue, a_fWrIgnMask, a_fWrGpMask, a_szName)

/** Range: Functions handles everything. */
#define RFN(a_uFirst, a_uLast, a_szName, a_enmRdFnSuff, a_enmWrFnSuff) \
    RINT(a_uFirst, a_uLast, kCpumMsrRdFn_##a_enmRdFnSuff, kCpumMsrWrFn_##a_enmWrFnSuff, 0, 0, 0, 0, a_szName)
/** Range: Read fixed value, read-only. */
#define RVO(a_uFirst, a_uLast, a_szName, a_uValue) \
    RINT(a_uFirst, a_uLast, kCpumMsrRdFn_FixedValue, kCpumMsrWrFn_ReadOnly, 0, a_uValue, 0, UINT64_MAX, a_szName)
/** Range: Read fixed value, ignore writes. */
#define RVI(a_uFirst, a_uLast, a_szName, a_uValue) \
    RINT(a_uFirst, a_uLast, kCpumMsrRdFn_FixedValue, kCpumMsrWrFn_IgnoreWrite, 0, a_uValue, UINT64_MAX, 0, a_szName)
/** Range: The short form, no CPUM backing. */
#define RSN(a_uFirst, a_uLast, a_szName, a_enmRdFnSuff, a_enmWrFnSuff, a_uInitOrReadValue, a_fWrIgnMask, a_fWrGpMask) \
    RINT(a_uFirst, a_uLast, kCpumMsrRdFn_##a_enmRdFnSuff, kCpumMsrWrFn_##a_enmWrFnSuff, 0, \
         a_uInitOrReadValue, a_fWrIgnMask, a_fWrGpMask, a_szName)

/** Internal form used by the macros. */
#ifdef VBOX_WITH_STATISTICS
# define RINT(a_uFirst, a_uLast, a_enmRdFn, a_enmWrFn, a_offCpumCpu, a_uInitOrReadValue, a_fWrIgnMask, a_fWrGpMask, a_szName) \
    { a_uFirst, a_uLast, a_enmRdFn, a_enmWrFn, a_offCpumCpu, 0, a_uInitOrReadValue, a_fWrIgnMask, a_fWrGpMask, a_szName, \
      { 0 }, { 0 }, { 0 }, { 0 } }
#else
# define RINT(a_uFirst, a_uLast, a_enmRdFn, a_enmWrFn, a_offCpumCpu, a_uInitOrReadValue, a_fWrIgnMask, a_fWrGpMask, a_szName) \
    { a_uFirst, a_uLast, a_enmRdFn, a_enmWrFn, a_offCpumCpu, 0, a_uInitOrReadValue, a_fWrIgnMask, a_fWrGpMask, a_szName }
#endif
/** @} */


#include "cpus/Intel_Core_i7_3960X.h"
#include "cpus/Intel_Core_i5_3570.h"
#include "cpus/Intel_Core_i7_2635QM.h"
#include "cpus/Intel_Xeon_X5482_3_20GHz.h"
#include "cpus/Intel_Pentium_M_processor_2_00GHz.h"
#include "cpus/Intel_Pentium_4_3_00GHz.h"

#include "cpus/AMD_FX_8150_Eight_Core.h"
#include "cpus/AMD_Phenom_II_X6_1100T.h"
#include "cpus/Quad_Core_AMD_Opteron_2384.h"
#include "cpus/AMD_Athlon_64_X2_Dual_Core_4200.h"
#include "cpus/AMD_Athlon_64_3200.h"

#include "cpus/VIA_QuadCore_L4700_1_2_GHz.h"



/**
 * The database entries.
 *
 * 1. The first entry is special.  It is the fallback for unknown
 *    processors.  Thus, it better be pretty representative.
 *
 * 2. The first entry for a CPU vendor is likewise important as it is
 *    the default entry for that vendor.
 *
 * Generally we put the most recent CPUs first, since these tend to have the
 * most complicated and backwards compatible list of MSRs.
 */
static CPUMDBENTRY const * const g_apCpumDbEntries[] =
{
#ifdef VBOX_CPUDB_Intel_Core_i5_3570
    &g_Entry_Intel_Core_i5_3570,
#endif
#ifdef VBOX_CPUDB_Intel_Core_i7_3960X
    &g_Entry_Intel_Core_i7_3960X,
#endif
#ifdef VBOX_CPUDB_Intel_Core_i7_2635QM
    &g_Entry_Intel_Core_i7_2635QM,
#endif
#ifdef Intel_Pentium_M_processor_2_00GHz
    &g_Entry_Intel_Pentium_M_processor_2_00GHz,
#endif
#ifdef VBOX_CPUDB_Intel_Xeon_X5482_3_20GHz
    &g_Entry_Intel_Xeon_X5482_3_20GHz,
#endif
#ifdef VBOX_CPUDB_Intel_Pentium_4_3_00GHz
    &g_Entry_Intel_Pentium_4_3_00GHz,
#endif

#ifdef VBOX_CPUDB_AMD_FX_8150_Eight_Core
    &g_Entry_AMD_FX_8150_Eight_Core,
#endif
#ifdef VBOX_CPUDB_AMD_Phenom_II_X6_1100T
    &g_Entry_AMD_Phenom_II_X6_1100T,
#endif
#ifdef VBOX_CPUDB_Quad_Core_AMD_Opteron_2384
    &g_Entry_Quad_Core_AMD_Opteron_2384,
#endif
#ifdef VBOX_CPUDB_AMD_Athlon_64_X2_Dual_Core_4200
    &g_Entry_AMD_Athlon_64_X2_Dual_Core_4200,
#endif
#ifdef VBOX_CPUDB_AMD_Athlon_64_3200
    &g_Entry_AMD_Athlon_64_3200,
#endif

#ifdef VBOX_CPUDB_VIA_QuadCore_L4700_1_2_GHz
    &g_Entry_VIA_QuadCore_L4700_1_2_GHz,
#endif
};


#ifndef CPUM_DB_STANDALONE

/**
 * Binary search used by cpumR3MsrRangesInsert and has some special properties
 * wrt to mismatches.
 *
 * @returns Insert location.
 * @param   paMsrRanges         The MSR ranges to search.
 * @param   cMsrRanges          The number of MSR ranges.
 * @param   uMsr                What to search for.
 */
static uint32_t cpumR3MsrRangesBinSearch(PCCPUMMSRRANGE paMsrRanges, uint32_t cMsrRanges, uint32_t uMsr)
{
    if (!cMsrRanges)
        return 0;

    uint32_t iStart = 0;
    uint32_t iLast  = cMsrRanges - 1;
    for (;;)
    {
        uint32_t i = iStart + (iLast - iStart + 1) / 2;
        if (   uMsr >= paMsrRanges[i].uFirst
            && uMsr <= paMsrRanges[i].uLast)
            return i;
        if (uMsr < paMsrRanges[i].uFirst)
        {
            if (i <= iStart)
                return i;
            iLast = i - 1;
        }
        else
        {
            if (i >= iLast)
            {
                if (i < cMsrRanges)
                    i++;
                return i;
            }
            iStart = i + 1;
        }
    }
}


/**
 * Ensures that there is space for at least @a cNewRanges in the table,
 * reallocating the table if necessary.
 *
 * @returns Pointer to the MSR ranges on success, NULL on failure.  On failure
 *          @a *ppaMsrRanges is freed and set to NULL.
 * @param   ppaMsrRanges    The variable pointing to the ranges (input/output).
 * @param   cMsrRanges      The current number of ranges.
 * @param   cNewRanges      The number of ranges to be added.
 */
static PCPUMMSRRANGE cpumR3MsrRangesEnsureSpace(PCPUMMSRRANGE *ppaMsrRanges, uint32_t cMsrRanges, uint32_t cNewRanges)
{
    uint32_t cMsrRangesAllocated = RT_ALIGN_32(cMsrRanges, 16);
    if (cMsrRangesAllocated < cMsrRanges + cNewRanges)
    {
        uint32_t cNew = RT_ALIGN_32(cMsrRanges + cNewRanges, 16);
        void *pvNew = RTMemRealloc(*ppaMsrRanges, cNew * sizeof(**ppaMsrRanges));
        if (!pvNew)
        {
            RTMemFree(*ppaMsrRanges);
            *ppaMsrRanges = NULL;
            return NULL;
        }
        *ppaMsrRanges = (PCPUMMSRRANGE)pvNew;
    }
    return *ppaMsrRanges;
}


/**
 * Inserts a new MSR range in into an sorted MSR range array.
 *
 * If the new MSR range overlaps existing ranges, the existing ones will be
 * adjusted/removed to fit in the new one.
 *
 * @returns VBox status code.
 * @retval  VINF_SUCCESS
 * @retval  VERR_NO_MEMORY
 *
 * @param   ppaMsrRanges    The variable pointing to the ranges (input/output).
 * @param   pcMsrRanges     The variable holding number of ranges.
 * @param   pNewRange       The new range.
 */
int cpumR3MsrRangesInsert(PCPUMMSRRANGE *ppaMsrRanges, uint32_t *pcMsrRanges, PCCPUMMSRRANGE pNewRange)
{
    uint32_t        cMsrRanges  = *pcMsrRanges;
    PCPUMMSRRANGE   paMsrRanges = *ppaMsrRanges;

    Assert(pNewRange->uLast >= pNewRange->uFirst);
    Assert(pNewRange->enmRdFn > kCpumMsrRdFn_Invalid && pNewRange->enmRdFn < kCpumMsrRdFn_End);
    Assert(pNewRange->enmWrFn > kCpumMsrWrFn_Invalid && pNewRange->enmWrFn < kCpumMsrWrFn_End);

    /*
     * Optimize the linear insertion case where we add new entries at the end.
     */
    if (   cMsrRanges > 0
        && paMsrRanges[cMsrRanges - 1].uLast < pNewRange->uFirst)
    {
        paMsrRanges = cpumR3MsrRangesEnsureSpace(ppaMsrRanges, cMsrRanges, 1);
        if (!paMsrRanges)
            return VERR_NO_MEMORY;
        paMsrRanges[cMsrRanges] = *pNewRange;
        *pcMsrRanges += 1;
    }
    else
    {
        uint32_t i = cpumR3MsrRangesBinSearch(paMsrRanges, cMsrRanges, pNewRange->uFirst);
        Assert(i == cMsrRanges || pNewRange->uFirst <= paMsrRanges[i].uLast);
        Assert(i == 0 || pNewRange->uFirst > paMsrRanges[i - 1].uLast);

        /*
         * Adding an entirely new entry?
         */
        if (   i >= cMsrRanges
            || pNewRange->uLast < paMsrRanges[i].uFirst)
        {
            paMsrRanges = cpumR3MsrRangesEnsureSpace(ppaMsrRanges, cMsrRanges, 1);
            if (!paMsrRanges)
                return VERR_NO_MEMORY;
            if (i < cMsrRanges)
                memmove(&paMsrRanges[i + 1], &paMsrRanges[i], (cMsrRanges - i) * sizeof(paMsrRanges[0]));
            paMsrRanges[i] = *pNewRange;
            *pcMsrRanges += 1;
        }
        /*
         * Replace existing entry?
         */
        else if (   pNewRange->uFirst == paMsrRanges[i].uFirst
                 && pNewRange->uLast  == paMsrRanges[i].uLast)
            paMsrRanges[i] = *pNewRange;
        /*
         * Splitting an existing entry?
         */
        else if (   pNewRange->uFirst > paMsrRanges[i].uFirst
                 && pNewRange->uLast  < paMsrRanges[i].uLast)
        {
            paMsrRanges = cpumR3MsrRangesEnsureSpace(ppaMsrRanges, cMsrRanges, 2);
            if (!paMsrRanges)
                return VERR_NO_MEMORY;
            if (i < cMsrRanges)
                memmove(&paMsrRanges[i + 2], &paMsrRanges[i], (cMsrRanges - i) * sizeof(paMsrRanges[0]));
            paMsrRanges[i + 1] = *pNewRange;
            paMsrRanges[i + 2] = paMsrRanges[i];
            paMsrRanges[i    ].uLast  = pNewRange->uFirst - 1;
            paMsrRanges[i + 2].uFirst = pNewRange->uLast  + 1;
            *pcMsrRanges += 2;
        }
        /*
         * Complicated scenarios that can affect more than one range.
         *
         * The current code does not optimize memmove calls when replacing
         * one or more existing ranges, because it's tedious to deal with and
         * not expected to be a frequent usage scenario.
         */
        else
        {
            /* Adjust start of first match? */
            if (   pNewRange->uFirst <= paMsrRanges[i].uFirst
                && pNewRange->uLast  <  paMsrRanges[i].uLast)
                paMsrRanges[i].uFirst = pNewRange->uLast + 1;
            else
            {
                /* Adjust end of first match? */
                if (pNewRange->uFirst > paMsrRanges[i].uFirst)
                {
                    Assert(paMsrRanges[i].uLast >= pNewRange->uFirst);
                    paMsrRanges[i].uLast = pNewRange->uFirst - 1;
                    i++;
                }
                /* Replace the whole first match (lazy bird). */
                else
                {
                    if (i + 1 < cMsrRanges)
                        memmove(&paMsrRanges[i], &paMsrRanges[i + 1], (cMsrRanges - i - 1) * sizeof(paMsrRanges[0]));
                    cMsrRanges = *pcMsrRanges -= 1;
                }

                /* Do the new range affect more ranges? */
                while (   i < cMsrRanges
                       && pNewRange->uLast >= paMsrRanges[i].uFirst)
                {
                    if (pNewRange->uLast < paMsrRanges[i].uLast)
                    {
                        /* Adjust the start of it, then we're done. */
                        paMsrRanges[i].uFirst = pNewRange->uLast + 1;
                        break;
                    }

                    /* Remove it entirely. */
                    if (i + 1 < cMsrRanges)
                        memmove(&paMsrRanges[i], &paMsrRanges[i + 1], (cMsrRanges - i - 1) * sizeof(paMsrRanges[0]));
                    cMsrRanges = *pcMsrRanges -= 1;
                }
            }

            /* Now, perform a normal insertion. */
            paMsrRanges = cpumR3MsrRangesEnsureSpace(ppaMsrRanges, cMsrRanges, 1);
            if (!paMsrRanges)
                return VERR_NO_MEMORY;
            if (i < cMsrRanges)
                memmove(&paMsrRanges[i + 1], &paMsrRanges[i], (cMsrRanges - i) * sizeof(paMsrRanges[0]));
            paMsrRanges[i] = *pNewRange;
            *pcMsrRanges += 1;
        }
    }

    return VINF_SUCCESS;
}


/**
 * Worker for cpumR3MsrApplyFudge that applies one table.
 *
 * @returns VBox status code.
 * @param   pVM                 Pointer to the cross context VM structure.
 * @param   paRanges            Array of MSRs to fudge.
 * @param   cRanges             Number of MSRs in the array.
 */
static int cpumR3MsrApplyFudgeTable(PVM pVM, PCCPUMMSRRANGE paRanges, size_t cRanges)
{
    for (uint32_t i = 0; i < cRanges; i++)
        if (!cpumLookupMsrRange(pVM, paRanges[i].uFirst))
        {
            LogRel(("CPUM: MSR fudge: %#010x %s\n", paRanges[i].uFirst, paRanges[i].szName));
            int rc = cpumR3MsrRangesInsert(&pVM->cpum.s.GuestInfo.paMsrRangesR3, &pVM->cpum.s.GuestInfo.cMsrRanges,
                                           &paRanges[i]);
            if (RT_FAILURE(rc))
                return rc;
        }
    return VINF_SUCCESS;
}


/**
 * Fudges the MSRs that guest are known to access in some odd cases.
 *
 * A typical example is a VM that has been moved between different hosts where
 * for instance the cpu vendor differs.
 *
 * @returns VBox status code.
 * @param   pVM                 Pointer to the cross context VM structure.
 */
int cpumR3MsrApplyFudge(PVM pVM)
{
    /*
     * Basic.
     */
    static CPUMMSRRANGE const s_aFudgeMsrs[] =
    {
        MFO(0x00000000, "IA32_P5_MC_ADDR",          Ia32P5McAddr),
        MFX(0x00000001, "IA32_P5_MC_TYPE",          Ia32P5McType,   Ia32P5McType,   0, 0, UINT64_MAX),
        MVO(0x00000017, "IA32_PLATFORM_ID",         0),
        MFN(0x0000001b, "IA32_APIC_BASE",           Ia32ApicBase,   Ia32ApicBase),
        MVI(0x0000008b, "BIOS_SIGN",                0),
        MFX(0x000000fe, "IA32_MTRRCAP",             Ia32MtrrCap,    ReadOnly,       0x508, 0, 0),
        MFX(0x00000179, "IA32_MCG_CAP",             Ia32McgCap,     ReadOnly,       0x005, 0, 0),
        MFX(0x0000017a, "IA32_MCG_STATUS",          Ia32McgStatus,  Ia32McgStatus,  0, ~(uint64_t)UINT32_MAX, 0),
        MFN(0x000001a0, "IA32_MISC_ENABLE",         Ia32MiscEnable, Ia32MiscEnable),
        MFN(0x000001d9, "IA32_DEBUGCTL",            Ia32DebugCtl,   Ia32DebugCtl),
        MFO(0x000001db, "P6_LAST_BRANCH_FROM_IP",   P6LastBranchFromIp),
        MFO(0x000001dc, "P6_LAST_BRANCH_TO_IP",     P6LastBranchToIp),
        MFO(0x000001dd, "P6_LAST_INT_FROM_IP",      P6LastIntFromIp),
        MFO(0x000001de, "P6_LAST_INT_TO_IP",        P6LastIntToIp),
        MFS(0x00000277, "IA32_PAT",                 Ia32Pat, Ia32Pat, Guest.msrPAT),
        MFZ(0x000002ff, "IA32_MTRR_DEF_TYPE",       Ia32MtrrDefType, Ia32MtrrDefType, GuestMsrs.msr.MtrrDefType, 0, ~(uint64_t)0xc07),
        MFN(0x00000400, "IA32_MCi_CTL_STATUS_ADDR_MISC", Ia32McCtlStatusAddrMiscN, Ia32McCtlStatusAddrMiscN),
    };
    int rc = cpumR3MsrApplyFudgeTable(pVM, &s_aFudgeMsrs[0], RT_ELEMENTS(s_aFudgeMsrs));
    AssertLogRelRCReturn(rc, rc);

    /*
     * XP might mistake opterons and other newer CPUs for P4s.
     */
    if (pVM->cpum.s.GuestFeatures.uFamily >= 0xf)
    {
        static CPUMMSRRANGE const s_aP4FudgeMsrs[] =
        {
            MFX(0x0000002c, "P4_EBC_FREQUENCY_ID", IntelP4EbcFrequencyId, IntelP4EbcFrequencyId, 0xf12010f, UINT64_MAX, 0),
        };
        rc = cpumR3MsrApplyFudgeTable(pVM, &s_aP4FudgeMsrs[0], RT_ELEMENTS(s_aP4FudgeMsrs));
        AssertLogRelRCReturn(rc, rc);
    }

    return rc;
}


int cpumR3DbGetCpuInfo(const char *pszName, PCPUMINFO pInfo)
{
    CPUMDBENTRY const *pEntry = NULL;
    int                rc;

    if (!strcmp(pszName, "host"))
    {
        /*
         * Create a CPU database entry for the host CPU.  This means getting
         * the CPUID bits from the real CPU and grabbing the closest matching
         * database entry for MSRs.
         */
        rc = CPUMR3CpuIdDetectUnknownLeafMethod(&pInfo->enmUnknownCpuIdMethod, &pInfo->DefCpuId);
        if (RT_FAILURE(rc))
            return rc;
        rc = CPUMR3CpuIdCollectLeaves(&pInfo->paCpuIdLeavesR3, &pInfo->cCpuIdLeaves);
        if (RT_FAILURE(rc))
            return rc;

        /* Lookup database entry for MSRs. */
        CPUMCPUVENDOR const enmVendor    = CPUMR3CpuIdDetectVendorEx(pInfo->paCpuIdLeavesR3[0].uEax,
                                                                     pInfo->paCpuIdLeavesR3[0].uEbx,
                                                                     pInfo->paCpuIdLeavesR3[0].uEcx,
                                                                     pInfo->paCpuIdLeavesR3[0].uEdx);
        uint32_t      const uStd1Eax     = pInfo->paCpuIdLeavesR3[1].uEax;
        uint8_t       const uFamily      = ASMGetCpuFamily(uStd1Eax);
        uint8_t       const uModel       = ASMGetCpuModel(uStd1Eax, enmVendor == CPUMCPUVENDOR_INTEL);
        uint8_t       const uStepping    = ASMGetCpuStepping(uStd1Eax);
        CPUMMICROARCH const enmMicroarch = CPUMR3CpuIdDetermineMicroarchEx(enmVendor, uFamily, uModel, uStepping);

        for (unsigned i = 0; i < RT_ELEMENTS(g_apCpumDbEntries); i++)
        {
            CPUMDBENTRY const *pCur = g_apCpumDbEntries[i];
            if ((CPUMCPUVENDOR)pCur->enmVendor == enmVendor)
            {
                /* Match against Family, Microarch, model and stepping.  Except
                   for family, always match the closer with preference given to
                   the later/older ones. */
                if (pCur->uFamily == uFamily)
                {
                    if (pCur->enmMicroarch == enmMicroarch)
                    {
                        if (pCur->uModel == uModel)
                        {
                            if (pCur->uStepping == uStepping)
                            {
                                /* Perfect match. */
                                pEntry = pCur;
                                break;
                            }

                            if (   !pEntry
                                || pEntry->uModel       != uModel
                                || pEntry->enmMicroarch != enmMicroarch
                                || pEntry->uFamily      != uFamily)
                                pEntry = pCur;
                            else if (  pCur->uStepping >= uStepping
                                     ? pCur->uStepping < pEntry->uStepping || pEntry->uStepping < uStepping
                                     : pCur->uStepping > pEntry->uStepping)
                                     pEntry = pCur;
                        }
                        else if (   !pEntry
                                 || pEntry->enmMicroarch != enmMicroarch
                                 || pEntry->uFamily      != uFamily)
                            pEntry = pCur;
                        else if (  pCur->uModel >= uModel
                                 ? pCur->uModel < pEntry->uModel || pEntry->uModel < uModel
                                 : pCur->uModel > pEntry->uModel)
                            pEntry = pCur;
                    }
                    else if (   !pEntry
                             || pEntry->uFamily != uFamily)
                        pEntry = pCur;
                    else if (  pCur->enmMicroarch >= enmMicroarch
                             ? pCur->enmMicroarch < pEntry->enmMicroarch || pEntry->enmMicroarch < enmMicroarch
                             : pCur->enmMicroarch > pEntry->enmMicroarch)
                        pEntry = pCur;
                }
                /* We don't do closeness matching on family, we use the first
                   entry for the CPU vendor instead. (P4 workaround.) */
                else if (!pEntry)
                    pEntry = pCur;
            }
        }

        if (pEntry)
            LogRel(("CPUM: Matched host CPU %s %#x/%#x/%#x %s with CPU DB entry '%s' (%s %#x/%#x/%#x %s).\n",
                    CPUMR3CpuVendorName(enmVendor), uFamily, uModel, uStepping, CPUMR3MicroarchName(enmMicroarch),
                    pEntry->pszName,  CPUMR3CpuVendorName((CPUMCPUVENDOR)pEntry->enmVendor), pEntry->uFamily, pEntry->uModel,
                    pEntry->uStepping, CPUMR3MicroarchName(pEntry->enmMicroarch) ));
        else
        {
            pEntry = g_apCpumDbEntries[0];
            LogRel(("CPUM: No matching processor database entry %s %#x/%#x/%#x %s, falling back on '%s'.\n",
                    CPUMR3CpuVendorName(enmVendor), uFamily, uModel, uStepping, CPUMR3MicroarchName(enmMicroarch),
                    pEntry->pszName));
        }
    }
    else
    {
        /*
         * We're supposed to be emulating a specific CPU that is included in
         * our CPU database.  The CPUID tables needs to be copied onto the
         * heap so the caller can modify them and so they can be freed like
         * in the host case above.
         */
        for (unsigned i = 0; i < RT_ELEMENTS(g_apCpumDbEntries); i++)
            if (!strcmp(pszName, g_apCpumDbEntries[i]->pszName))
            {
                pEntry = g_apCpumDbEntries[i];
                break;
            }
        if (!pEntry)
        {
            LogRel(("CPUM: Cannot locate any CPU by the name '%s'\n", pszName));
            return VERR_CPUM_DB_CPU_NOT_FOUND;
        }

        pInfo->cCpuIdLeaves = pEntry->cCpuIdLeaves;
        if (pEntry->cCpuIdLeaves)
        {
            pInfo->paCpuIdLeavesR3 = (PCPUMCPUIDLEAF)RTMemDup(pEntry->paCpuIdLeaves,
                                                              sizeof(pEntry->paCpuIdLeaves[0]) * pEntry->cCpuIdLeaves);
            if (!pInfo->paCpuIdLeavesR3)
                return VERR_NO_MEMORY;
        }
        else
            pInfo->paCpuIdLeavesR3 = NULL;

        pInfo->enmUnknownCpuIdMethod = pEntry->enmUnknownCpuId;
        pInfo->DefCpuId         = pEntry->DefUnknownCpuId;

        LogRel(("CPUM: Using CPU DB entry '%s' (%s %#x/%#x/%#x %s).\n",
                pEntry->pszName, CPUMR3CpuVendorName((CPUMCPUVENDOR)pEntry->enmVendor),
                pEntry->uFamily, pEntry->uModel, pEntry->uStepping, CPUMR3MicroarchName(pEntry->enmMicroarch) ));
    }

    pInfo->fMsrMask             = pEntry->fMsrMask;
    pInfo->iFirstExtCpuIdLeaf   = 0; /* Set by caller. */
    pInfo->uPadding             = 0;
    pInfo->uScalableBusFreq     = pEntry->uScalableBusFreq;
    pInfo->paCpuIdLeavesR0      = NIL_RTR0PTR;
    pInfo->paMsrRangesR0        = NIL_RTR0PTR;
    pInfo->paCpuIdLeavesRC      = NIL_RTRCPTR;
    pInfo->paMsrRangesRC        = NIL_RTRCPTR;

    /*
     * Copy the MSR range.
     */
    uint32_t        cMsrs   = 0;
    PCPUMMSRRANGE   paMsrs  = NULL;

    PCCPUMMSRRANGE  pCurMsr = pEntry->paMsrRanges;
    uint32_t        cLeft   = pEntry->cMsrRanges;
    while (cLeft-- > 0)
    {
        rc = cpumR3MsrRangesInsert(&paMsrs, &cMsrs, pCurMsr);
        if (RT_FAILURE(rc))
        {
            Assert(!paMsrs); /* The above function frees this. */
            RTMemFree(pInfo->paCpuIdLeavesR3);
            pInfo->paCpuIdLeavesR3 = NULL;
            return rc;
        }
        pCurMsr++;
    }

    pInfo->paMsrRangesR3   = paMsrs;
    pInfo->cMsrRanges      = cMsrs;
    return VINF_SUCCESS;
}


/**
 * Register statistics for the MSRs.
 *
 * This must not be called before the MSRs have been finalized and moved to the
 * hyper heap.
 *
 * @returns VBox status code.
 * @param   pVM                 Pointer to the cross context VM structure.
 */
int cpumR3MsrRegStats(PVM pVM)
{
    /*
     * Global statistics.
     */
    PCPUM pCpum = &pVM->cpum.s;
    STAM_REL_REG(pVM, &pCpum->cMsrReads,                STAMTYPE_COUNTER,   "/CPUM/MSR-Totals/Reads",
                 STAMUNIT_OCCURENCES, "All RDMSRs making it to CPUM.");
    STAM_REL_REG(pVM, &pCpum->cMsrReadsRaiseGp,         STAMTYPE_COUNTER,   "/CPUM/MSR-Totals/ReadsRaisingGP",
                 STAMUNIT_OCCURENCES, "RDMSR raising #GPs, except unknown MSRs.");
    STAM_REL_REG(pVM, &pCpum->cMsrReadsUnknown,         STAMTYPE_COUNTER,   "/CPUM/MSR-Totals/ReadsUnknown",
                 STAMUNIT_OCCURENCES, "RDMSR on unknown MSRs (raises #GP).");
    STAM_REL_REG(pVM, &pCpum->cMsrWrites,               STAMTYPE_COUNTER,   "/CPUM/MSR-Totals/Writes",
                 STAMUNIT_OCCURENCES, "All RDMSRs making it to CPUM.");
    STAM_REL_REG(pVM, &pCpum->cMsrWritesRaiseGp,        STAMTYPE_COUNTER,   "/CPUM/MSR-Totals/WritesRaisingGP",
                 STAMUNIT_OCCURENCES, "WRMSR raising #GPs, except unknown MSRs.");
    STAM_REL_REG(pVM, &pCpum->cMsrWritesToIgnoredBits,  STAMTYPE_COUNTER,   "/CPUM/MSR-Totals/WritesToIgnoredBits",
                 STAMUNIT_OCCURENCES, "Writing of ignored bits.");
    STAM_REL_REG(pVM, &pCpum->cMsrWritesUnknown,        STAMTYPE_COUNTER,   "/CPUM/MSR-Totals/WritesUnknown",
                 STAMUNIT_OCCURENCES, "WRMSR on unknown MSRs (raises #GP).");


# ifdef VBOX_WITH_STATISTICS
    /*
     * Per range.
     */
    PCPUMMSRRANGE   paRanges = pVM->cpum.s.GuestInfo.paMsrRangesR3;
    uint32_t        cRanges  = pVM->cpum.s.GuestInfo.cMsrRanges;
    for (uint32_t i = 0; i < cRanges; i++)
    {
        char    szName[160];
        ssize_t cchName;

        if (paRanges[i].uFirst == paRanges[i].uLast)
            cchName = RTStrPrintf(szName, sizeof(szName), "/CPUM/MSRs/%#010x-%s",
                                  paRanges[i].uFirst, paRanges[i].szName);
        else
            cchName = RTStrPrintf(szName, sizeof(szName), "/CPUM/MSRs/%#010x-%#010x-%s",
                                  paRanges[i].uFirst, paRanges[i].uLast, paRanges[i].szName);

        RTStrCopy(&szName[cchName], sizeof(szName) - cchName, "-reads");
        STAMR3Register(pVM, &paRanges[i].cReads, STAMTYPE_COUNTER, STAMVISIBILITY_ALWAYS, szName, STAMUNIT_OCCURENCES, "RDMSR");

        RTStrCopy(&szName[cchName], sizeof(szName) - cchName, "-writes");
        STAMR3Register(pVM, &paRanges[i].cWrites, STAMTYPE_COUNTER, STAMVISIBILITY_USED, szName, STAMUNIT_OCCURENCES, "WRMSR");

        RTStrCopy(&szName[cchName], sizeof(szName) - cchName, "-GPs");
        STAMR3Register(pVM, &paRanges[i].cGps, STAMTYPE_COUNTER, STAMVISIBILITY_USED, szName, STAMUNIT_OCCURENCES, "#GPs");

        RTStrCopy(&szName[cchName], sizeof(szName) - cchName, "-ign-bits-writes");
        STAMR3Register(pVM, &paRanges[i].cIgnoredBits, STAMTYPE_COUNTER, STAMVISIBILITY_USED, szName, STAMUNIT_OCCURENCES, "WRMSR w/ ignored bits");
    }
# endif /* VBOX_WITH_STATISTICS */

    return VINF_SUCCESS;
}

#endif /* !CPUM_DB_STANDALONE */

