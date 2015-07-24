/* $Id: tstGIP-2.cpp $ */
/** @file
 * SUP Testcase - Global Info Page interface (ring 3).
 */

/*
 * Copyright (C) 2006-2013 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 *
 * The contents of this file may alternatively be used under the terms
 * of the Common Development and Distribution License Version 1.0
 * (CDDL) only, as it comes in the "COPYING.CDDL" file of the
 * VirtualBox OSE distribution, in which case the provisions of the
 * CDDL are applicable instead of those of the GPL.
 *
 * You may elect to license modified versions of this file under the
 * terms and conditions of either the GPL or the CDDL or both.
 */

/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#include <VBox/sup.h>
#include <VBox/err.h>
#include <VBox/param.h>
#include <iprt/asm.h>
#include <iprt/x86.h>
#include <iprt/asm-amd64-x86.h>
#include <iprt/assert.h>
#include <iprt/alloc.h>
#include <iprt/thread.h>
#include <iprt/stream.h>
#include <iprt/string.h>
#include <iprt/initterm.h>
#include <iprt/getopt.h>


/**
 * Checks whether the CPU advertises an invariant TSC or not.
 *
 * @returns true if invariant, false otherwise.
 */
bool tstIsInvariantTsc(void)
{
    if (ASMHasCpuId())
    {
        uint32_t uEax, uEbx, uEcx, uEdx;
        ASMCpuId(0x80000000, &uEax, &uEbx, &uEcx, &uEdx);
        if (uEax >= 0x80000007)
        {
            ASMCpuId(0x80000007, &uEax, &uEbx, &uEcx, &uEdx);
            if (uEdx & X86_CPUID_AMD_ADVPOWER_EDX_TSCINVAR)
                return true;
        }
    }
    return false;
}


