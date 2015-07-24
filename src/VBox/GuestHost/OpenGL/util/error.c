/* $Id: error.c $ */
/** @file
 * VBox crOpenGL error logging
 */

/*
 * Copyright (C) 2014 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */
#if 1

#define LOG_GROUP LOG_GROUP_SHARED_CROPENGL

#include <iprt/string.h>
#include <iprt/stream.h>
#include <iprt/initterm.h>
#include <VBox/log.h>

#ifdef RT_OS_WINDOWS
# include <windows.h>
# include "cr_environment.h"
#endif

#include <signal.h>
#include <stdlib.h>

static void logMessageV(const char *pszPrefix, const char *pszFormat, va_list va)
{
    va_list vaCopy;
    if (RTR3InitIsInitialized())
    {
        va_copy(vaCopy, va);
        LogRel(("%s%N\n", pszPrefix, pszFormat, &vaCopy));
        va_end(vaCopy);
    }

#ifdef IN_GUEST  /** @todo Could be subject to pre-iprt-init issues, but hopefully not... */
    va_copy(vaCopy, va);
    RTStrmPrintf(g_pStdErr, "%s%N\n", pszPrefix, pszFormat, &vaCopy);
    va_end(vaCopy);
#endif
}

static void logMessage(const char *pszPrefix, const char *pszFormat, ...)
{
    va_list va;

    va_start(va, pszFormat);
    logMessageV(pszPrefix, pszFormat, va);
    va_end(va);
}

DECLEXPORT(void) crError(const char *pszFormat, ...)
{
    va_list va;
#ifdef WINDOWS
    DWORD dwLastErr;
#endif

#ifdef WINDOWS
    /* Log last error on windows. */
    dwLastErr = GetLastError();
    if (dwLastErr != 0 && crGetenv("CR_WINDOWS_ERRORS") != NULL)
    {
        LPTSTR pszWindowsMessage;

        SetLastError(0);
        FormatMessageA(  FORMAT_MESSAGE_ALLOCATE_BUFFER
                       | FORMAT_MESSAGE_FROM_SYSTEM
                       | FORMAT_MESSAGE_MAX_WIDTH_MASK,
                       NULL, dwLastErr,
                       MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                       (LPTSTR)&pszWindowsMessage, 0, NULL);
        if (pszWindowsMessage)
        {
            logMessage("OpenGL, Windows error: ", "%u\n%s", dwLastErr, pszWindowsMessage);
            LocalFree(pszWindowsMessage);
        }
        else
            logMessage("OpenGL, Windows error: ", "%u", dwLastErr);
    }
#endif

    /* The message. */
    va_start(va, pszFormat);
    logMessageV("OpenGL Error: ", pszFormat, va);
    va_end(va);

    /* Dump core or activate the debugger in debug builds. */
    AssertFailed();

#ifdef IN_GUEST
    /* Give things a chance to close down. */
    raise(SIGTERM);
    exit(1);
#endif
}

DECLEXPORT(void) crWarning(const char *pszFormat, ...)
{
    if (RTR3InitIsInitialized())
    {
        va_list va;

        va_start(va, pszFormat);
        logMessageV("OpenGL Warning: ", pszFormat, va);
        va_end(va);
    }
}

DECLEXPORT(void) crInfo(const char *pszFormat, ...)
{
    if (RTR3InitIsInitialized())
    {
        va_list va;

        va_start(va, pszFormat);
        logMessageV("OpenGL Info: ", pszFormat, va);
        va_end(va);
    }
}

DECLEXPORT(void) crDebug(const char *pszFormat, ...)
{
    if (RTR3InitIsInitialized())
    {
        va_list va;

        va_start(va, pszFormat);
#if defined(DEBUG_vgalitsy) || defined(DEBUG_galitsyn)
        LogRel(("OpenGL Debug: %N\n", pszFormat, &va));
#else
        Log(("OpenGL Debug: %N\n", pszFormat, &va));
#endif
        va_end(va);
    }
}

#else
/* Copyright (c) 2001, Stanford University
 * All rights reserved
 *
 * See the file LICENSE.txt for information on redistributing this software.
 */

#include "cr_environment.h"
#include "cr_error.h"
#include "cr_string.h"
#include "cr_net.h"
#include "cr_process.h"

#ifdef WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <signal.h>

