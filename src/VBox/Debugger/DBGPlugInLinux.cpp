/* $Id: DBGPlugInLinux.cpp $ */
/** @file
 * DBGPlugInLinux - Debugger and Guest OS Digger Plugin For Linux.
 */

/*
 * Copyright (C) 2008-2015 Oracle Corporation
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
#define LOG_GROUP LOG_GROUP_DBGF ///@todo add new log group.
#include "DBGPlugIns.h"
#include "DBGPlugInCommonELF.h"
#include <VBox/vmm/dbgf.h>
#include <iprt/string.h>
#include <iprt/mem.h>
#include <iprt/stream.h>
#include <iprt/ctype.h>


/*******************************************************************************
*   Structures and Typedefs                                                    *
*******************************************************************************/

/** @name InternalLinux structures
 * @{ */


/** @} */


/**
 * Linux guest OS digger instance data.
 */
typedef struct DBGDIGGERLINUX
{
    /** Whether the information is valid or not.
     * (For fending off illegal interface method calls.) */
    bool fValid;
    /** Set if 64-bit, clear if 32-bit.  */
    bool f64Bit;

    /** The address of the linux banner.
     * This is set during probing. */
    DBGFADDRESS AddrLinuxBanner;
    /** Kernel base address.
     * This is set during probing, refined during kallsyms parsing. */
    DBGFADDRESS AddrKernelBase;
    /** The kernel size.   */
    uint32_t    cbKernel;

    /** The number of kernel symbols (kallsyms_num_syms).
     * This is set during init.  */
    uint32_t   cKernelSymbols;
    /** The size of the kernel name table (sizeof(kallsyms_names)).   */
    uint32_t   cbKernelNames;
    /** Number of entries in the kernel_markers table. */
    uint32_t   cKernelNameMarkers;
    /** The size of the kernel symbol token table. */
    uint32_t   cbKernelTokenTable;
    /** The address of the encoded kernel symbol names (kallsyms_names). */
    DBGFADDRESS AddrKernelNames;
    /** The address of the kernel symbol addresses (kallsyms_addresses). */
    DBGFADDRESS AddrKernelAddresses;
    /** The address of the kernel symbol name markers (kallsyms_markers). */
    DBGFADDRESS AddrKernelNameMarkers;
    /** The address of the kernel symbol token table (kallsyms_token_table). */
    DBGFADDRESS AddrKernelTokenTable;
    /** The address of the kernel symbol token index table (kallsyms_token_index). */
    DBGFADDRESS AddrKernelTokenIndex;

    /** The kernel message log interface. */
    DBGFOSIDMESG    IDmesg;
} DBGDIGGERLINUX;
/** Pointer to the linux guest OS digger instance data. */
typedef DBGDIGGERLINUX *PDBGDIGGERLINUX;


/**
 * The current printk_log structure.
 */
typedef struct LNXPRINTKHDR
{
    /** Monotonic timestamp. */
    uint64_t nsTimestamp;
    /** The total size of this message record. */
    uint16_t cbTotal;
    /** The size of the text part (immediately follows the header). */
    uint16_t cbText;
    /** The size of the optional dictionary part (follows the text). */
    uint16_t cbDict;
    /** The syslog facility number. */
    uint8_t  bFacility;
    /** First 5 bits are internal flags, next 3 bits are log level. */
    uint8_t  fFlagsAndLevel;
} LNXPRINTKHDR;
AssertCompileSize(LNXPRINTKHDR, 2*sizeof(uint64_t));
/** Pointer to linux printk_log header. */
typedef LNXPRINTKHDR *PLNXPRINTKHDR;
/** Pointer to linux const printk_log header. */
typedef LNXPRINTKHDR const *PCLNXPRINTKHDR;


/*******************************************************************************
*   Defined Constants And Macros                                               *
*******************************************************************************/
/** Validates a 32-bit linux kernel address */
#define LNX32_VALID_ADDRESS(Addr)       ((Addr) > UINT32_C(0x80000000) && (Addr) < UINT32_C(0xfffff000))
/** Validates a 64-bit linux kernel address */
#define LNX64_VALID_ADDRESS(Addr)       ((Addr) > UINT64_C(0xffff800000000000) && (Addr) < UINT64_C(0xfffffffffffff000))

/** The max kernel size. */
#define LNX_MAX_KERNEL_SIZE                 UINT32_C(0x0f000000)

/** The maximum size we expect for kallsyms_names. */
#define LNX_MAX_KALLSYMS_NAMES_SIZE         UINT32_C(0x200000)
/** The maximum size we expect for kallsyms_token_table. */
#define LNX_MAX_KALLSYMS_TOKEN_TABLE_SIZE   UINT32_C(0x10000)
/** The minimum number of symbols we expect in kallsyms_num_syms. */
#define LNX_MIN_KALLSYMS_SYMBOLS            UINT32_C(2048)
/** The maximum number of symbols we expect in kallsyms_num_syms. */
#define LNX_MAX_KALLSYMS_SYMBOLS            UINT32_C(1048576)
/** The min length an encoded symbol in kallsyms_names is expected to have. */
#define LNX_MIN_KALLSYMS_ENC_LENGTH         UINT8_C(1)
/** The max length an encoded symbol in kallsyms_names is expected to have.
 * @todo check real life here.  */
#define LNX_MAX_KALLSYMS_ENC_LENGTH         UINT8_C(28)
/** The approximate maximum length of a string token. */
#define LNX_MAX_KALLSYMS_TOKEN_LEN          UINT16_C(32)

/** Module tag for linux ('linuxmod' on little endian ASCII systems). */
#define DIG_LNX_MOD_TAG                     UINT64_C(0x545f5d78758e898c)


/*******************************************************************************
*   Internal Functions                                                         *
*******************************************************************************/
static DECLCALLBACK(int)  dbgDiggerLinuxInit(PUVM pUVM, void *pvData);


/*******************************************************************************
*   Global Variables                                                           *
*******************************************************************************/
/** Table of common linux kernel addresses. */
static uint64_t g_au64LnxKernelAddresses[] =
{
    UINT64_C(0xc0100000),
    UINT64_C(0x90100000),
    UINT64_C(0xffffffff80200000)
};


/**
 * @interface_method_impl{DBGFOSIDMESG,pfnQueryKernelLog}
 */
