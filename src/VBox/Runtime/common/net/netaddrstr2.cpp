/* $Id: netaddrstr2.cpp $ */
/** @file
 * IPRT - Network Address String Handling.
 */

/*
 * Copyright (C) 2013-2015 Oracle Corporation
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
#include "internal/iprt.h"
#include <iprt/net.h>

#include <iprt/asm.h>
#include <iprt/mem.h>
#include <iprt/string.h>
#include <iprt/stream.h>
#include "internal/string.h"


DECLHIDDEN(int) rtNetStrToIPv4AddrEx(const char *pcszAddr, PRTNETADDRIPV4 pAddr,
                                     char **ppszNext)
{
    char *pszNext;
    int rc;

    AssertPtrReturn(pcszAddr, VERR_INVALID_PARAMETER);
    AssertPtrReturn(pAddr, VERR_INVALID_PARAMETER);

    rc = RTStrToUInt8Ex(pcszAddr, &pszNext, 10, &pAddr->au8[0]);
    if (rc != VINF_SUCCESS && rc != VWRN_TRAILING_CHARS)
        return VERR_INVALID_PARAMETER;
    if (*pszNext++ != '.')
        return VERR_INVALID_PARAMETER;

    rc = RTStrToUInt8Ex(pszNext, &pszNext, 10, &pAddr->au8[1]);
    if (rc != VINF_SUCCESS && rc != VWRN_TRAILING_CHARS)
        return VERR_INVALID_PARAMETER;
    if (*pszNext++ != '.')
        return VERR_INVALID_PARAMETER;

    rc = RTStrToUInt8Ex(pszNext, &pszNext, 10, &pAddr->au8[2]);
    if (rc != VINF_SUCCESS && rc != VWRN_TRAILING_CHARS)
        return VERR_INVALID_PARAMETER;
    if (*pszNext++ != '.')
        return VERR_INVALID_PARAMETER;

    rc = RTStrToUInt8Ex(pszNext, &pszNext, 10, &pAddr->au8[3]);
    if (rc != VINF_SUCCESS && rc != VWRN_TRAILING_SPACES && rc != VWRN_TRAILING_CHARS)
        return VERR_INVALID_PARAMETER;

    if (ppszNext != NULL)
        *ppszNext = pszNext;
    return VINF_SUCCESS;
}


RTDECL(int) RTNetStrToIPv4AddrEx(const char *pcszAddr, PRTNETADDRIPV4 pAddr,
                                 char **ppszNext)
{
    return rtNetStrToIPv4AddrEx(pcszAddr, pAddr, ppszNext);
}
RT_EXPORT_SYMBOL(RTNetStrToIPv4AddrEx);


RTDECL(int) RTNetStrToIPv4Addr(const char *pcszAddr, PRTNETADDRIPV4 pAddr)
{
    char *pszNext;
    int rc;

    AssertPtrReturn(pcszAddr, VERR_INVALID_PARAMETER);
    AssertPtrReturn(pAddr, VERR_INVALID_PARAMETER);

    pcszAddr = RTStrStripL(pcszAddr);
    rc = rtNetStrToIPv4AddrEx(pcszAddr, pAddr, &pszNext);
    if (rc != VINF_SUCCESS)
        return VERR_INVALID_PARAMETER;

    pszNext = RTStrStripL(pszNext);
    if (*pszNext != '\0')
        return VERR_INVALID_PARAMETER;

    return VINF_SUCCESS;
}
RT_EXPORT_SYMBOL(RTNetStrToIPv4Addr);


RTDECL(bool) RTNetIsIPv4AddrStr(const char *pcszAddr)
{
    RTNETADDRIPV4 addrIPv4;
    char *pszNext;
    int rc;

    if (pcszAddr == NULL)
        return false;

    rc = rtNetStrToIPv4AddrEx(pcszAddr, &addrIPv4, &pszNext);
    if (rc != VINF_SUCCESS)
        return false;

    if (*pszNext != '\0')
        return false;

    return true;
}
RT_EXPORT_SYMBOL(RTNetIsIPv4AddrStr);


static int rtNetStrToHexGroup(const char *pcszValue, char **ppszNext,
                              uint16_t *pu16)
{
    char *pszNext;
    int rc;

    rc = RTStrToUInt16Ex(pcszValue, &pszNext, 16, pu16);
    if (RT_FAILURE(rc))
        return rc;

    if (   rc != VINF_SUCCESS
        && rc != VWRN_TRAILING_CHARS
        && rc != VWRN_TRAILING_SPACES)
    {
        return -rc;             /* convert warning to error */
    }

    /* parser always accepts 0x prefix */
    if (pcszValue[0] == '0' && (pcszValue[1] == 'x' || pcszValue[1] == 'X'))
    {
        if (pu16)
            *pu16 = 0;
        if (ppszNext)
            *ppszNext = (/* UNCONST */ char *)pcszValue + 1; /* to 'x' */
        return VWRN_TRAILING_CHARS;
    }

    /* parser accepts leading zeroes "000000f" */
    if (pszNext - pcszValue > 4)
        return VERR_PARSE_ERROR;

    if (ppszNext)
        *ppszNext = pszNext;
    return rc;
}