#ifndef IN_GUEST
#define LOG_GROUP LOG_GROUP_SHARED_CROPENGL
#endif
#if !defined(IN_GUEST) || defined(CR_DEBUG_BACKDOOR_ENABLE)
#include <VBox/log.h>
#endif

#if defined(WINDOWS)
# define CR_DEBUG_CONSOLE_ENABLE

# include "Shlwapi.h"
#endif

#if defined(WINDOWS) && defined(IN_GUEST)
# ifndef CR_DEBUG_BACKDOOR_ENABLE
#  error "CR_DEBUG_BACKDOOR_ENABLE is expected!"
# endif
#else
# ifdef CR_DEBUG_BACKDOOR_ENABLE
#  error "CR_DEBUG_BACKDOOR_ENABLE is NOT expected!"
# endif
#endif


#ifdef CR_DEBUG_BACKDOOR_ENABLE
# include <VBoxDispMpLogger.h>
# include <iprt/err.h>
#endif


static char my_hostname[256];
#ifdef WINDOWS
static HANDLE my_pid;
#else
static int my_pid = 0;
#endif
static int canada = 0;
static int swedish_chef = 0;
static int australia = 0;
static int warnings_enabled = 1;

#ifdef DEBUG_misha
//int g_VBoxFbgFBreakDdi = 0;
#define DebugBreak() Assert(0)
#endif

void __getHostInfo( void )
{
    char *temp;
    /* on windows guests we're typically get called in a context of VBoxOGL!DllMain ( which calls VBoxOGLcrutil!crNetInit ),
     * which may lead to deadlocks..
     * Avoid it as it is needed for debugging purposes only */
#if !defined(IN_GUEST) || !defined(RT_OS_WINDOWS)
    if ( crGetHostname( my_hostname, sizeof( my_hostname ) ) )
#endif
    {
        crStrcpy( my_hostname, "????" );
    }
    temp = crStrchr( my_hostname, '.' );
    if (temp)
    {
        *temp = '\0';
    }
    my_pid = crGetPID();
}

static void __crCheckCanada(void)
{
    static int first = 1;
    if (first)
    {
        const char *env = crGetenv( "CR_CANADA" );
        if (env)
            canada = 1;
        first = 0;
    }
}

static void __crCheckSwedishChef(void)
{
    static int first = 1;
    if (first)
    {
        const char *env = crGetenv( "CR_SWEDEN" );
        if (env)
            swedish_chef = 1;
        first = 0;
    }
}

static void __crCheckAustralia(void)
{
    static int first = 1;
    if (first)
    {
        const char *env = crGetenv( "CR_AUSTRALIA" );
        const char *env2 = crGetenv( "CR_AUSSIE" );
        if (env || env2)
            australia = 1;
        first = 0;
    }
}

static void outputChromiumMessage( FILE *output, char *str )
{
    fprintf( output, "%s%s%s%s\n", str,
            swedish_chef ? " BORK BORK BORK!" : "",
            canada ? ", eh?" : "",
            australia ? ", mate!" : ""
            );
    fflush( output );
}

#ifdef WINDOWS
static void crRedirectIOToConsole()
{
    int hConHandle;
    HANDLE StdHandle;
    FILE *fp;

    AllocConsole();

    StdHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    hConHandle = _open_osfhandle((long)StdHandle, _O_TEXT);
    fp = _fdopen( hConHandle, "w" );
    *stdout = *fp;
    *stderr = *fp;

    StdHandle = GetStdHandle(STD_INPUT_HANDLE);
    hConHandle = _open_osfhandle((long)StdHandle, _O_TEXT);
    fp = _fdopen( hConHandle, "r" );
    *stdin = *fp;
}
#endif