static DECLCALLBACK(int) dbgDiggerLinuxIDmsg_QueryKernelLog(PDBGFOSIDMESG pThis, PUVM pUVM, uint32_t fFlags, uint32_t cMessages,
                                                            char *pszBuf, size_t cbBuf, size_t *pcbActual)
{
    PDBGDIGGERLINUX pData = RT_FROM_MEMBER(pThis, DBGDIGGERLINUX, IDmesg);

    if (cMessages < 1)
        return VERR_INVALID_PARAMETER;

    /*
     * Resolve the symbols we need and read their values.
     */
    RTDBGAS  hAs = DBGFR3AsResolveAndRetain(pUVM, DBGF_AS_KERNEL);
    RTDBGMOD hMod;
    int rc = RTDbgAsModuleByName(hAs, "vmlinux", 0, &hMod);
    if (RT_FAILURE(rc))
        return VERR_NOT_FOUND;
    RTDbgAsRelease(hAs);

    RTGCPTR  GCPtrLogBuf;
    uint32_t cbLogBuf;
    uint32_t idxFirst;
    uint32_t idxNext;

    struct { void *pvVar; size_t cbHost, cbGuest; const char *pszSymbol; } aSymbols[] =
    {
        { &GCPtrLogBuf, sizeof(GCPtrLogBuf),    pData->f64Bit ? sizeof(uint64_t) : sizeof(uint32_t),   "log_buf" },
        { &cbLogBuf,    sizeof(cbLogBuf),       sizeof(cbLogBuf),                                      "log_buf_len" },
        { &idxFirst,    sizeof(idxFirst),       sizeof(idxFirst),                                      "log_first_idx" },
        { &idxNext,     sizeof(idxNext),        sizeof(idxNext),                                       "log_next_idx" },
    };
    for (uint32_t i = 0; i < RT_ELEMENTS(aSymbols); i++)
    {
        RTDBGSYMBOL SymInfo;
        rc = RTDbgModSymbolByName(hMod, aSymbols[i].pszSymbol, &SymInfo);
        if (RT_SUCCESS(rc))
        {
            RT_BZERO(aSymbols[i].pvVar, aSymbols[i].cbHost);
            Assert(aSymbols[i].cbHost >= aSymbols[i].cbGuest);
            DBGFADDRESS Addr;
            rc = DBGFR3MemRead(pUVM, 0 /*idCpu*/,
                               DBGFR3AddrFromFlat(pUVM, &Addr, (RTGCPTR)SymInfo.Value + pData->AddrKernelBase.FlatPtr),
                               aSymbols[i].pvVar,  aSymbols[i].cbGuest);
            if (RT_SUCCESS(rc))
                continue;
            Log(("dbgDiggerLinuxIDmsg_QueryKernelLog: Reading '%s' at %RGv: %Rrc\n", aSymbols[i].pszSymbol, Addr.FlatPtr, rc));
        }
        else
            Log(("dbgDiggerLinuxIDmsg_QueryKernelLog: Error looking up '%s': %Rrc\n", aSymbols[i].pszSymbol, rc));
        RTDbgModRelease(hMod);
        return VERR_NOT_FOUND;
    }

    /*
     * Check if the values make sense.
     */
    if (pData->f64Bit ? !LNX64_VALID_ADDRESS(GCPtrLogBuf) : !LNX32_VALID_ADDRESS(GCPtrLogBuf))
    {
        Log(("dbgDiggerLinuxIDmsg_QueryKernelLog: 'log_buf' value %RGv is not valid.\n", GCPtrLogBuf));
        return VERR_NOT_FOUND;
    }
    if (   cbLogBuf < 4096
        || !RT_IS_POWER_OF_TWO(cbLogBuf)
        || cbLogBuf > 16*_1M)
    {
        Log(("dbgDiggerLinuxIDmsg_QueryKernelLog: 'log_buf_len' value %#x is not valid.\n", cbLogBuf));
        return VERR_NOT_FOUND;
    }
    uint32_t const cbLogAlign = 4;
    if (   idxFirst > cbLogBuf - sizeof(LNXPRINTKHDR)
        || (idxFirst & (cbLogAlign - 1)) != 0)
    {
        Log(("dbgDiggerLinuxIDmsg_QueryKernelLog: 'log_first_idx' value %#x is not valid.\n", idxFirst));
        return VERR_NOT_FOUND;
    }
    if (   idxNext > cbLogBuf - sizeof(LNXPRINTKHDR)
        || (idxNext & (cbLogAlign - 1)) != 0)
    {
        Log(("dbgDiggerLinuxIDmsg_QueryKernelLog: 'log_next_idx' value %#x is not valid.\n", idxNext));
        return VERR_NOT_FOUND;
    }

    /*
     * Read the whole log buffer.
     */
    uint8_t *pbLogBuf = (uint8_t *)RTMemAlloc(cbLogBuf);
    if (!pbLogBuf)
    {
        Log(("dbgDiggerLinuxIDmsg_QueryKernelLog: Failed to allocate %#x bytes for log buffer\n", cbLogBuf));
        return VERR_NO_MEMORY;
    }
    DBGFADDRESS Addr;
    rc = DBGFR3MemRead(pUVM, 0 /*idCpu*/, DBGFR3AddrFromFlat(pUVM, &Addr, GCPtrLogBuf), pbLogBuf, cbLogBuf);
    if (RT_FAILURE(rc))
    {
        Log(("dbgDiggerLinuxIDmsg_QueryKernelLog: Error reading %#x bytes of log buffer at %RGv: %Rrc\n",
             cbLogBuf, Addr.FlatPtr, rc));
        RTMemFree(pbLogBuf);
        return VERR_NOT_FOUND;
    }

    /*
     * Count the messages in the buffer while doing some basic validation.
     */
    uint32_t const cbUsed = idxFirst == idxNext ? cbLogBuf /* could be empty... */
                          : idxFirst < idxNext  ? idxNext - idxFirst : cbLogBuf - idxFirst + idxNext;
    uint32_t cbLeft    = cbUsed;
    uint32_t offCur    = idxFirst;
    uint32_t cLogMsgs  = 0;

    while (cbLeft > 0)
    {
        PCLNXPRINTKHDR pHdr = (PCLNXPRINTKHDR)&pbLogBuf[offCur];
        if (!pHdr->cbTotal)
        {
            /* Wrap around packet, most likely... */
            if (cbLogBuf - offCur >= cbLeft)
                break;
            offCur = 0;
            pHdr = (PCLNXPRINTKHDR)&pbLogBuf[offCur];
        }
        if (RT_UNLIKELY(   pHdr->cbTotal > cbLogBuf - sizeof(*pHdr) - offCur
                        || pHdr->cbTotal > cbLeft
                        || (pHdr->cbTotal & (cbLogAlign - 1)) != 0
                        || pHdr->cbTotal < (uint32_t)pHdr->cbText + (uint32_t)pHdr->cbDict + sizeof(*pHdr) ))
        {
            Log(("dbgDiggerLinuxIDmsg_QueryKernelLog: Invalid printk_log record at %#x: cbTotal=%#x cbText=%#x cbDict=%#x cbLogBuf=%#x cbLeft=%#x\n",
                 offCur, pHdr->cbTotal, pHdr->cbText, pHdr->cbDict, cbLogBuf, cbLeft));
            rc = VERR_INVALID_STATE;
            break;
        }

        if (pHdr->cbText > 0)
            cLogMsgs++;

        /* next */
        offCur += pHdr->cbTotal;
        cbLeft -= pHdr->cbTotal;
    }
    if (RT_FAILURE(rc))
    {
        RTMemFree(pbLogBuf);
        return rc;
    }

    /*
     * Copy the messages into the output buffer.
     */
    offCur = idxFirst;
    cbLeft = cbUsed;

    /* Skip messages that the caller doesn't want. */
    if (cMessages < cLogMsgs)
    {
        uint32_t cToSkip = cLogMsgs - cMessages;
        while (cToSkip > 0)
        {
            PCLNXPRINTKHDR pHdr = (PCLNXPRINTKHDR)&pbLogBuf[offCur];
            if (!pHdr->cbTotal)
            {
                offCur = 0;
                pHdr = (PCLNXPRINTKHDR)&pbLogBuf[offCur];
            }
            if (pHdr->cbText > 0)
                cToSkip--;

            /* next */
            offCur += pHdr->cbTotal;
            cbLeft -= pHdr->cbTotal;
        }
    }

    /* Now copy the messages. */
    size_t offDst = 0;
    while (cbLeft > 0)
    {
        PCLNXPRINTKHDR pHdr = (PCLNXPRINTKHDR)&pbLogBuf[offCur];
        if (!pHdr->cbTotal)
        {
            if (cbLogBuf - offCur >= cbLeft)
                break;
            offCur = 0;
            pHdr = (PCLNXPRINTKHDR)&pbLogBuf[offCur];
        }

        if (pHdr->cbText > 0)
        {
            char  *pchText = (char *)(pHdr + 1);
            size_t cchText = RTStrNLen(pchText, pHdr->cbText);
            if (offDst + cchText < cbBuf)
            {
                memcpy(&pszBuf[offDst], pHdr + 1, cchText);
                pszBuf[offDst + cchText] = '\n';
            }
            else if (offDst < cbBuf)
                memcpy(&pszBuf[offDst], pHdr + 1, cbBuf - offDst);
            offDst += cchText + 1;
        }

        /* next */
        offCur += pHdr->cbTotal;
        cbLeft -= pHdr->cbTotal;
    }

    /* Done with the buffer. */
    RTMemFree(pbLogBuf);

    /* Make sure we've reserved a char for the terminator. */
    if (!offDst)
        offDst = 1;

    /* Set return size value. */
    if (pcbActual)
        *pcbActual = offDst;

    /*
     * All VBox strings are UTF-8 and bad things may in theory happen if we
     * pass bad UTF-8 to code which assumes it's all valid.  So, we enforce
     * UTF-8 upon the guest kernel messages here even if they (probably) have
     * no defined code set in reality.
     */
    if (offDst <= cbBuf)
    {
        pszBuf[offDst - 1] = '\0';
        RTStrPurgeEncoding(pszBuf);
        return VINF_SUCCESS;
    }

    if (cbBuf)
    {
        pszBuf[cbBuf - 1] = '\0';
        RTStrPurgeEncoding(pszBuf);
    }
    return VERR_BUFFER_OVERFLOW;
}