int main(int argc, char **argv)
{
    RTR3InitExe(argc, &argv, 0);

    /*
     * Parse args
     */
    static const RTGETOPTDEF g_aOptions[] =
    {
        { "--iterations",       'i', RTGETOPT_REQ_INT32 },
        { "--hex",              'h', RTGETOPT_REQ_NOTHING },
        { "--decimal",          'd', RTGETOPT_REQ_NOTHING },
        { "--spin",             's', RTGETOPT_REQ_NOTHING },
        { "--reference",        'r', RTGETOPT_REQ_UINT64 },  /* reference value of CpuHz, display the
                                                              * difference in a separate column. */
    };

    uint32_t cIterations = 40;
    bool fHex = true;
    bool fSpin = false;
    int ch;
    uint64_t uCpuHzRef = 0;
    uint64_t uCpuHzOverallDeviation = 0;
    int64_t  iCpuHzMaxDeviation = 0;
    uint32_t cCpuHzOverallDevCnt = 0;
    RTGETOPTUNION ValueUnion;
    RTGETOPTSTATE GetState;
    RTGetOptInit(&GetState, argc, argv, g_aOptions, RT_ELEMENTS(g_aOptions), 1, RTGETOPTINIT_FLAGS_NO_STD_OPTS);
    while ((ch = RTGetOpt(&GetState, &ValueUnion)))
    {
        switch (ch)
        {
            case 'i':
                cIterations = ValueUnion.u32;
                break;

            case 'd':
                fHex = false;
                break;

            case 'h':
                fHex = true;
                break;

            case 's':
                fSpin = true;
                break;

            case 'r':
                uCpuHzRef = ValueUnion.u64;
                break;

            default:
                return RTGetOptPrintError(ch, &ValueUnion);
        }
    }

    /*
     * Init
     */
    PSUPDRVSESSION pSession = NIL_RTR0PTR;
    int rc = SUPR3Init(&pSession);
    if (RT_SUCCESS(rc))
    {
        if (g_pSUPGlobalInfoPage)
        {
            RTPrintf("tstGIP-2: cCpus=%d  u32UpdateHz=%RU32  u32UpdateIntervalNS=%RU32  u64NanoTSLastUpdateHz=%RX64  uCpuHzRef=%RU64  u32Mode=%d (%s) u32Version=%#x\n",
                     g_pSUPGlobalInfoPage->cCpus,
                     g_pSUPGlobalInfoPage->u32UpdateHz,
                     g_pSUPGlobalInfoPage->u32UpdateIntervalNS,
                     g_pSUPGlobalInfoPage->u64NanoTSLastUpdateHz,
                     uCpuHzRef,
                     g_pSUPGlobalInfoPage->u32Mode,
                     g_pSUPGlobalInfoPage->u32Mode == SUPGIPMODE_SYNC_TSC       ? "sync"
                     : g_pSUPGlobalInfoPage->u32Mode == SUPGIPMODE_ASYNC_TSC    ? "async"
                     :                                                            "???",
                     g_pSUPGlobalInfoPage->u32Version);
            RTPrintf(fHex
                     ? "tstGIP-2:     it: u64NanoTS        delta     u64TSC           UpIntTSC H  TransId           CpuHz %sTSC Interval History...\n"
                     : "tstGIP-2:     it: u64NanoTS        delta     u64TSC             UpIntTSC H    TransId           CpuHz %sTSC Interval History...\n",
                     uCpuHzRef ? "  CpuHz deviation  " : "");
            static SUPGIPCPU s_aaCPUs[2][256];
            for (uint32_t i = 0; i < cIterations; i++)
            {
                /* copy the data */
                memcpy(&s_aaCPUs[i & 1][0], &g_pSUPGlobalInfoPage->aCPUs[0], g_pSUPGlobalInfoPage->cCpus * sizeof(g_pSUPGlobalInfoPage->aCPUs[0]));

                /* display it & find something to spin on. */
                uint32_t u32TransactionId = 0;
                uint32_t volatile *pu32TransactionId = NULL;
                for (unsigned iCpu = 0; iCpu < g_pSUPGlobalInfoPage->cCpus; iCpu++)
                    if (    g_pSUPGlobalInfoPage->aCPUs[iCpu].u64CpuHz > 0
                        &&  g_pSUPGlobalInfoPage->aCPUs[iCpu].u64CpuHz != _4G + 1)
                    {
                        char szCpuHzDeviation[32];
                        PSUPGIPCPU pPrevCpu = &s_aaCPUs[!(i & 1)][iCpu];
                        PSUPGIPCPU pCpu = &s_aaCPUs[i & 1][iCpu];
                        if (uCpuHzRef)
                        {
                            int64_t iCpuHzDeviation = pCpu->u64CpuHz - uCpuHzRef;
                            uint64_t uCpuHzDeviation = RT_ABS(iCpuHzDeviation);
                            if (uCpuHzDeviation > 999999999)
                                RTStrPrintf(szCpuHzDeviation, sizeof(szCpuHzDeviation), "%17s  ", "?");
                            else
                            {
                                if (pCpu->u32TransactionId > 23 + (8 * 2) + 1) /* Wait until the history validation code takes effect. */
                                {
                                    if (RT_ABS(iCpuHzDeviation) > RT_ABS(iCpuHzMaxDeviation))
                                        iCpuHzMaxDeviation = iCpuHzDeviation;
                                    uCpuHzOverallDeviation += uCpuHzDeviation;
                                    cCpuHzOverallDevCnt++;
                                }
                                uint32_t uPct = (uint32_t)(uCpuHzDeviation * 100000 / uCpuHzRef + 5);
                                RTStrPrintf(szCpuHzDeviation, sizeof(szCpuHzDeviation), "%10RI64%3d.%02d%%  ",
                                                iCpuHzDeviation, uPct / 1000, (uPct % 1000) / 10);
                            }
                        }
                        else
                            szCpuHzDeviation[0] = '\0';
                        RTPrintf(fHex
                                 ? "tstGIP-2: %4d/%d: %016llx %09llx %016llx %08x %d %08x %15llu %s%08x %08x %08x %08x %08x %08x %08x %08x (%d)\n"
                                 : "tstGIP-2: %4d/%d: %016llu %09llu %016llu %010u %d %010u %15llu %s%08x %08x %08x %08x %08x %08x %08x %08x (%d)\n",
                                 i, iCpu,
                                 pCpu->u64NanoTS,
                                 i ? pCpu->u64NanoTS - pPrevCpu->u64NanoTS : 0,
                                 pCpu->u64TSC,
                                 pCpu->u32UpdateIntervalTSC,
                                 pCpu->iTSCHistoryHead,
                                 pCpu->u32TransactionId,
                                 pCpu->u64CpuHz,
                                 szCpuHzDeviation,
                                 pCpu->au32TSCHistory[0],
                                 pCpu->au32TSCHistory[1],
                                 pCpu->au32TSCHistory[2],
                                 pCpu->au32TSCHistory[3],
                                 pCpu->au32TSCHistory[4],
                                 pCpu->au32TSCHistory[5],
                                 pCpu->au32TSCHistory[6],
                                 pCpu->au32TSCHistory[7],
                                 pCpu->cErrors);
                        if (!pu32TransactionId)
                        {
                            pu32TransactionId = &g_pSUPGlobalInfoPage->aCPUs[iCpu].u32TransactionId;
                            u32TransactionId = pCpu->u32TransactionId;
                        }
                    }

                /* wait a bit / spin */
                if (!fSpin)
                    RTThreadSleep(9);
                else
                {
                    if (pu32TransactionId)
                    {
                        while (u32TransactionId == *pu32TransactionId)
                            ASMNopPause();
                    }
                    else
                        RTThreadSleep(1);
                }
            }
            RTPrintf("CPUID.Invariant-TSC: %RTbool\n", tstIsInvariantTsc());
            if (   uCpuHzRef
                && cCpuHzOverallDevCnt)
            {
                uint32_t uPct    = (uint32_t)(uCpuHzOverallDeviation * 100000 / cCpuHzOverallDevCnt / uCpuHzRef + 5);
                uint32_t uMaxPct = (uint32_t)(RT_ABS(iCpuHzMaxDeviation) * 100000 / uCpuHzRef + 5);
                RTPrintf("Average CpuHz deviation: %d.%02d%%\n", uPct / 1000, (uPct % 1000) / 10);
                RTPrintf("Maximum CpuHz deviation: %d.%02d%% (%RI64 ticks)\n", uMaxPct / 1000, (uMaxPct % 1000) / 10, iCpuHzMaxDeviation);
            }
        }
        else
        {
            RTPrintf("tstGIP-2: g_pSUPGlobalInfoPage is NULL\n");
            rc = -1;
        }

        SUPR3Term(false /*fForced*/);
    }
    else
        RTPrintf("tstGIP-2: SUPR3Init failed: %Rrc\n", rc);
    return !!rc;
}