/*
 * This function deals only with the hex-group IPv6 address syntax
 * proper (with possible embedded IPv4).
 */
DECLHIDDEN(int) rtNetStrToIPv6AddrBase(const char *pcszAddr, PRTNETADDRIPV6 pAddrResult,
                                       char **ppszNext)
{
    RTNETADDRIPV6 ipv6;
    RTNETADDRIPV4 ipv4;
    const char *pcszPos;
    char *pszNext;
    int iGroup;
    uint16_t u16;
    int rc;

    memset(&ipv6, 0, sizeof(ipv6));

    pcszPos = pcszAddr;

    if (pcszPos[0] == ':') /* compressed zero run at the beginning? */
    {
        if (pcszPos[1] != ':')
            return VERR_PARSE_ERROR;

        pcszPos += 2;           /* skip over "::" */
        pszNext = (/* UNCONST */ char *)pcszPos;
        iGroup = 1;
    }
    else
    {
        /*
         * Scan forward until we either get complete address or find
         * "::" compressed zero run.
         */
        for (iGroup = 0; iGroup < 8; ++iGroup)
        {
            /* check for embedded IPv4 at the end */
            if (iGroup == 6)
            {
                rc = rtNetStrToIPv4AddrEx(pcszPos, &ipv4, &pszNext);
                if (rc == VINF_SUCCESS)
                {
                    ipv6.au32[3] = ipv4.au32[0];
                    iGroup = 8; /* filled 6 and 7 */
                    break;      /* we are done */
                }
            }

            rc = rtNetStrToHexGroup(pcszPos, &pszNext, &u16);
            if (RT_FAILURE(rc))
                return VERR_PARSE_ERROR;

            ipv6.au16[iGroup] = RT_H2N_U16(u16);

            if (iGroup == 7)
                pcszPos = pszNext;
            else
            {
                /* skip the colon that delimits this group */
                if (*pszNext != ':')
                    return VERR_PARSE_ERROR;
                pcszPos = pszNext + 1;

                /* compressed zero run? */
                if (*pcszPos == ':')
                {
                    ++pcszPos;    /* skip over :: */
                    pszNext += 2; /* skip over :: (in case we are done) */
                    iGroup += 2;  /* current field and the zero in the next */
                    break;
                }
            }
        }
    }

    if (iGroup != 8)
    {
        /*
         * iGroup is the first group that can be filled by the part of
         * the address after "::".
         */
        RTNETADDRIPV6 ipv6Tail;
        const int iMaybeStart = iGroup;
        int j;

        memset(&ipv6Tail, 0, sizeof(ipv6Tail));

        /*
         * We try to accept longest match; we'll shift if necessary.
         * Unlike the first loop, a failure to parse a group doesn't
         * mean invalid address.
         */
        for (; iGroup < 8; ++iGroup)
        {
            /* check for embedded IPv4 at the end */
            if (iGroup <= 6)
            {
                rc = rtNetStrToIPv4AddrEx(pcszPos, &ipv4, &pszNext);
                if (rc == VINF_SUCCESS)
                {
                    ipv6Tail.au16[iGroup]     = ipv4.au16[0];
                    ipv6Tail.au16[iGroup + 1] = ipv4.au16[1];
                    iGroup = iGroup + 2; /* these two are done */
                    break;               /* the rest is trailer */
                }
            }

            rc = rtNetStrToHexGroup(pcszPos, &pszNext, &u16);
            if (RT_FAILURE(rc))
                break;

            ipv6Tail.au16[iGroup] = RT_H2N_U16(u16);

            if (iGroup == 7)
                pcszPos = pszNext;
            else
            {
                if (*pszNext != ':')
                {
                    ++iGroup;   /* this one is done */
                    break;      /* the rest is trailer */
                }

                pcszPos = pszNext + 1;
            }
        }

        for (j = 7, --iGroup; iGroup >= iMaybeStart; --j, --iGroup)
            ipv6.au16[j] = ipv6Tail.au16[iGroup];
    }

    if (pAddrResult != NULL)
        memcpy(pAddrResult, &ipv6, sizeof(ipv6));
    if (ppszNext != NULL)
        *ppszNext = pszNext;
    return VINF_SUCCESS;
}