/**
 * @copydoc DBGFOSREG::pfnQueryInterface
 */
static DECLCALLBACK(void *) dbgDiggerLinuxQueryInterface(PUVM pUVM, void *pvData, DBGFOSINTERFACE enmIf)
{
    PDBGDIGGERLINUX pThis = (PDBGDIGGERLINUX)pvData;
    switch (enmIf)
    {
        case DBGFOSINTERFACE_DMESG:
            return &pThis->IDmesg;

        default:
            return NULL;
    }
}


/**
 * @copydoc DBGFOSREG::pfnQueryVersion
 */
static DECLCALLBACK(int)  dbgDiggerLinuxQueryVersion(PUVM pUVM, void *pvData, char *pszVersion, size_t cchVersion)
{
    PDBGDIGGERLINUX pThis = (PDBGDIGGERLINUX)pvData;
    Assert(pThis->fValid);

    /*
     * It's all in the linux banner.
     */
    int rc = DBGFR3MemReadString(pUVM, 0, &pThis->AddrLinuxBanner, pszVersion, cchVersion);
    if (RT_SUCCESS(rc))
    {
        char *pszEnd = RTStrEnd(pszVersion, cchVersion);
        AssertReturn(pszEnd, VERR_BUFFER_OVERFLOW);
        while (     pszEnd > pszVersion
               &&   RT_C_IS_SPACE(pszEnd[-1]))
            pszEnd--;
        *pszEnd = '\0';
    }
    else
        RTStrPrintf(pszVersion, cchVersion, "DBGFR3MemRead -> %Rrc", rc);

    return rc;
}


/**
 * @copydoc DBGFOSREG::pfnTerm
 */
static DECLCALLBACK(void)  dbgDiggerLinuxTerm(PUVM pUVM, void *pvData)
{
    PDBGDIGGERLINUX pThis = (PDBGDIGGERLINUX)pvData;
    Assert(pThis->fValid);

    pThis->fValid = false;
}


/**
 * @copydoc DBGFOSREG::pfnRefresh
 */
static DECLCALLBACK(int)  dbgDiggerLinuxRefresh(PUVM pUVM, void *pvData)
{
    PDBGDIGGERLINUX pThis = (PDBGDIGGERLINUX)pvData;
    NOREF(pThis);
    Assert(pThis->fValid);

    /*
     * For now we'll flush and reload everything.
     */
    dbgDiggerLinuxTerm(pUVM, pvData);
    return dbgDiggerLinuxInit(pUVM, pvData);
}


/**
 * Worker for dbgDiggerLinuxFindStartOfNamesAndSymbolCount that update the
 * digger data.
 *
 * @returns VINF_SUCCESS.
 * @param   pThis               The Linux digger data to update.
 * @param   pAddrKernelNames    The kallsyms_names address.
 * @param   cKernelSymbols      The number of kernel symbol.
 * @param   cbAddress           The guest address size.
 */
static int dbgDiggerLinuxFoundStartOfNames(PDBGDIGGERLINUX pThis, PCDBGFADDRESS pAddrKernelNames,
                                           uint32_t cKernelSymbols, uint32_t cbAddress)
{
    pThis->cKernelSymbols = cKernelSymbols;
    pThis->AddrKernelNames = *pAddrKernelNames;
    pThis->AddrKernelAddresses = *pAddrKernelNames;
    DBGFR3AddrSub(&pThis->AddrKernelAddresses, (cKernelSymbols + 1) * cbAddress);

    Log(("dbgDiggerLinuxFoundStartOfNames: AddrKernelAddresses=%RGv\n"
         "dbgDiggerLinuxFoundStartOfNames: cKernelSymbols=%#x (at %RGv)\n"
         "dbgDiggerLinuxFoundStartOfNames: AddrKernelName=%RGv\n",
         pThis->AddrKernelAddresses.FlatPtr,
         pThis->cKernelSymbols, pThis->AddrKernelNames.FlatPtr - cbAddress,
         pThis->AddrKernelNames.FlatPtr));
    return VINF_SUCCESS;
}


/**
 * Tries to find the address of the kallsyms_names, kallsyms_num_syms and
 * kallsyms_addresses symbols.
 *
 * The kallsyms_num_syms is read and stored in pThis->cKernelSymbols, while the
 * addresses of the other two are stored as pThis->AddrKernelNames and
 * pThis->AddrKernelAddresses.
 *
 * @returns VBox status code, success indicating that all three variables have
 *          been found and taken down.
 * @param   pUVM                The user mode VM handle.
 * @param   pThis               The Linux digger data.
 * @param   pHitAddr            An address we think is inside kallsyms_names.
 */
