/* $Id: VBoxDef2LazyLoad.cpp $ */
/** @file
 * VBoxDef2LazyLoad - Lazy Library Loader Generator.
 *
 * @note Only tested on win.amd64 & darwin.amd64.
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
 */

/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <iprt/types.h>


/*******************************************************************************
*   Structures and Typedefs                                                    *
*******************************************************************************/
typedef struct MYEXPORT
{
    struct MYEXPORT    *pNext;
    bool                fNoName;
    unsigned            uOrdinal;
    char                szName[1];
} MYEXPORT;
typedef MYEXPORT *PMYEXPORT;



/*******************************************************************************
*   Global Variables                                                           *
*******************************************************************************/
/** @name Options
 * @{ */
static const char  *g_pszOutput   = NULL;
static const char  *g_pszLibrary  = NULL;
static const char  *g_apszInputs[8] = { NULL, NULL, NULL, NULL,  NULL, NULL, NULL, NULL };
static unsigned     g_cInputs = 0;
static bool         g_fIgnoreData = true;
static bool         g_fWithExplictLoadFunction = false;
/** @} */

/** Pointer to the export name list head. */
static PMYEXPORT    g_pExpHead  = NULL;
/** Pointer to the next pointer for insertion. */
static PMYEXPORT   *g_ppExpNext = &g_pExpHead;



static const char *leftStrip(const char *psz)
{
    while (isspace(*psz))
        psz++;
    return psz;
}


static char *leftStrip(char *psz)
{
    while (isspace(*psz))
        psz++;
    return psz;
}


static unsigned wordLength(const char *pszWord)
{
    unsigned off = 0;
    char ch;
    while (   (ch = pszWord[off]) != '\0'
           && ch                  != '='
           && ch                  != ','
           && ch                  != ':'
           && !isspace(ch) )
        off++;
    return off;
}


/**
 * Parses the module definition file, collecting export information.
 *
 * @returns RTEXITCODE_SUCCESS or RTEXITCODE_FAILURE, in the latter case full
 *          details has been displayed.
 * @param   pInput              The input stream.
 */