DECLEXPORT(void) crError(const char *format, ... )
{
    va_list args;
    static char txt[8092];
    int offset;
#ifdef WINDOWS
    DWORD err;
#endif

    __crCheckCanada();
    __crCheckSwedishChef();
    __crCheckAustralia();
    if (!my_hostname[0])
        __getHostInfo();
#ifdef WINDOWS
    if ((err = GetLastError()) != 0 && crGetenv( "CR_WINDOWS_ERRORS" ) != NULL )
    {
        static char buf[8092], *temp;

        SetLastError(0);
        sprintf( buf, "err=%d", err );

        FormatMessage( FORMAT_MESSAGE_ALLOCATE_BUFFER |
                FORMAT_MESSAGE_FROM_SYSTEM |
                FORMAT_MESSAGE_MAX_WIDTH_MASK, NULL, err,
                MAKELANGID( LANG_NEUTRAL, SUBLANG_DEFAULT ),
                (LPTSTR) &temp, 0, NULL );
        if ( temp )
        {
            crStrncpy( buf, temp, sizeof(buf)-1 );
            buf[sizeof(buf)-1] = 0;
        }

        temp = buf + crStrlen(buf) - 1;
        while ( temp > buf && isspace( *temp ) )
        {
            *temp = '\0';
            temp--;
        }

        offset = sprintf( txt, "\t-----------------------\n\tWindows ERROR: %s\n\t----------------------\nCR Error(%s:%d): ", buf, my_hostname, my_pid );
    }
    else
    {
        offset = sprintf( txt, "OpenGL Error: ");
    }
#else
    offset = sprintf( txt, "OpenGL Error: " );
#endif
    va_start( args, format );
    vsprintf( txt + offset, format, args );
#if defined(IN_GUEST)
    crDebug("%s", txt);
    outputChromiumMessage( stderr, txt );
#else
    LogRel(("%s\n", txt));
#endif
#ifdef WINDOWS
    if (crGetenv( "CR_GUI_ERROR" ) != NULL)
    {
        MessageBox( NULL, txt, "Chromium Error", MB_OK );
    }
    else
    {
#endif
        va_end( args );
#ifdef WINDOWS
    }
#if !defined(DEBUG_leo) && !defined(DEBUG_ll158262) && !defined(DEBUG_misha)
    if (crGetenv( "CR_DEBUG_ON_ERROR" ) != NULL)
#endif
    {
        DebugBreak();
    }
#endif

#ifdef IN_GUEST
    /* Give chance for things to close down */
    raise( SIGTERM );

    exit(1);
#endif
}

void crEnableWarnings(int onOff)
{
    warnings_enabled = onOff;
}

#ifdef DEBUG_misha
# undef crWarning
#endif
DECLEXPORT(void) crWarning(const char *format, ... )
{
    if (warnings_enabled) {
        va_list args;
        static char txt[8092];
        int offset;

        __crCheckCanada();
        __crCheckSwedishChef();
        __crCheckAustralia();
        if (!my_hostname[0])
            __getHostInfo();
        offset = sprintf( txt, "OpenGL Warning: ");
        va_start( args, format );
        vsprintf( txt + offset, format, args );
#if defined(IN_GUEST)
        crDebug("%s", txt);
        outputChromiumMessage( stderr, txt );
#else
        LogRel(("%s\n", txt));
#endif
        va_end( args );

#if defined(WINDOWS) && defined(DEBUG) && !defined(IN_GUEST) && defined(DEBUG_misha)
        DebugBreak();
#endif
    }
}

DECLEXPORT(void) crInfo(const char *format, ... )
{
    va_list args;
    static char txt[8092];
    int offset;

    __crCheckCanada();
    __crCheckSwedishChef();
    __crCheckAustralia();
    if (!my_hostname[0])
        __getHostInfo();
    offset = sprintf( txt, "OpenGL Info: ");
    va_start( args, format );
    vsprintf( txt + offset, format, args );
#if defined(IN_GUEST)
    crDebug("%s", txt);
    outputChromiumMessage( stderr, txt );
#else
    LogRel(("%s\n", txt));
#endif
    va_end( args );
}

#ifdef CR_DEBUG_BACKDOOR_ENABLE
static DECLCALLBACK(void) crDebugBackdoorRt(char* pcszStr)
{
    RTLogBackdoorPrintf("%s", pcszStr);
}

static DECLCALLBACK(void) crDebugBackdoorDispMp(char* pcszStr)
{
    VBoxDispMpLoggerLog(pcszStr);
}
#endif


#if defined(WINDOWS) /* && (!defined(DEBUG_misha) || !defined(IN_GUEST) ) */
# define CR_DEBUG_DBGPRINT_ENABLE
#endif

#ifdef CR_DEBUG_DBGPRINT_ENABLE
static void crDebugDbgPrint(const char *str)
{
    OutputDebugString(str);
    OutputDebugString("\n");
}