static int dbgDiggerLinuxFindStartOfNamesAndSymbolCount(PUVM pUVM, PDBGDIGGERLINUX pThis, PCDBGFADDRESS pHitAddr)
{
    /*
     * Search backwards in chunks.
     */
    union
    {
        uint8_t  ab[0x1000];
        uint32_t au32[0x1000 / sizeof(uint32_t)];
        uint64_t au64[0x1000 / sizeof(uint64_t)];
    } uBuf;
    uint32_t        cbLeft  = LNX_MAX_KALLSYMS_NAMES_SIZE;
    uint32_t        cbBuf   = pHitAddr->FlatPtr & (sizeof(uBuf) - 1);
    DBGFADDRESS     CurAddr = *pHitAddr;
    DBGFR3AddrSub(&CurAddr, cbBuf);
    cbBuf += sizeof(uint64_t) - 1;      /* In case our kobj hit is in the first 4/8 bytes. */
    for (;;)
    {
        int rc = DBGFR3MemRead(pUVM, 0 /*idCpu*/, &CurAddr, &uBuf, sizeof(uBuf));
        if (RT_FAILURE(rc))
            return rc;

        /*
         * We assume that the three symbols are aligned on guest pointer boundrary.
         *
         * The boundrary between the two tables should be noticable as the number
         * is unlikely to be more than 16 millions, there will be at least one zero
         * byte where it is, 64-bit will have 5 zero bytes.  Zero bytes aren't all
         * that common in the kallsyms_names table.
         *
         * Also the kallsyms_names table starts with a length byte, which means
         * we're likely to see a byte in the range 1..31.
         *
         * The kallsyms_addresses are mostly sorted (except for the start where the
         * absolute symbols are), so we'll spot a bunch of kernel addresses
         * immediately preceeding the kallsyms_num_syms field.
         *
         * Lazy bird: If kallsyms_num_syms is on a buffer boundrary, we skip
         *            the check for kernel addresses preceeding it.
         */
        if (pThis->f64Bit)
        {
            uint32_t i = cbBuf / sizeof(uint64_t);
            while (i-- > 0)
                if (   uBuf.au64[i] <= LNX_MAX_KALLSYMS_SYMBOLS
                    && uBuf.au64[i] >= LNX_MIN_KALLSYMS_SYMBOLS)
                {
                    uint8_t *pb = (uint8_t *)&uBuf.au64[i + 1];
                    if (   pb[0] <= LNX_MAX_KALLSYMS_ENC_LENGTH
                        && pb[0] >= LNX_MIN_KALLSYMS_ENC_LENGTH)
                    {
                        if (   (i <= 0 || LNX64_VALID_ADDRESS(uBuf.au64[i - 1]))
                            && (i <= 1 || LNX64_VALID_ADDRESS(uBuf.au64[i - 2]))
                            && (i <= 2 || LNX64_VALID_ADDRESS(uBuf.au64[i - 3])))
                            return dbgDiggerLinuxFoundStartOfNames(pThis,
                                                                   DBGFR3AddrAdd(&CurAddr, (i + 1) * sizeof(uint64_t)),
                                                                   (uint32_t)uBuf.au64[i], sizeof(uint64_t));
                    }
                }
        }
        else
        {
            uint32_t i = cbBuf / sizeof(uint32_t);
            while (i-- > 0)
                if (   uBuf.au32[i] <= LNX_MAX_KALLSYMS_SYMBOLS
                    && uBuf.au32[i] >= LNX_MIN_KALLSYMS_SYMBOLS)
                {
                    uint8_t *pb = (uint8_t *)&uBuf.au32[i + 1];
                    if (   pb[0] <= LNX_MAX_KALLSYMS_ENC_LENGTH
                        && pb[0] >= LNX_MIN_KALLSYMS_ENC_LENGTH)
                    {
                        if (   (i <= 0 || LNX32_VALID_ADDRESS(uBuf.au32[i - 1]))
                            && (i <= 1 || LNX32_VALID_ADDRESS(uBuf.au32[i - 2]))
                            && (i <= 2 || LNX32_VALID_ADDRESS(uBuf.au32[i - 3])))
                            return dbgDiggerLinuxFoundStartOfNames(pThis,
                                                                   DBGFR3AddrAdd(&CurAddr, (i + 1) * sizeof(uint32_t)),
                                                                   uBuf.au32[i], sizeof(uint32_t));
                    }
                }
        }

        /*
         * Advance
         */
        if (RT_UNLIKELY(cbLeft <= sizeof(uBuf)))
        {
            Log(("dbgDiggerLinuxFindStartOfNamesAndSymbolCount: failed (pHitAddr=%RGv)\n", pHitAddr->FlatPtr));
            return VERR_NOT_FOUND;
        }
        cbLeft -= sizeof(uBuf);
        DBGFR3AddrSub(&CurAddr, sizeof(uBuf));
        cbBuf = sizeof(uBuf);
    }
}


/**
 * Worker for dbgDiggerLinuxFindEndNames that records the findings.
 *
 * @returns VINF_SUCCESS
 * @param   pThis           The linux digger data to update.
 * @param   pAddrMarkers    The address of the marker (kallsyms_markers).
 * @param   cbMarkerEntry   The size of a marker entry (32-bit or 64-bit).
 */
static int dbgDiggerLinuxFoundMarkers(PDBGDIGGERLINUX pThis, PCDBGFADDRESS pAddrMarkers, uint32_t cbMarkerEntry)
{
    pThis->cbKernelNames         = pAddrMarkers->FlatPtr - pThis->AddrKernelNames.FlatPtr - 1;
    pThis->AddrKernelNameMarkers = *pAddrMarkers;
    pThis->cKernelNameMarkers    = RT_ALIGN_32(pThis->cKernelSymbols, 256) / 256;
    pThis->AddrKernelTokenTable  = *pAddrMarkers;
    DBGFR3AddrAdd(&pThis->AddrKernelTokenTable, pThis->cKernelNameMarkers * cbMarkerEntry);

    Log(("dbgDiggerLinuxFoundMarkers: AddrKernelNames=%RGv cbKernelNames=%#x\n"
         "dbgDiggerLinuxFoundMarkers: AddrKernelNameMarkers=%RGv cKernelNameMarkers=%#x\n"
         "dbgDiggerLinuxFoundMarkers: AddrKernelTokenTable=%RGv\n",
         pThis->AddrKernelNames.FlatPtr, pThis->cbKernelNames,
         pThis->AddrKernelNameMarkers.FlatPtr, pThis->cKernelNameMarkers,
         pThis->AddrKernelTokenTable.FlatPtr));
    return VINF_SUCCESS;
}


/**
 * Tries to find the end of kallsyms_names and thereby the start of
 * kallsyms_markers and kallsyms_token_table.
 *
 * The kallsyms_names size is stored in pThis->cbKernelNames, the addresses of
 * the two other symbols in pThis->AddrKernelNameMarkers and
 * pThis->AddrKernelTokenTable.  The number of marker entries is stored in
 * pThis->cKernelNameMarkers.
 *
 * @returns VBox status code, success indicating that all three variables have
 *          been found and taken down.
 * @param   pUVM                The user mode VM handle.
 * @param   pThis               The Linux digger data.
 * @param   pHitAddr            An address we think is inside kallsyms_names.
 */