static RTEXITCODE parseInputInner(FILE *pInput, const char *pszInput)
{
    /*
     * Process the file line-by-line.
     */
    bool        fInExports = false;
    unsigned    iLine      = 0;
    char        szLine[16384];
    while (fgets(szLine, sizeof(szLine), pInput))
    {
        iLine++;

        /*
         * Strip leading and trailing spaces from the line as well as
         * trailing comments.
         */
        char *psz = leftStrip(szLine);
        if (*psz == ';')
            continue; /* comment line. */

        char *pszComment = strchr(psz, ';');
        if (pszComment)
            *pszComment = '\0';

        unsigned cch = (unsigned)strlen(psz);
        while (cch > 0 && (isspace(psz[cch - 1]) || psz[cch - 1] == '\r' || psz[cch - 1] == '\n'))
            psz[--cch] = '\0';

        if (!cch)
            continue;

        /*
         * Check for known directives.
         */
        size_t cchWord0 = wordLength(psz);
#define WORD_CMP(pszWord1, cchWord1, szWord2) \
            ( (cchWord1) == sizeof(szWord2) - 1 && memcmp(pszWord1, szWord2, sizeof(szWord2) - 1) == 0 )
        if (WORD_CMP(psz, cchWord0, "EXPORTS"))
        {
            fInExports = true;

            /* In case there is an export on the same line. (Really allowed?) */
            psz = leftStrip(psz + sizeof("EXPORTS") - 1);
            if (!*psz)
                continue;
        }
        /* Directives that we don't care about, but need to catch in order to
           terminate the EXPORTS section in a timely manner. */
        else if (   WORD_CMP(psz, cchWord0, "NAME")
                 || WORD_CMP(psz, cchWord0, "LIBRARY")
                 || WORD_CMP(psz, cchWord0, "DESCRIPTION")
                 || WORD_CMP(psz, cchWord0, "STACKSIZE")
                 || WORD_CMP(psz, cchWord0, "SECTIONS")
                 || WORD_CMP(psz, cchWord0, "SEGMENTS")
                 || WORD_CMP(psz, cchWord0, "VERSION")
                )
        {
            fInExports = false;
        }

        /*
         * Process exports:
         *  entryname[=internalname] [@ordinal[ ][NONAME]] [DATA] [PRIVATE]
         */
        if (fInExports)
        {
            const char *pchName = psz;
            unsigned    cchName = wordLength(psz);

            psz = leftStrip(psz + cchName);
            if (*psz == '=')
            {
                psz = leftStrip(psz + 1);
                psz = leftStrip(psz + wordLength(psz));
            }

            bool     fNoName  = true;
            unsigned uOrdinal = ~0U;
            if (*psz == '@')
            {
                psz++;
                if (!isdigit(*psz))
                {
                    fprintf(stderr, "%s:%u: error: Invalid ordinal spec.\n", pszInput, iLine);
                    return RTEXITCODE_FAILURE;
                }
                uOrdinal = *psz++ - '0';
                while (isdigit(*psz))
                {
                    uOrdinal *= 10;
                    uOrdinal += *psz++ - '0';
                }
                psz = leftStrip(psz);
                cch = wordLength(psz);
                if (WORD_CMP(psz, cch, "NONAME"))
                {
#if 0
                    fNoName = true;
                    psz = leftStrip(psz + cch);
#else
                    fprintf(stderr, "%s:%u: error: NONAME export not implemented.\n", pszInput, iLine);
                    return RTEXITCODE_FAILURE;
#endif
                }
            }

            while (*psz)
            {
                cch = wordLength(psz);
                if (WORD_CMP(psz, cch, "DATA"))
                {
                    if (!g_fIgnoreData)
                    {
                        fprintf(stderr, "%s:%u: error: Cannot wrap up DATA export '%.*s'.\n",
                                pszInput, iLine, cchName, pchName);
                        return RTEXITCODE_SUCCESS;
                    }
                }
                else if (!WORD_CMP(psz, cch, "PRIVATE"))
                {
                    fprintf(stderr, "%s:%u: error: Cannot wrap up DATA export '%.*s'.\n",
                            pszInput, iLine, cchName, pchName);
                    return RTEXITCODE_SUCCESS;
                }
                psz = leftStrip(psz + cch);
            }

            /*
             * Add the export.
             */
            PMYEXPORT pExp = (PMYEXPORT)malloc(sizeof(*pExp) + cchName);
            if (!pExp)
            {
                fprintf(stderr, "%s:%u: error: Out of memory.\n", pszInput, iLine);
                return RTEXITCODE_SUCCESS;
            }
            memcpy(pExp->szName, pchName, cchName);
            pExp->szName[cchName] = '\0';
            pExp->uOrdinal = uOrdinal;
            pExp->fNoName  = fNoName;
            pExp->pNext    = NULL;
            *g_ppExpNext   = pExp;
            g_ppExpNext    = &pExp->pNext;
        }
    }

    /*
     * Why did we quit the loop, EOF or error?
     */
    if (feof(pInput))
        return RTEXITCODE_SUCCESS;
    fprintf(stderr, "error: Read while reading '%s' (iLine=%u).\n", pszInput, iLine);
    return RTEXITCODE_FAILURE;
}


/**
 * Parses a_apszInputs, populating the list pointed to by g_pExpHead.
 *
 * @returns RTEXITCODE_SUCCESS or RTEXITCODE_FAILURE, in the latter case full
 *          details has been displayed.
 */