static void crDebugDbgPrintF(const char * szString, ...)
{
    char szBuffer[4096] = {0};
    va_list pArgList;
    va_start(pArgList, szString);
    vsprintf( szBuffer, szString, pArgList );
    va_end(pArgList);

    OutputDebugStringA(szBuffer);
}

static void crDebugDmlPrint(const char* pszDesc, const char* pszCmd)
{
    crDebugDbgPrintF("<?dml?><exec cmd=\"%s\">%s</exec>, ( %s )\n", pszCmd, pszDesc, pszCmd);
}


DECLEXPORT(void) crDbgCmdPrint(const char *description1, const char *description2, const char *cmd, ...)
{
    va_list args;
    char aTxt[8092];
    char aCmd[8092];

    sprintf( aTxt, "%s%s", description1, description2 );

    va_start( args, cmd );

    vsprintf( aCmd, cmd, args );

    va_end( args );

    crDebugDmlPrint(aTxt, aCmd);

    crDebug("%s: %s", aTxt, aCmd);
}

DECLEXPORT(void) crDbgCmdSymLoadPrint(const char *modName, const void*pvAddress)
{
    static bool fEnable = false;
    static bool fInitialized = false;
    const char * pszName;
    static const char * pszModulePath = NULL;

    if (!fInitialized)
    {
#ifndef DEBUG_misha
        if (crGetenv( "CR_DEBUG_MODULE_ENABLE" ))
#endif
        {
            fEnable = true;
        }

        fInitialized = true;
    }

    if (!fEnable)
        return;

    pszName = PathFindFileNameA(modName);

    if (!pszModulePath)
        pszModulePath = crGetenv("CR_DEBUG_MODULE_PATH");
    if (!pszModulePath)
        pszModulePath = "c:\\Users\\senmk\\Downloads\\Data\\Data";

    crDbgCmdPrint("load modules for ", pszName, ".reload /i /f %s\\%s=%#p", pszModulePath, pszName, pvAddress);
}

#endif