static int dbgDiggerLinuxFindEndOfNamesAndMore(PUVM pUVM, PDBGDIGGERLINUX pThis, PCDBGFADDRESS pHitAddr)
{
    /*
     * Search forward in chunks.
     */
    union
    {
        uint8_t  ab[0x1000];
        uint32_t au32[0x1000 / sizeof(uint32_t)];
        uint64_t au64[0x1000 / sizeof(uint64_t)];
    } uBuf;
    bool            fPendingZeroHit = false;
    uint32_t        cbLeft  = LNX_MAX_KALLSYMS_NAMES_SIZE + sizeof(uBuf);
    uint32_t        offBuf  = pHitAddr->FlatPtr & (sizeof(uBuf) - 1);
    DBGFADDRESS     CurAddr = *pHitAddr;
    DBGFR3AddrSub(&CurAddr, offBuf);
    for (;;)
    {
        int rc = DBGFR3MemRead(pUVM, 0 /*idCpu*/, &CurAddr, &uBuf, sizeof(uBuf));
        if (RT_FAILURE(rc))
            return rc;

        /*
         * The kallsyms_names table is followed by kallsyms_markers we assume,
         * using sizeof(unsigned long) alignment like the preceeding symbols.
         *
         * The kallsyms_markers table has entried sizeof(unsigned long) and
         * contains offsets into kallsyms_names.  The kallsyms_markers used to
         * index kallsyms_names and reduce seek time when looking up the name
         * of an address/symbol.  Each entry in kallsyms_markers covers 256
         * symbol names.
         *
         * Because of this, the first entry is always zero and all the entries
         * are ascending.  It also follows that the size of the table can be
         * calculated from kallsyms_num_syms.
         *
         * Note! We could also have walked kallsyms_names by skipping
         *       kallsyms_num_syms names, but this is faster and we will
         *       validate the encoded names later.
         */
        if (pThis->f64Bit)
        {
            if (   RT_UNLIKELY(fPendingZeroHit)
                && uBuf.au64[0] >= (LNX_MIN_KALLSYMS_ENC_LENGTH + 1) * 256
                && uBuf.au64[0] <= (LNX_MAX_KALLSYMS_ENC_LENGTH + 1) * 256)
                return dbgDiggerLinuxFoundMarkers(pThis, DBGFR3AddrSub(&CurAddr, sizeof(uint64_t)), sizeof(uint64_t));

            uint32_t const cEntries = sizeof(uBuf) / sizeof(uint64_t);
            for (uint32_t i = offBuf / sizeof(uint64_t); i < cEntries; i++)
                if (uBuf.au64[i] == 0)
                {
                    if (RT_UNLIKELY(i + 1 >= cEntries))
                    {
                        fPendingZeroHit = true;
                        break;
                    }
                    if (   uBuf.au64[i + 1] >= (LNX_MIN_KALLSYMS_ENC_LENGTH + 1) * 256
                        && uBuf.au64[i + 1] <= (LNX_MAX_KALLSYMS_ENC_LENGTH + 1) * 256)
                        return dbgDiggerLinuxFoundMarkers(pThis, DBGFR3AddrAdd(&CurAddr, i * sizeof(uint64_t)), sizeof(uint64_t));
                }
        }
        else
        {
            if (   RT_UNLIKELY(fPendingZeroHit)
                && uBuf.au32[0] >= (LNX_MIN_KALLSYMS_ENC_LENGTH + 1) * 256
                && uBuf.au32[0] <= (LNX_MAX_KALLSYMS_ENC_LENGTH + 1) * 256)
                return dbgDiggerLinuxFoundMarkers(pThis, DBGFR3AddrSub(&CurAddr, sizeof(uint32_t)), sizeof(uint32_t));

            uint32_t const cEntries = sizeof(uBuf) / sizeof(uint32_t);
            for (uint32_t i = offBuf / sizeof(uint32_t); i < cEntries; i++)
                if (uBuf.au32[i] == 0)
                {
                    if (RT_UNLIKELY(i + 1 >= cEntries))
                    {
                        fPendingZeroHit = true;
                        break;
                    }
                    if (   uBuf.au32[i + 1] >= (LNX_MIN_KALLSYMS_ENC_LENGTH + 1) * 256
                        && uBuf.au32[i + 1] <= (LNX_MAX_KALLSYMS_ENC_LENGTH + 1) * 256)
                        return dbgDiggerLinuxFoundMarkers(pThis, DBGFR3AddrAdd(&CurAddr, i * sizeof(uint32_t)), sizeof(uint32_t));
                }
        }

        /*
         * Advance
         */
        if (RT_UNLIKELY(cbLeft <= sizeof(uBuf)))
        {
            Log(("dbgDiggerLinuxFindEndOfNamesAndMore: failed (pHitAddr=%RGv)\n", pHitAddr->FlatPtr));
            return VERR_NOT_FOUND;
        }
        cbLeft -= sizeof(uBuf);
        DBGFR3AddrAdd(&CurAddr, sizeof(uBuf));
        offBuf = 0;
    }
}


/**
 * Locates the kallsyms_token_index table.
 *
 * Storing the address in pThis->AddrKernelTokenIndex and the size of the token
 * table in pThis->cbKernelTokenTable.
 *
 * @returns VBox status code.
 * @param   pUVM                The user mode VM handle.
 * @param   pThis               The Linux digger data.
 */
static int dbgDiggerLinuxFindTokenIndex(PUVM pUVM, PDBGDIGGERLINUX pThis)
{
    /*
     * The kallsyms_token_table is very much like a string table.  Due to the
     * nature of the compression algorithm it is reasonably short (one example
     * here is 853 bytes), so we'll not be reading it in chunks but in full.
     * To be on the safe side, we read 8KB, ASSUMING we won't run into unmapped
     * memory or any other nasty stuff...
     */
    union
    {
        uint8_t  ab[0x2000];
        uint16_t au16[0x2000 / sizeof(uint16_t)];
    } uBuf;
    DBGFADDRESS CurAddr = pThis->AddrKernelTokenTable;
    int rc = DBGFR3MemRead(pUVM, 0 /*idCpu*/, &CurAddr, &uBuf, sizeof(uBuf));
    if (RT_FAILURE(rc))
        return rc;

    /*
     * We've got two choices here, either walk the string table or look for
     * the next structure, kallsyms_token_index.
     *
     * The token index is a table of 256 uint16_t entries (index by bytes
     * from kallsyms_names) that gives offsets in kallsyms_token_table.  It
     * starts with a zero entry and the following entries are sorted in
     * ascending order.  The range of the entries are reasonably small since
     * kallsyms_token_table is small.
     *
     * The alignment seems to be sizeof(unsigned long), just like
     * kallsyms_token_table.
     *
     * So, we start by looking for a zero 16-bit entry.
     */
    uint32_t cIncr = (pThis->f64Bit ? sizeof(uint64_t) : sizeof(uint32_t)) / sizeof(uint16_t);

    for (uint32_t i = 0; i < sizeof(uBuf) / sizeof(uint16_t) - 16; i += cIncr)
        if (   uBuf.au16[i] == 0
            && uBuf.au16[i + 1] >  0
            && uBuf.au16[i + 1] <= LNX_MAX_KALLSYMS_TOKEN_LEN
            && (uint16_t)(uBuf.au16[i + 2] - uBuf.au16[i + 1] - 1U) <= (uint16_t)LNX_MAX_KALLSYMS_TOKEN_LEN
            && (uint16_t)(uBuf.au16[i + 3] - uBuf.au16[i + 2] - 1U) <= (uint16_t)LNX_MAX_KALLSYMS_TOKEN_LEN
            && (uint16_t)(uBuf.au16[i + 4] - uBuf.au16[i + 3] - 1U) <= (uint16_t)LNX_MAX_KALLSYMS_TOKEN_LEN
            && (uint16_t)(uBuf.au16[i + 5] - uBuf.au16[i + 4] - 1U) <= (uint16_t)LNX_MAX_KALLSYMS_TOKEN_LEN
            && (uint16_t)(uBuf.au16[i + 6] - uBuf.au16[i + 5] - 1U) <= (uint16_t)LNX_MAX_KALLSYMS_TOKEN_LEN
            )
        {
            pThis->AddrKernelTokenIndex = CurAddr;
            DBGFR3AddrAdd(&pThis->AddrKernelTokenIndex, i * sizeof(uint16_t));
            pThis->cbKernelTokenTable = i * sizeof(uint16_t);
            return VINF_SUCCESS;
        }

    Log(("dbgDiggerLinuxFindTokenIndex: Failed (%RGv..%RGv)\n", CurAddr.FlatPtr, CurAddr.FlatPtr + (RTGCUINTPTR)sizeof(uBuf)));
    return VERR_NOT_FOUND;
}


