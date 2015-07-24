/** @file
 * IPRT - Crypto - PEM-file Reader & Writer.
 */

/*
 * Copyright (C) 2006-2014 Oracle Corporation
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

#ifndef ___iprt_crypto_spc_h
#define ___iprt_crypto_spc_h

#include <iprt/asn1.h>
#include <iprt/crypto/x509.h>
#include <iprt/crypto/pkcs7.h>
#include <iprt/md5.h>
#include <iprt/sha.h>


RT_C_DECLS_BEGIN

/** @defgroup grp_rt_spc  RTCrPem - PEM-file Reader & Writer
 * @ingroup grp_rt_crypto
 * @{
 */


/**
 * One PEM marker word (use RT_STR_TUPLE to initialize).
 */
typedef struct RTCRPEMMARKERWORD
{
    /** The word string. */
    const char         *pszWord;
    /** The length. */
    uint32_t            cchWord;
} RTCRPEMMARKERWORD;
/** Pointer to a const marker word. */
typedef RTCRPEMMARKERWORD const *PCRTCRPEMMARKERWORD;


/**
 * A PEM marker.
 *
 * This is an array of words with lengths, optimized for avoid unnecessary
 * strlen() while searching the file content.  It is ASSUMED that all PEM
 * section markers starts with either 'BEGIN' or 'END', followed by the words
 * in the this structure.
 */
typedef struct RTCRPEMMARKER
{
    /** Pointer to an array of marker words. */
    PCRTCRPEMMARKERWORD paWords;
    /** Number of works in the array papszWords points to. */
    uint32_t            cWords;
} RTCRPEMMARKER;
/** Pointer to a const PEM marker. */
typedef RTCRPEMMARKER const *PCRTCRPEMMARKER;


/**
 * A PEM section.
 *
 * The API works on linked lists of these.
 */
typedef struct RTCRPEMSECTION
{
    /** Pointer to the next file section. */
    struct RTCRPEMSECTION const *pNext;
    /** The marker for this section.  NULL if binary file. */
    PCRTCRPEMMARKER     pMarker;
    /** Pointer to the binary data. */
    uint8_t            *pbData;
    /** The size of the binary data. */
    size_t              cbData;
    /** Additional text preceeding the binary data.  NULL if none. */
    char               *pszPreamble;
    /** The length of the preamble. */
    size_t              cchPreamble;
} RTCRPEMSECTION;
/** Pointer to a PEM section. */
typedef RTCRPEMSECTION *PRTCRPEMSECTION;
/** Pointer to a const PEM section. */
typedef RTCRPEMSECTION const *PCRTCRPEMSECTION;


/**
 * Frees sections returned by RTCrPemReadFile.
 * @returns IPRT status code.
 * @param   pSectionHead        The first section.
 */
RTDECL(int) RTCrPemFreeSections(PCRTCRPEMSECTION pSectionHead);

/**
 * Reads the content of the given file and returns a list of binary sections
 * found in the file.
 *
 * If the file isn't an ASCII file or if no markers were found, the entire file
 * content is returned as one single section (with pMarker = NULL).
 *
 * @returns IPRT status code.
 * @param   pszFilename     The path to the file to read.
 * @param   fFlags          Flags reserved for future hacks.
 * @param   paMarkers       Array of one or more section markers to look for.
 * @param   cMarkers        Number of markers in the array.
 * @param   ppSectionHead   Where to return the head of the section list.
 * @param   pErrInfo        Where to return extend error info. Optional.
 */
RTDECL(int) RTCrPemReadFile(const char *pszFilename, uint32_t fFlags, PCRTCRPEMMARKER paMarkers, size_t cMarkers,
                            PCRTCRPEMSECTION *ppSectionHead, PRTERRINFO pErrInfo);

/** @} */

RT_C_DECLS_END

#endif