DECLEXPORT(void) crDebug(const char *format, ... )
{
    va_list args;
    static char txt[8092];
    int offset;
#ifdef WINDOWS
    DWORD err;
#endif
    static FILE *output;
    static int first_time = 1;
    static int silent = 0;
#ifdef CR_DEBUG_BACKDOOR_ENABLE
    typedef DECLCALLBACK(void) FNCRGEDUGBACKDOOR(char* pcszStr);
    typedef FNCRGEDUGBACKDOOR *PFNCRGEDUGBACKDOOR;
    static PFNCRGEDUGBACKDOOR pfnLogBackdoor = NULL;
#endif
#ifdef CR_DEBUG_DBGPRINT_ENABLE
    static int dbgPrintEnable = 0;
#endif

    if (first_time)
    {
        const char *fname = crGetenv( "CR_DEBUG_FILE" );
        const char *fnamePrefix = crGetenv( "CR_DEBUG_FILE_PREFIX" );
        char str[2048];
#ifdef CR_DEBUG_CONSOLE_ENABLE
        int logToConsole = 0;
#endif
#ifdef CR_DEBUG_BACKDOOR_ENABLE
        if (crGetenv( "CR_DEBUG_BACKDOOR" ))
        {
            int rc = VBoxDispMpLoggerInit();
            if (RT_SUCCESS(rc))
                pfnLogBackdoor = crDebugBackdoorDispMp;
            else
                pfnLogBackdoor = crDebugBackdoorRt;
        }
#endif
#ifdef CR_DEBUG_DBGPRINT_ENABLE
        if (crGetenv( "CR_DEBUG_DBGPRINT" ))
        {
            dbgPrintEnable = 1;
        }
#endif

        if (!fname && fnamePrefix)
        {
            char pname[1024];
            if (crStrlen(fnamePrefix) < sizeof (str) - sizeof (pname) - 20)
            {
                crGetProcName(pname, 1024);
                sprintf(str,
#ifdef RT_OS_WINDOWS
                "%s_%s_%u.txt", fnamePrefix, pname, GetCurrentProcessId()
#else
                "%s_%s_%lu.txt", fnamePrefix, pname, crGetPID()
#endif
                );
                fname = &str[0];
            }
        }

        first_time = 0;
        if (fname)
        {
            char debugFile[2048], *p;
            crStrcpy(debugFile, fname);
            p = crStrstr(debugFile, "%p");
            if (p) {
                /* replace %p with process number */
                unsigned long n = (unsigned long) crGetPID();
                sprintf(p, "%lu", n);
            }
            fname = debugFile;
            output = fopen( fname, "w" );
            if (!output)
            {
                crError( "Couldn't open debug log %s", fname );
            }
        }
        else
        {
#ifdef CR_DEBUG_CONSOLE_ENABLE
            if (crGetenv( "CR_DEBUG_CONSOLE" ))
            {
                crRedirectIOToConsole();
                logToConsole = 1;
            }
#endif
            output = stderr;
        }

#if !defined(DEBUG)/* || defined(DEBUG_misha)*/
        /* Release mode: only emit crDebug messages if CR_DEBUG
         * or CR_DEBUG_FILE is set.
         */
        if (!fname && !crGetenv("CR_DEBUG")
#ifdef CR_DEBUG_CONSOLE_ENABLE
                    && !logToConsole
#endif
#ifdef CR_DEBUG_BACKDOOR_ENABLE
                    && !pfnLogBackdoor
#endif
#ifdef CR_DEBUG_DBGPRINT_ENABLE
                    && !dbgPrintEnable
#endif
                )
            silent = 1;
#endif
    }

    if (silent)
        return;

    __crCheckCanada();
    __crCheckSwedishChef();
    __crCheckAustralia();
    if (!my_hostname[0])
        __getHostInfo();

#ifdef WINDOWS
    if ((err = GetLastError()) != 0 && crGetenv( "CR_WINDOWS_ERRORS" ) != NULL )
    {
        static char buf[8092], *temp;

        SetLastError(0);
        sprintf( buf, "err=%d", err );

        FormatMessage( FORMAT_MESSAGE_ALLOCATE_BUFFER |
                FORMAT_MESSAGE_FROM_SYSTEM |
                FORMAT_MESSAGE_MAX_WIDTH_MASK, NULL, err,
                MAKELANGID( LANG_NEUTRAL, SUBLANG_DEFAULT ),
                (LPTSTR) &temp, 0, NULL );
        if ( temp )
        {
            crStrncpy( buf, temp, sizeof(buf)-1 );
            buf[sizeof(buf)-1] = 0;
        }

        temp = buf + crStrlen(buf) - 1;
        while ( temp > buf && isspace( *temp ) )
        {
            *temp = '\0';
            temp--;
        }

        offset = sprintf( txt, "\t-----------------------\n\tWindows ERROR: %s\n\t-----------------\nCR Debug(%s:%d): ", buf, my_hostname, my_pid );
    }
    else
    {
        offset = sprintf( txt, "[0x%x.0x%x] OpenGL Debug: ", GetCurrentProcessId(), crThreadID());
    }
#else
    offset = sprintf( txt, "[0x%lx.0x%lx] OpenGL Debug: ", crGetPID(), crThreadID());
#endif
    va_start( args, format );
    vsprintf( txt + offset, format, args );
#ifdef CR_DEBUG_BACKDOOR_ENABLE
    if (pfnLogBackdoor)
    {
        pfnLogBackdoor(txt);
    }
#endif
#ifdef CR_DEBUG_DBGPRINT_ENABLE
    if (dbgPrintEnable)
    {
        crDebugDbgPrint(txt);
    }
#endif
#if defined(IN_GUEST)
    outputChromiumMessage( output, txt );
#else
    if (!output
#ifndef DEBUG_misha
            || output==stderr
#endif
            )
    {
        LogRel(("%s\n", txt));
    }
    else
    {
        LogRel(("%s\n", txt));
        outputChromiumMessage(output, txt);
    }
#endif
    va_end( args );
}

#if defined(DEBUG_misha) && defined(RT_OS_WINDOWS)
BOOL WINAPI DllMain(HINSTANCE hDLLInst, DWORD fdwReason, LPVOID lpvReserved)
{
    (void) lpvReserved;

    switch (fdwReason)
    {
        case DLL_PROCESS_ATTACH:
        {
            char aName[MAX_PATH];
             GetModuleFileNameA(hDLLInst, aName, RT_ELEMENTS(aName));
             crDbgCmdSymLoadPrint(aName, hDLLInst);
            break;
        }
        default:
            break;
    }

    return TRUE;
}
#endif
#endif