/**
 * Loads the kernel symbols from the kallsyms tables.
 *
 * @returns VBox status code.
 * @param   pUVM                The user mode VM handle.
 * @param   pThis               The Linux digger data.
 */
static int dbgDiggerLinuxLoadKernelSymbols(PUVM pUVM, PDBGDIGGERLINUX pThis)
{
    /*
     * Allocate memory for temporary table copies, reading the tables as we go.
     */
    uint32_t const cbGuestAddr = pThis->f64Bit ? sizeof(uint64_t) : sizeof(uint32_t);
    void *pvAddresses = RTMemAllocZ(pThis->cKernelSymbols * cbGuestAddr);
    int rc = DBGFR3MemRead(pUVM, 0 /*idCpu*/, &pThis->AddrKernelAddresses, pvAddresses, pThis->cKernelSymbols * cbGuestAddr);
    if (RT_SUCCESS(rc))
    {
        uint8_t *pbNames = (uint8_t *)RTMemAllocZ(pThis->cbKernelNames);
        rc = DBGFR3MemRead(pUVM, 0 /*idCpu*/, &pThis->AddrKernelNames, pbNames, pThis->cbKernelNames);
        if (RT_SUCCESS(rc))
        {
            char *pszzTokens = (char *)RTMemAllocZ(pThis->cbKernelTokenTable);
            rc = DBGFR3MemRead(pUVM, 0 /*idCpu*/, &pThis->AddrKernelTokenTable, pszzTokens, pThis->cbKernelTokenTable);
            if (RT_SUCCESS(rc))
            {
                uint16_t *paoffTokens = (uint16_t *)RTMemAllocZ(256 * sizeof(uint16_t));
                rc = DBGFR3MemRead(pUVM, 0 /*idCpu*/, &pThis->AddrKernelTokenIndex, paoffTokens, 256 * sizeof(uint16_t));
                if (RT_SUCCESS(rc))
                {
                    /*
                     * Figure out the kernel start and end.
                     */
                    RTGCUINTPTR uKernelStart = pThis->AddrKernelAddresses.FlatPtr;
                    RTGCUINTPTR uKernelEnd   = pThis->AddrKernelTokenIndex.FlatPtr + 256 * sizeof(uint16_t);
                    uint32_t    i;
                    if (cbGuestAddr == sizeof(uint64_t))
                    {
                        uint64_t *pauAddrs = (uint64_t *)pvAddresses;
                        for (i = 0; i < pThis->cKernelSymbols; i++)
                            if (   pauAddrs[i] < uKernelStart
                                && LNX64_VALID_ADDRESS(pauAddrs[i])
                                && uKernelStart - pauAddrs[i] < LNX_MAX_KERNEL_SIZE)
                                uKernelStart = pauAddrs[i];

                        for (i = pThis->cKernelSymbols - 1; i > 0; i--)
                            if (   pauAddrs[i] > uKernelEnd
                                && LNX64_VALID_ADDRESS(pauAddrs[i])
                                && pauAddrs[i] - uKernelEnd < LNX_MAX_KERNEL_SIZE)
                                uKernelEnd = pauAddrs[i];
                    }
                    else
                    {
                        uint32_t *pauAddrs = (uint32_t *)pvAddresses;
                        for (i = 0; i < pThis->cKernelSymbols; i++)
                            if (   pauAddrs[i] < uKernelStart
                                && LNX32_VALID_ADDRESS(pauAddrs[i])
                                && uKernelStart - pauAddrs[i] < LNX_MAX_KERNEL_SIZE)
                                uKernelStart = pauAddrs[i];

                        for (i = pThis->cKernelSymbols - 1; i > 0; i--)
                            if (   pauAddrs[i] > uKernelEnd
                                && LNX32_VALID_ADDRESS(pauAddrs[i])
                                && pauAddrs[i] - uKernelEnd < LNX_MAX_KERNEL_SIZE)
                                uKernelEnd = pauAddrs[i];
                    }

                    RTGCUINTPTR cbKernel = uKernelEnd - uKernelStart;
                    pThis->cbKernel = (uint32_t)cbKernel;
                    DBGFR3AddrFromFlat(pUVM, &pThis->AddrKernelBase, uKernelStart);
                    Log(("dbgDiggerLinuxLoadKernelSymbols: uKernelStart=%RGv cbKernel=%#x\n", uKernelStart, cbKernel));

                    /*
                     * Create a module for the kernel.
                     */
                    RTDBGMOD hMod;
                    rc = RTDbgModCreate(&hMod, "vmlinux", cbKernel, 0 /*fFlags*/);
                    if (RT_SUCCESS(rc))
                    {
                        rc = RTDbgModSetTag(hMod, DIG_LNX_MOD_TAG); AssertRC(rc);
                        rc = VINF_SUCCESS;

                        /*
                         * Enumerate the symbols.
                         */
                        uint8_t const  *pbCurAddr = (uint8_t const *)pvAddresses;
                        uint32_t        offName   = 0;
                        uint32_t        cLeft = pThis->cKernelSymbols;
                        while (cLeft-- > 0 && RT_SUCCESS(rc))
                        {
                            /* Decode the symbol name first. */
                            if (RT_LIKELY(offName < pThis->cbKernelNames))
                            {
                                uint8_t cbName = pbNames[offName++];
                                if (RT_LIKELY(offName + cbName <= pThis->cbKernelNames))
                                {
                                    char     szSymbol[4096];
                                    uint32_t offSymbol = 0;
                                    while (cbName-- > 0)
                                    {
                                        uint8_t  bEnc     = pbNames[offName++];
                                        uint16_t offToken = paoffTokens[bEnc];
                                        if (RT_LIKELY(offToken < pThis->cbKernelTokenTable))
                                        {
                                            const char *pszToken = &pszzTokens[offToken];
                                            char ch;
                                            while ((ch = *pszToken++) != '\0')
                                                if (offSymbol < sizeof(szSymbol) - 1)
                                                    szSymbol[offSymbol++] = ch;
                                        }
                                        else
                                        {
                                            rc = VERR_INVALID_UTF8_ENCODING;
                                            break;
                                        }
                                    }
                                    szSymbol[offSymbol < sizeof(szSymbol) ? offSymbol : sizeof(szSymbol) - 1] = '\0';

                                    /* The address. */
                                    RTGCUINTPTR uSymAddr = cbGuestAddr == sizeof(uint64_t)
                                                         ? *(uint64_t *)pbCurAddr : *(uint32_t *)pbCurAddr;
                                    pbCurAddr += cbGuestAddr;

                                    /* Add it without the type char. */
                                    if (uSymAddr - uKernelStart <= cbKernel)
                                    {
                                        rc = RTDbgModSymbolAdd(hMod, &szSymbol[1], RTDBGSEGIDX_RVA, uSymAddr - uKernelStart,
                                                               0 /*cb*/, 0 /*fFlags*/, NULL);
                                        if (RT_FAILURE(rc))
                                        {
                                            if (   rc == VERR_DBG_SYMBOL_NAME_OUT_OF_RANGE
                                                || rc == VERR_DBG_INVALID_RVA
                                                || rc == VERR_DBG_ADDRESS_CONFLICT
                                                || rc == VERR_DBG_DUPLICATE_SYMBOL)
                                            {
                                                Log2(("dbgDiggerLinuxLoadKernelSymbols: RTDbgModSymbolAdd(,%s,) failed %Rrc (ignored)\n", szSymbol, rc));
                                                rc = VINF_SUCCESS;
                                            }
                                            else
                                                Log(("dbgDiggerLinuxLoadKernelSymbols: RTDbgModSymbolAdd(,%s,) failed %Rrc\n", szSymbol, rc));
                                        }
                                    }
                                }
                                else
                                {
                                    rc = VERR_END_OF_STRING;
                                    Log(("dbgDiggerLinuxLoadKernelSymbols: offName=%#x cLeft=%#x cbName=%#x cbKernelNames=%#x\n",
                                         offName, cLeft, cbName, pThis->cbKernelNames));
                                }
                            }
                            else
                            {
                                rc = VERR_END_OF_STRING;
                                Log(("dbgDiggerLinuxLoadKernelSymbols: offName=%#x cLeft=%#x cbKernelNames=%#x\n",
                                     offName, cLeft, pThis->cbKernelNames));
                            }
                        }

                        /*
                         * Link the module into the address space.
                         */
                        if (RT_SUCCESS(rc))
                        {
                            RTDBGAS hAs = DBGFR3AsResolveAndRetain(pUVM, DBGF_AS_KERNEL);
                            if (hAs != NIL_RTDBGAS)
                                rc = RTDbgAsModuleLink(hAs, hMod, uKernelStart, RTDBGASLINK_FLAGS_REPLACE);
                            else
                                rc = VERR_INTERNAL_ERROR;
                            RTDbgAsRelease(hAs);
                        }
                        else
                            Log(("dbgDiggerLinuxFindTokenIndex: Failed: %Rrc\n", rc));
                        RTDbgModRelease(hMod);
                    }
                    else
                        Log(("dbgDiggerLinuxFindTokenIndex: RTDbgModCreate failed: %Rrc\n", rc));
                }
                else
                    Log(("dbgDiggerLinuxFindTokenIndex: Reading token index at %RGv failed: %Rrc\n",
                         pThis->AddrKernelTokenIndex.FlatPtr, rc));
                RTMemFree(paoffTokens);
            }
            else
                Log(("dbgDiggerLinuxFindTokenIndex: Reading token table at %RGv failed: %Rrc\n",
                     pThis->AddrKernelTokenTable.FlatPtr, rc));
            RTMemFree(pszzTokens);
        }
        else
            Log(("dbgDiggerLinuxFindTokenIndex: Reading encoded names at %RGv failed: %Rrc\n",
                 pThis->AddrKernelNames.FlatPtr, rc));
        RTMemFree(pbNames);
    }
    else
        Log(("dbgDiggerLinuxFindTokenIndex: Reading symbol addresses at %RGv failed: %Rrc\n",
             pThis->AddrKernelAddresses.FlatPtr, rc));
    RTMemFree(pvAddresses);
    return rc;
}