static RTEXITCODE  parseInputs(void)
{
    RTEXITCODE rcExit = RTEXITCODE_SUCCESS;
    for (unsigned i = 0; i < g_cInputs; i++)
    {
        FILE *pInput = fopen(g_apszInputs[i], "r");
        if (pInput)
        {
            RTEXITCODE rcExit2 = parseInputInner(pInput, g_apszInputs[i]);
            fclose(pInput);
            if (rcExit2 == RTEXITCODE_SUCCESS && !g_pExpHead)
            {
                fprintf(stderr, "error: Found no exports in '%s'.\n", g_apszInputs[i]);
                rcExit2 = RTEXITCODE_FAILURE;
            }
            if (rcExit2 != RTEXITCODE_SUCCESS)
                rcExit = rcExit2;
        }
        else
        {
            fprintf(stderr, "error: Failed to open '%s' for reading.\n", g_apszInputs[i]);
            rcExit = RTEXITCODE_FAILURE;
        }
    }
    return rcExit;
}


/**
 * Generates the assembly source code, writing it to @a pOutput.
 *
 * @returns RTEXITCODE_SUCCESS or RTEXITCODE_FAILURE, in the latter case full
 *          details has been displayed.
 * @param   pOutput              The output stream (caller checks it for errors
 *                               when closing).
 */