DECLHIDDEN(int) rtNetStrToIPv6AddrEx(const char *pcszAddr, PRTNETADDRIPV6 pAddr,
                                     char **ppszZone, char **ppszNext)
{
    char *pszNext, *pszZone;
    int rc;

    rc = rtNetStrToIPv6AddrBase(pcszAddr, pAddr, &pszNext);
    if (RT_FAILURE(rc))
        return rc;

    if (*pszNext != '%')      /* is there a zone id? */
    {
        pszZone = NULL;
    }
    else
    {
        pszZone = pszNext + 1; /* skip '%' zone id delimiter */
        if (*pszZone == '\0')
            return VERR_PARSE_ERROR; /* empty zone id */

        /*
         * XXX: this is speculative as zone id syntax is
         * implementation dependent, so we kinda guess here (accepting
         * unreserved characters from URI syntax).
         */
        for (pszNext = pszZone; *pszNext != '\0'; ++pszNext)
        {
            const char c = *pszNext;
            if (   !('0' <= c && c <= '9')
                   && !('a' <= c && c <= 'z')
                   && !('A' <= c && c <= 'Z')
                   && c != '_'
                   && c != '.'
                   && c != '-'
                   && c != '~')
            {
                break;
            }
        }
    }

    if (ppszZone != NULL)
        *ppszZone = pszZone;
    if (ppszNext != NULL)
        *ppszNext = pszNext;

    if (*pszNext == '\0')       /* all input string consumed */
        return VINF_SUCCESS;
    else
    {
        while (*pszNext == ' ' || *pszNext == '\t')
            ++pszNext;
        if (*pszNext == '\0')
            return VWRN_TRAILING_SPACES;
        else
            return VWRN_TRAILING_CHARS;
    }
}


RTDECL(int) RTNetStrToIPv6AddrEx(const char *pcszAddr, PRTNETADDRIPV6 pAddr,
                                 char **ppszNext)
{
    AssertPtrReturn(pcszAddr, VERR_INVALID_PARAMETER);
    AssertPtrReturn(pAddr, VERR_INVALID_PARAMETER);

    return rtNetStrToIPv6AddrBase(pcszAddr, pAddr, ppszNext);
}
RT_EXPORT_SYMBOL(RTNetStrToIPv6AddrEx);


RTDECL(int) RTNetStrToIPv6Addr(const char *pcszAddr, PRTNETADDRIPV6 pAddr,
                               char **ppszZone)
{
    int rc;

    AssertPtrReturn(pcszAddr, VERR_INVALID_PARAMETER);
    AssertPtrReturn(pAddr, VERR_INVALID_PARAMETER);
    AssertPtrReturn(ppszZone, VERR_INVALID_PARAMETER);

    pcszAddr = RTStrStripL(pcszAddr);
    rc = rtNetStrToIPv6AddrEx(pcszAddr, pAddr, ppszZone, NULL);
    if (rc != VINF_SUCCESS && rc != VWRN_TRAILING_SPACES)
        return VERR_INVALID_PARAMETER;

    return VINF_SUCCESS;
}
RT_EXPORT_SYMBOL(RTNetStrToIPv6Addr);


RTDECL(bool) RTNetIsIPv6AddrStr(const char *pcszAddr)
{
    RTNETADDRIPV6 addrIPv6;
    int rc;

    if (pcszAddr == NULL)
        return false;

    rc = rtNetStrToIPv6AddrEx(pcszAddr, &addrIPv6, NULL, NULL);
    if (rc != VINF_SUCCESS)
        return false;

    return true;
}
RT_EXPORT_SYMBOL(RTNetIsIPv6AddrStr);