/**
 * Checks if there is a likely kallsyms_names fragment at pHitAddr.
 *
 * @returns true if it's a likely fragment, false if not.
 * @param   pUVM                The user mode VM handle.
 * @param   pHitAddr            The address where paNeedle was found.
 * @param   pabNeedle           The fragment we've been searching for.
 * @param   cbNeedle            The length of the fragment.
 */
static bool dbgDiggerLinuxIsLikelyNameFragment(PUVM pUVM, PCDBGFADDRESS pHitAddr, uint8_t const *pabNeedle, uint8_t cbNeedle)
{
    /*
     * Examples of lead and tail bytes of our choosen needle in a randomly
     * picked kernel:
     *         k  o  b  j
     *     22  6b 6f 62 6a  aa
     *     fc  6b 6f 62 6a  aa
     *     82  6b 6f 62 6a  5f      - ascii trail byte (_).
     *     ee  6b 6f 62 6a  aa
     *     fc  6b 6f 62 6a  5f      - ascii trail byte (_).
     *  0a 74  6b 6f 62 6a  5f ea   - ascii lead (t) and trail (_) bytes.
     *  0b 54  6b 6f 62 6a  aa      - ascii lead byte (T).
     * ... omitting 29 samples similar to the last two ...
     *     d8  6b 6f 62 6a  aa
     *     d8  6b 6f 62 6a  aa
     *     d8  6b 6f 62 6a  aa
     *     d8  6b 6f 62 6a  aa
     *  f9 5f  6b 6f 62 6a  5f 94   - ascii lead and trail bytes (_)
     *  f9 5f  6b 6f 62 6a  0c      - ascii lead byte (_).
     *     fd  6b 6f 62 6a  0f
     *  ... enough.
     */
    uint8_t         abBuf[32];
    DBGFADDRESS     ReadAddr = *pHitAddr;
    DBGFR3AddrSub(&ReadAddr, 2);
    int rc = DBGFR3MemRead(pUVM, 0 /*idCpu*/, &ReadAddr, abBuf, 2 + cbNeedle + 2);
    if (RT_SUCCESS(rc))
    {
        if (memcmp(&abBuf[2], pabNeedle, cbNeedle) == 0) /* paranoia */
        {
            uint8_t const bLead = abBuf[1] == '_' || abBuf[1] == 'T' || abBuf[1] == 't' ? abBuf[0] : abBuf[1];
            uint8_t const offTail = 2 + cbNeedle;
            uint8_t const bTail = abBuf[offTail] == '_' ? abBuf[offTail] : abBuf[offTail + 1];
            if (   bLead >= 1 && (bLead < 0x20 || bLead >= 0x80)
                && bTail >= 1 && (bTail < 0x20 || bTail >= 0x80))
                return true;
            Log(("dbgDiggerLinuxIsLikelyNameFragment: failed at %RGv: bLead=%#x bTail=%#x (offTail=%#x)\n",
                 pHitAddr->FlatPtr, bLead, bTail, offTail));
        }
        else
            Log(("dbgDiggerLinuxIsLikelyNameFragment: failed at %RGv: Needle changed!\n", pHitAddr->FlatPtr));
    }
    else
        Log(("dbgDiggerLinuxIsLikelyNameFragment: failed at %RGv: %Rrc\n", pHitAddr->FlatPtr, rc));

    return false;
}