static RTEXITCODE generateOutputInner(FILE *pOutput)
{
    fprintf(pOutput, ";;\n");
    for (unsigned i = 0; i < g_cInputs; i++)
        fprintf(pOutput, ";; Autogenerated from '%s'.\n", g_apszInputs[i]);

    fprintf(pOutput,
            ";; DO NOT EDIT!\n"
            ";;\n"
            "\n"
            "\n"
            "%%include \"iprt/asmdefs.mac\"\n"
            "\n"
            "\n");

    /*
     * Put the thunks first for alignment and other reasons. It's the hot part of the code.
     */
    fprintf(pOutput,
            ";\n"
            "; Thunks.\n"
            ";\n"
            "BEGINCODE\n");
    for (PMYEXPORT pExp = g_pExpHead; pExp; pExp = pExp->pNext)
        fprintf(pOutput,
                "BEGINPROC %s\n"
                "    jmp RTCCPTR_PRE [g_pfn%s xWrtRIP]\n"
                "ENDPROC   %s\n",
                pExp->szName,
                pExp->szName,
                pExp->szName);
    fprintf(pOutput,
            "\n"
            "\n");

    /*
     * Import pointers
     */
    fprintf(pOutput,
            ";\n"
            "; Import pointers. Initialized to point a lazy loading stubs.\n"
            ";\n"
            "BEGINDATA\n"
            "g_apfnImports:\n");
    for (PMYEXPORT pExp = g_pExpHead; pExp; pExp = pExp->pNext)
        fprintf(pOutput,
                "%%ifdef ASM_FORMAT_PE\n"
                "global __imp_%s\n"
                "__imp_%s:\n"
                "%%endif\n"
                "g_pfn%s RTCCPTR_DEF ___LazyLoad___%s\n",
                pExp->szName,
                pExp->szName,
                pExp->szName,
                pExp->szName);
    fprintf(pOutput,
            "RTCCPTR_DEF 0 ; Terminator entry for traversal.\n"
            "\n"
            "\n");

    /*
     * Now for the less important stuff, starting with the names.
     *
     * We keep the names separate so we can traverse them in parallel to
     * g_apfnImports in the load-everything routine further down.
     */
    fprintf(pOutput,
            ";\n"
            "; Imported names.\n"
            ";\n"
            "BEGINCODE\n"
            "g_szLibrary db '%s',0\n"
            "g_szzNames:\n",
            g_pszLibrary);
    for (PMYEXPORT pExp = g_pExpHead; pExp; pExp = pExp->pNext)
        fprintf(pOutput, "g_sz%s: db '%s',0\n", pExp->szName, pExp->szName);
    fprintf(pOutput,
            "g_EndOfNames: db 0\n"
            "\n"
            "\n");

    /*
     * The per import lazy load code.
     */
    fprintf(pOutput,
            ";\n"
            "; Lazy load+resolve stubs.\n"
            ";\n"
            "BEGINCODE\n");
    for (PMYEXPORT pExp = g_pExpHead; pExp; pExp = pExp->pNext)
        fprintf(pOutput,
                "___LazyLoad___%s:\n"
                /* "int3\n" */
                "%%ifdef RT_ARCH_AMD64\n"
                "    lea     rax, [g_sz%s wrt rip]\n"
                "    lea     r10, [g_pfn%s wrt rip]\n"
                "%%elifdef RT_ARCH_X86\n"
                "    push    g_sz%s\n"
                "    push    g_pfn%s\n"
                "%%else\n"
                " %%error \"Unsupported architecture\"\n"
                "%%endif\n"
                "    call    LazyLoadResolver\n"
                "%%ifdef RT_ARCH_X86\n"
                "    add     esp, 8h\n"
                "%%endif\n"
                "    jmp     NAME(%s)\n"
                "\n"
                ,
                pExp->szName,
                pExp->szName,
                pExp->szName,
                pExp->szName,
                pExp->szName,
                pExp->szName);
    fprintf(pOutput,
            "\n"
            "\n"
            "\n");

    /*
     * The code that does the loading and resolving.
     */
    fprintf(pOutput,
            ";\n"
            "; The module handle.\n"
            ";\n"
            "BEGINDATA\n"
            "g_hMod RTCCPTR_DEF 0\n"
            "\n"
            "\n"
            "\n");

    /*
     * How we load the module needs to be selectable later on.
     *
     * The LazyLoading routine returns the module handle in RCX/ECX, caller
     * saved all necessary registers.
     */
    fprintf(pOutput,
            ";\n"
            ";SUPR3DECL(int) SUPR3HardenedLdrLoadAppPriv(const char *pszFilename, PRTLDRMOD phLdrMod,\n"
            ";                                           uint32_t fFlags, PRTERRINFO pErrInfo);\n"
            ";\n"
            "EXTERN_IMP2 SUPR3HardenedLdrLoadAppPriv\n"
            "BEGINCODE\n"
            "\n"
            "LazyLoading:\n"
            "    mov     xCX, [g_hMod xWrtRIP]\n"
            "    or      xCX, xCX\n"
            "    jnz     .return\n"
            "\n"
            "%%ifdef ASM_CALL64_GCC\n"
            "    xor     rcx, rcx               ; pErrInfo\n"
            "    xor     rdx, rdx               ; fFlags (local load)\n"
            "    lea     rsi, [g_hMod wrt rip]  ; phLdrMod\n"
            "    lea     rdi, [g_szLibrary wrt rip] ; pszFilename\n"
            "    sub     rsp, 08h\n"
            "    call    IMP2(SUPR3HardenedLdrLoadAppPriv)\n"
            "    add     rsp, 08h\n"
            "\n"
            "%%elifdef ASM_CALL64_MSC\n"
            "    xor     r9, r9                 ; pErrInfo\n"
            "    xor     r8, r8                 ; fFlags (local load)\n"
            "    lea     rdx, [g_hMod wrt rip]  ; phLdrMod\n"
            "    lea     rcx, [g_szLibrary wrt rip] ; pszFilename\n"
            "    sub     rsp, 28h\n"
            "    call    IMP2(SUPR3HardenedLdrLoadAppPriv)\n"
            "    add     rsp, 28h\n"
            "\n"
            "%%elifdef RT_ARCH_X86\n"
            "    sub     xSP, 0ch\n"
            "    push    0              ; pErrInfo\n"
            "    push    0              ; fFlags (local load)\n"
            "    push    g_hMod         ; phLdrMod\n"
            "    push    g_szLibrary    ; pszFilename\n"
            "    call    IMP2(SUPR3HardenedLdrLoadAppPriv)\n"
            "    add     esp, 1ch\n"
            "%%else\n"
            " %%error \"Unsupported architecture\"\n"
            "%%endif\n"
            "    or      eax, eax\n"
            "    jz      .loadok\n"
            ".badload:\n"
            "    int3\n"
            "    jmp     .badload\n"
            ".loadok:\n"
            "    mov     xCX, [g_hMod xWrtRIP]\n"
            ".return:\n"
            "    ret\n"
            "LazyLoading_End:\n"
            "\n"
            "\n");


    fprintf(pOutput,
            ";\n"
            ";RTDECL(int) RTLdrGetSymbol(RTLDRMOD hLdrMod, const char *pszSymbol, void **ppvValue);\n"
            ";\n"
            "EXTERN_IMP2 RTLdrGetSymbol\n"
            "BEGINCODE\n"
            "LazyLoadResolver:\n"
            "%%ifdef RT_ARCH_AMD64\n"
            "    push    rbp\n"
            "    mov     rbp, rsp\n"
            "    push    r15\n"
            "    push    r14\n"
            "    mov     r15, rax       ; name\n"
            "    mov     r14, r10       ; ppfn\n"
            "    push    r9\n"
            "    push    r8\n"
            "    push    rcx\n"
            "    push    rdx\n"
            "    push    r12\n"
            " %%ifdef ASM_CALL64_GCC\n"
            "    push    rsi\n"
            "    push    rdi\n"
            "    mov     r12, rsp\n"
            " %%else\n"
            "    mov     r12, rsp\n"
            "    sub     rsp, 20h\n"
            " %%endif\n"
            "    and     rsp, 0fffffff0h ; Try make sure the stack is aligned\n"
            "\n"
            "    call    LazyLoading    ; returns handle in rcx\n"
            " %%ifdef ASM_CALL64_GCC\n"
            "    mov     rdi, rcx       ; hLdrMod\n"
            "    mov     rsi, r15       ; pszSymbol\n"
            "    mov     rdx, r14       ; ppvValue\n"
            " %%else\n"
            "    mov     rdx, r15       ; pszSymbol\n"
            "    mov     r8, r14        ; ppvValue\n"
            " %%endif\n"
            "    call    IMP2(RTLdrGetSymbol)\n"
            "    or      eax, eax\n"
            "    jz      .symok\n"
            ".badsym:\n"
            "    int3\n"
            "    jmp     .badsym\n"
            ".symok:\n"
            "\n"
            "    mov     rsp, r12\n"
            " %%ifdef ASM_CALL64_GCC\n"
            "    pop     rdi\n"
            "    pop     rsi\n"
            " %%endif\n"
            "    pop     r12\n"
            "    pop     rdx\n"
            "    pop     rcx\n"
            "    pop     r8\n"
            "    pop     r9\n"
            "    pop     r14\n"
            "    pop     r15\n"
            "    leave\n"
            "\n"
            "%%elifdef RT_ARCH_X86\n"
            "    push    ebp\n"
            "    mov     ebp, esp\n"
            "    push    eax\n"
            "    push    ecx\n"
            "    push    edx\n"
            "    and     esp, 0fffffff0h\n"
            "\n"
            ".loaded:\n"
            "    mov     eax, [ebp + 4] ; value addr\n"
            "    push    eax\n"
            "    mov     edx, [ebp + 8] ; symbol name\n"
            "    push    edx\n"
            "    call    LazyLoading    ; returns handle in ecx\n"
            "    mov     ecx, [g_hMod]\n"
            "    call    IMP2(RTLdrGetSymbol)\n"
            "    or      eax, eax\n"
            "    jz      .symok\n"
            ".badsym:\n"
            "    int3\n"
            "    jmp     .badsym\n"
            ".symok:\n"
            "    lea     esp, [ebp - 0ch]\n"
            "    pop     edx\n"
            "    pop     ecx\n"
            "    pop     eax\n"
            "    leave\n"
            "%%else\n"
            " %%error \"Unsupported architecture\"\n"
            "%%endif\n"
            "    ret\n"
            "LazyLoadResolver_End:\n"
            "\n"
            "\n"
            );



    /*
     * C callable method for explicitly loading the library and optionally
     * resolving all the imports.
     */
    if (g_fWithExplictLoadFunction)
    {
        int cchLibBaseName = (int)(strchr(g_pszLibrary, '.') ? strchr(g_pszLibrary, '.') - g_pszLibrary : strlen(g_pszLibrary));
        fprintf(pOutput,
                ";;\n"
                "; ExplicitlyLoad%.*s(bool fResolveAllImports, pErrInfo);\n"
                ";\n"
                "EXTERN_IMP2 RTErrInfoSet\n"
                "BEGINCODE\n"
                "BEGINPROC ExplicitlyLoad%.*s\n"
                "    push    xBP\n"
                "    mov     xBP, xSP\n"
                "    push    xBX\n"
                "%%ifdef ASM_CALL64_GCC\n"
                " %%define pszCurStr r14\n"
                "    push    r14\n"
                "%%else\n"
                " %%define pszCurStr xDI\n"
                "    push    xDI\n"
                "%%endif\n"
                "    sub     xSP, 40h\n"
                "\n"
                "    ;\n"
                "    ; Save parameters on stack (64-bit only).\n"
                "    ;\n"
                "%%ifdef ASM_CALL64_GCC\n"
                "    mov     [xBP - xCB * 3], rdi ; fResolveAllImports\n"
                "    mov     [xBP - xCB * 4], rsi ; pErrInfo\n"
                "%%elifdef ASM_CALL64_MSC\n"
                "    mov     [xBP - xCB * 3], rcx ; fResolveAllImports\n"
                "    mov     [xBP - xCB * 4], rdx ; pErrInfo\n"
                "%%endif\n"
                "\n"
                "    ;\n"
                "    ; Is the module already loaded?\n"
                "    ;\n"
                "    cmp     RTCCPTR_PRE [g_hMod xWrtRIP], 0\n"
                "    jnz     .loaded\n"
                "\n"
                "    ;\n"
                "    ; Load the module.\n"
                "    ;\n"
                "%%ifdef ASM_CALL64_GCC\n"
                "    mov     rcx, [xBP - xCB * 4]       ; pErrInfo\n"
                "    xor     rdx, rdx                   ; fFlags (local load)\n"
                "    lea     rsi, [g_hMod wrt rip]      ; phLdrMod\n"
                "    lea     rdi, [g_szLibrary wrt rip] ; pszFilename\n"
                "    call    IMP2(SUPR3HardenedLdrLoadAppPriv)\n"
                "\n"
                "%%elifdef ASM_CALL64_MSC\n"
                "    mov     r9, [xBP - xCB * 4]        ; pErrInfo\n"
                "    xor     r8, r8                     ; fFlags (local load)\n"
                "    lea     rdx, [g_hMod wrt rip]      ; phLdrMod\n"
                "    lea     rcx, [g_szLibrary wrt rip] ; pszFilename\n"
                "    call    IMP2(SUPR3HardenedLdrLoadAppPriv)\n"
                "\n"
                "%%elifdef RT_ARCH_X86\n"
                "    sub     xSP, 0ch\n"
                "    push    dword [xBP + 12]           ; pErrInfo\n"
                "    push    0                          ; fFlags (local load)\n"
                "    push    g_hMod                     ; phLdrMod\n"
                "    push    g_szLibrary                ; pszFilename\n"
                "    call    IMP2(SUPR3HardenedLdrLoadAppPriv)\n"
                "    add     esp, 1ch\n"
                "%%else\n"
                " %%error \"Unsupported architecture\"\n"
                "%%endif\n"
                "    or      eax, eax\n"
                "    jnz     .return\n"
                "\n"
                "    ;\n"
                "    ; Resolve the imports too if requested to do so.\n"
                "    ;\n"
                ".loaded:\n"
                "%%ifdef ASM_ARCH_X86\n"
                "    cmp     byte [xBP + 8], 0\n"
                "%%else\n"
                "    cmp     byte [xBP - xCB * 3], 0\n"
                "%%endif\n"
                "    je      .return\n"
                "\n"
                "    lea     pszCurStr, [g_szzNames xWrtRIP]\n"
                "    lea     xBX, [g_apfnImports xWrtRIP]\n"
                ".next_import:\n"
                "    cmp     RTCCPTR_PRE [xBX], 0\n"
                "    je      .return\n"
                "%%ifdef ASM_CALL64_GCC\n"
                "    mov     rdx, xBX                  ; ppvValue\n"
                "    mov     rsi, pszCurStr            ; pszSymbol\n"
                "    mov     rdi, [g_hMod wrt rip]     ; hLdrMod\n"
                "    call    IMP2(RTLdrGetSymbol)\n"
                "%%elifdef ASM_CALL64_MSC\n"
                "    mov     r8, xBX                   ; ppvValue\n"
                "    mov     rdx, pszCurStr            ; pszSymbol\n"
                "    mov     rcx, [g_hMod wrt rip]     ; pszSymbol\n"
                "    call    IMP2(RTLdrGetSymbol)\n"
                "%%else\n"
                "    push    xBX                       ; ppvValue\n"
                "    push    pszCurStr                 ; pszSymbol\n"
                "    push    RTCCPTR_PRE [g_hMod]      ; hLdrMod\n"
                "    call    IMP2(RTLdrGetSymbol)\n"
                "    add     xSP, 0ch\n"
                "%%endif\n"
                "    or      eax, eax\n"
                "    jnz     .symbol_error\n"
                "\n"
                "    ; Advance.\n"
                "    add     xBX, RTCCPTR_CB\n"
                "    xor     eax, eax\n"
                "    mov     xCX, 0ffffffffh\n"
                "%%ifdef ASM_CALL64_GCC\n"
                "    mov     xDI, pszCurStr\n"
                "    repne scasb\n"
                "    mov     pszCurStr, xDI\n"
                "%%else\n"
                "    repne scasb\n"
                "%%endif\n"
                "    jmp     .next_import\n"
                "\n"
                "    ;\n"
                "    ; Error loading a symbol. Call RTErrInfoSet on pErrInfo (preserves eax).\n"
                "    ;\n"
                ".symbol_error:\n"
                "%%ifdef ASM_CALL64_GCC\n"
                "    mov     rdx, pszCurStr            ; pszMsg\n"
                "    mov     esi, eax                  ; rc\n"
                "    mov     rdi, [xBP - xCB * 4]      ; pErrInfo\n"
                "    call    IMP2(RTErrInfoSet)\n"
                "%%elifdef ASM_CALL64_MSC\n"
                "    mov     r8, pszCurStr             ; pszMsg\n"
                "    mov     edx, eax                  ; rc\n"
                "    mov     rcx, [xBP - xCB * 4]      ; pErrInfo\n"
                "    call    IMP2(RTErrInfoSet)\n"
                "%%else\n"
                "    push    pszCurStr                 ; pszMsg\n"
                "    push    eax                       ; pszSymbol\n"
                "    push    dword [xBP + 0ch]         ; pErrInfo\n"
                "    call    IMP2(RTErrInfoSet)\n"
                "    add     xSP, 0ch\n"
                "%%endif\n"
                "    "
                "\n"
                ".return:\n"
                "    mov    pszCurStr, [xBP - xCB * 2]\n"
                "    mov    xBX,       [xBP - xCB * 1]\n"
                "    leave\n"
                "    ret\n"
                "ENDPROC   ExplicitlyLoad%.*s\n"
                "\n"
                "\n"
                ,
                cchLibBaseName, g_pszLibrary,
                cchLibBaseName, g_pszLibrary,
                cchLibBaseName, g_pszLibrary
                );
    }


    return RTEXITCODE_SUCCESS;
}


/**
 * Generates the assembly source code, writing it to g_pszOutput.
 *
 * @returns RTEXITCODE_SUCCESS or RTEXITCODE_FAILURE, in the latter case full
 *          details has been displayed.
 */
static RTEXITCODE generateOutput(void)
{
    RTEXITCODE rcExit = RTEXITCODE_FAILURE;
    FILE *pOutput = fopen(g_pszOutput, "w");
    if (pOutput)
    {
        rcExit = generateOutputInner(pOutput);
        if (fclose(pOutput))
        {
            fprintf(stderr, "error: Error closing '%s'.\n", g_pszOutput);
            rcExit = RTEXITCODE_FAILURE;
        }
    }
    else
        fprintf(stderr, "error: Failed to open '%s' for writing.\n", g_pszOutput);
    return rcExit;
}


/**
 * Displays usage information.
 *
 * @returns RTEXITCODE_SUCCESS.
 * @param   pszArgv0            The argv[0] string.
 */
static int usage(const char *pszArgv0)
{
    printf("usage: %s [options] --libary <loadname> --output <lazyload.asm> <input.def>\n"
           "\n"
           "Options:\n"
           "  --explicit-load-function, --no-explicit-load-function\n"
           "    Whether to include the explicit load function, default is not to.\n"
           "\n"
           "Copyright (C) 2013-2015 Oracle Corporation\n"
           , pszArgv0);

    return RTEXITCODE_SUCCESS;
}