/**
 * @copydoc DBGFOSREG::pfnInit
 */
static DECLCALLBACK(int)  dbgDiggerLinuxInit(PUVM pUVM, void *pvData)
{
    PDBGDIGGERLINUX pThis = (PDBGDIGGERLINUX)pvData;
    Assert(!pThis->fValid);

    /*
     * Assume 64-bit kernels all live way beyond 32-bit address space.
     */
    pThis->f64Bit = pThis->AddrLinuxBanner.FlatPtr > UINT32_MAX;

    /*
     * Go looking for the kallsyms table.  If it's there, it will be somewhere
     * after the linux_banner symbol, so use it for starting the search.
     */
    DBGFADDRESS CurAddr = pThis->AddrLinuxBanner;
    uint32_t    cbLeft  = LNX_MAX_KERNEL_SIZE;
    while (cbLeft > 4096)
    {
        static const uint8_t s_abNeedle[] = "kobj";
        DBGFADDRESS          HitAddr;
        int rc = DBGFR3MemScan(pUVM, 0 /*idCpu*/, &CurAddr, cbLeft, 1 /*uAlign*/,
                               s_abNeedle, sizeof(s_abNeedle) - 1, &HitAddr);
        if (RT_FAILURE(rc))
            break;
        if (dbgDiggerLinuxIsLikelyNameFragment(pUVM, &HitAddr, s_abNeedle, sizeof(s_abNeedle) - 1))
        {
            /* There will be another hit near by. */
            DBGFR3AddrAdd(&HitAddr, 1);
            rc = DBGFR3MemScan(pUVM, 0 /*idCpu*/, &HitAddr, LNX_MAX_KALLSYMS_NAMES_SIZE, 1 /*uAlign*/,
                               s_abNeedle, sizeof(s_abNeedle) - 1, &HitAddr);
            if (   RT_SUCCESS(rc)
                && dbgDiggerLinuxIsLikelyNameFragment(pUVM, &HitAddr, s_abNeedle, sizeof(s_abNeedle) - 1))
            {
                /*
                 * We've got a very likely candidate for a location inside kallsyms_names.
                 * Try find the start of it, that is to say, try find kallsyms_num_syms.
                 * kallsyms_num_syms is aligned on sizeof(unsigned long) boundrary
                 */
                rc = dbgDiggerLinuxFindStartOfNamesAndSymbolCount(pUVM, pThis, &HitAddr);
                if (RT_SUCCESS(rc))
                    rc = dbgDiggerLinuxFindEndOfNamesAndMore(pUVM, pThis, &HitAddr);
                if (RT_SUCCESS(rc))
                    rc = dbgDiggerLinuxFindTokenIndex(pUVM, pThis);
                if (RT_SUCCESS(rc))
                    rc = dbgDiggerLinuxLoadKernelSymbols(pUVM, pThis);
                if (RT_SUCCESS(rc))
                    break;
            }
        }

        /*
         * Advance.
         */
        RTGCUINTPTR cbDistance = HitAddr.FlatPtr - CurAddr.FlatPtr + sizeof(s_abNeedle) - 1;
        if (RT_UNLIKELY(cbDistance >= cbLeft))
        {
            Log(("dbgDiggerLinuxInit: Failed to find kallsyms\n"));
            break;
        }
        cbLeft -= cbDistance;
        DBGFR3AddrAdd(&CurAddr, cbDistance);

    }

    pThis->fValid = true;
    return VINF_SUCCESS;
}


/**
 * @copydoc DBGFOSREG::pfnProbe
 */
static DECLCALLBACK(bool)  dbgDiggerLinuxProbe(PUVM pUVM, void *pvData)
{
    PDBGDIGGERLINUX pThis = (PDBGDIGGERLINUX)pvData;

    /*
     * Look for "Linux version " at the start of the rodata segment.
     * Hope that this comes before any message buffer or other similar string.
     */
    for (unsigned i = 0; i < RT_ELEMENTS(g_au64LnxKernelAddresses); i++)
    {
        DBGFADDRESS KernelAddr;
        DBGFR3AddrFromFlat(pUVM, &KernelAddr, g_au64LnxKernelAddresses[i]);
        DBGFADDRESS HitAddr;
        static const uint8_t s_abLinuxVersion[] = "Linux version ";
        int rc = DBGFR3MemScan(pUVM, 0, &KernelAddr, LNX_MAX_KERNEL_SIZE, 1,
                               s_abLinuxVersion, sizeof(s_abLinuxVersion) - 1, &HitAddr);
        if (RT_SUCCESS(rc))
        {
            char szTmp[128];
            char const *pszX = &szTmp[sizeof(s_abLinuxVersion) - 1];
            rc = DBGFR3MemReadString(pUVM, 0, &HitAddr, szTmp, sizeof(szTmp));
            if (    RT_SUCCESS(rc)
                &&  (   (   pszX[0] == '2'  /* 2.x.y with x in {0..6} */
                         && pszX[1] == '.'
                         && pszX[2] >= '0'
                         && pszX[2] <= '6')
                     || (   pszX[0] >= '3'  /* 3.x, 4.x, ... 9.x */
                         && pszX[0] <= '9'
                         && pszX[1] == '.'
                         && pszX[2] >= '0'
                         && pszX[2] <= '9')
                     )
                )
            {
                pThis->AddrKernelBase  = KernelAddr;
                pThis->AddrLinuxBanner = HitAddr;
                return true;
            }
        }
    }
    return false;
}


/**
 * @copydoc DBGFOSREG::pfnDestruct
 */
static DECLCALLBACK(void)  dbgDiggerLinuxDestruct(PUVM pUVM, void *pvData)
{

}


/**
 * @copydoc DBGFOSREG::pfnConstruct
 */
static DECLCALLBACK(int)  dbgDiggerLinuxConstruct(PUVM pUVM, void *pvData)
{
    PDBGDIGGERLINUX pThis = (PDBGDIGGERLINUX)pvData;
    pThis->IDmesg.u32Magic = DBGFOSIDMESG_MAGIC;
    pThis->IDmesg.pfnQueryKernelLog = dbgDiggerLinuxIDmsg_QueryKernelLog;
    pThis->IDmesg.u32EndMagic = DBGFOSIDMESG_MAGIC;

    return VINF_SUCCESS;
}


const DBGFOSREG g_DBGDiggerLinux =
{
    /* .u32Magic = */           DBGFOSREG_MAGIC,
    /* .fFlags = */             0,
    /* .cbData = */             sizeof(DBGDIGGERLINUX),
    /* .szName = */             "Linux",
    /* .pfnConstruct = */       dbgDiggerLinuxConstruct,
    /* .pfnDestruct = */        dbgDiggerLinuxDestruct,
    /* .pfnProbe = */           dbgDiggerLinuxProbe,
    /* .pfnInit = */            dbgDiggerLinuxInit,
    /* .pfnRefresh = */         dbgDiggerLinuxRefresh,
    /* .pfnTerm = */            dbgDiggerLinuxTerm,
    /* .pfnQueryVersion = */    dbgDiggerLinuxQueryVersion,
    /* .pfnQueryInterface = */  dbgDiggerLinuxQueryInterface,
    /* .u32EndMagic = */        DBGFOSREG_MAGIC
};