int main(int argc, char **argv)
{
    /*
     * Parse options.
     */
    for (int i = 1; i < argc; i++)
    {
        const char *psz = argv[i];
        if (*psz == '-')
        {
            if (!strcmp(psz, "--output") || !strcmp(psz, "-o"))
            {
                if (++i >= argc)
                {
                    fprintf(stderr, "syntax error: File name expected after '%s'.\n", psz);
                    return RTEXITCODE_SYNTAX;
                }
                g_pszOutput = argv[i];
            }
            else if (!strcmp(psz, "--library") || !strcmp(psz, "-l"))
            {
                if (++i >= argc)
                {
                    fprintf(stderr, "syntax error: Library name expected after '%s'.\n", psz);
                    return RTEXITCODE_SYNTAX;
                }
                g_pszLibrary = argv[i];
            }
            else if (!strcmp(psz, "--explicit-load-function"))
                g_fWithExplictLoadFunction = true;
            else if (!strcmp(psz, "--no-explicit-load-function"))
                g_fWithExplictLoadFunction = false;
            /** @todo Support different load methods so this can be used on system libs and
             *        such if we like. */
            else if (   !strcmp(psz, "--help")
                     || !strcmp(psz, "-help")
                     || !strcmp(psz, "-h")
                     || !strcmp(psz, "-?") )
                return usage(argv[0]);
            else if (   !strcmp(psz, "--version")
                     || !strcmp(psz, "-V"))
            {
                printf("$Revision: 98272 $\n");
                return RTEXITCODE_SUCCESS;
            }
            else
            {
                fprintf(stderr, "syntax error: Unknown option '%s'.\n", psz);
                return RTEXITCODE_SYNTAX;
            }
        }
        else
        {
            if (g_cInputs >= RT_ELEMENTS(g_apszInputs))
            {
                fprintf(stderr, "syntax error: Too many input files, max is %d.\n", (int)RT_ELEMENTS(g_apszInputs));
                return RTEXITCODE_SYNTAX;
            }
            g_apszInputs[g_cInputs++] = argv[i];
        }
    }
    if (g_cInputs == 0)
    {
        fprintf(stderr, "syntax error: No input file specified.\n");
        return RTEXITCODE_SYNTAX;
    }
    if (!g_pszOutput)
    {
        fprintf(stderr, "syntax error: No output file specified.\n");
        return RTEXITCODE_SYNTAX;
    }
    if (!g_pszLibrary)
    {
        fprintf(stderr, "syntax error: No library name specified.\n");
        return RTEXITCODE_SYNTAX;
    }

    /*
     * Do the job.
     */
    RTEXITCODE rcExit = parseInputs();
    if (rcExit == RTEXITCODE_SUCCESS)
        rcExit = generateOutput();
    return rcExit;
}

