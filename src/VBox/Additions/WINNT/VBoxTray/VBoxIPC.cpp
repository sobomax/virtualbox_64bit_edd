/* $Id: VBoxIPC.cpp $ */
/** @file
 * VBoxIPC - IPC thread, acts as a (purely) local IPC server.
 *           Multiple sessions are supported, whereas every session
 *           has its own thread for processing requests.
 */

/*
 * Copyright (C) 2010-2014 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */
#include <windows.h>
#include "VBoxTray.h"
#include "VBoxTrayMsg.h"
#include "VBoxHelpers.h"
#include "VBoxIPC.h"

#include <iprt/asm.h>
#include <iprt/assert.h>
#include <iprt/critsect.h>
#include <iprt/err.h>
#include <iprt/ldr.h>
#include <iprt/list.h>
#include <iprt/localipc.h>
#include <iprt/mem.h>
#include <VBoxGuestInternal.h>



/**
 * IPC context data.
 */
typedef struct VBOXIPCCONTEXT
{
    /** Pointer to the service environment. */
    const VBOXSERVICEENV      *pEnv;
    /** Handle for the local IPC server. */
    RTLOCALIPCSERVER           hServer;
    /** Critical section serializing access to the session list, the state,
     * the response event, the session event, and the thread event. */
    RTCRITSECT                 CritSect;
    /** List of all active IPC sessions. */
    RTLISTANCHOR               SessionList;

} VBOXIPCCONTEXT, *PVBOXIPCCONTEXT;
static VBOXIPCCONTEXT gCtx = {0};

/** Function pointer for GetLastInputInfo(). */
typedef BOOL (WINAPI *PFNGETLASTINPUTINFO)(PLASTINPUTINFO);

/**
 * IPC per-session thread data.
 */
typedef struct VBOXIPCSESSION
{
    /** The list node required to be part of the
     *  IPC session list. */
    RTLISTNODE                          Node;
    /** Pointer to the IPC context data. */
    PVBOXIPCCONTEXT volatile            pCtx;
    /** The local ipc client handle. */
    RTLOCALIPCSESSION volatile          hSession;
    /** Indicate that the thread should terminate ASAP. */
    bool volatile                       fTerminate;
    /** The thread handle. */
    RTTHREAD                            hThread;

} VBOXIPCSESSION, *PVBOXIPCSESSION;

/** Static pointer to GetLastInputInfo() function. */
static PFNGETLASTINPUTINFO s_pfnGetLastInputInfo = NULL;

int vboxIPCSessionStop(PVBOXIPCSESSION pSession);

static int vboxIPCHandleVBoxTrayRestart(PVBOXIPCSESSION pSession, PVBOXTRAYIPCHEADER pHdr)
{
    AssertPtrReturn(pSession, VERR_INVALID_POINTER);
    AssertPtrReturn(pHdr, VERR_INVALID_POINTER);

    /** @todo Not implemented yet; don't return an error here. */
    return VINF_SUCCESS;
}

static int vboxIPCHandleShowBalloonMsg(PVBOXIPCSESSION pSession, PVBOXTRAYIPCHEADER pHdr)
{
    AssertPtrReturn(pSession, VERR_INVALID_POINTER);
    AssertPtrReturn(pHdr, VERR_INVALID_POINTER);
    AssertReturn(pHdr->uMsgLen > 0, VERR_INVALID_PARAMETER);

    VBOXTRAYIPCMSG_SHOWBALLOONMSG ipcMsg;
    int rc = RTLocalIpcSessionRead(pSession->hSession, &ipcMsg, pHdr->uMsgLen,
                                   NULL /* Exact read, blocking */);
    if (RT_SUCCESS(rc))
    {
        /* Showing the balloon tooltip is not critical. */
        int rc2 = hlpShowBalloonTip(ghInstance, ghwndToolWindow, ID_TRAYICON,
                                    ipcMsg.szMsgContent, ipcMsg.szMsgTitle,
                                    ipcMsg.uShowMS, ipcMsg.uType);
        LogFlowFunc(("Showing \"%s\" - \"%s\" (type %RU32, %RU32ms), rc=%Rrc\n",
                     ipcMsg.szMsgTitle, ipcMsg.szMsgContent,
                     ipcMsg.uType, ipcMsg.uShowMS, rc2));
    }

    return rc;
}

static int vboxIPCHandleUserLastInput(PVBOXIPCSESSION pSession, PVBOXTRAYIPCHEADER pHdr)
{
    AssertPtrReturn(pSession, VERR_INVALID_POINTER);
    AssertPtrReturn(pHdr, VERR_INVALID_POINTER);
    /* No actual message from client. */

    int rc = VINF_SUCCESS;

    bool fLastInputAvailable = false;
    VBOXTRAYIPCRES_USERLASTINPUT ipcRes;
    if (s_pfnGetLastInputInfo)
    {
        /* Note: This only works up to 49.7 days (= 2^32, 32-bit counter)
           since Windows was started. */
        LASTINPUTINFO lastInput;
        lastInput.cbSize = sizeof(LASTINPUTINFO);
        BOOL fRc = s_pfnGetLastInputInfo(&lastInput);
        if (fRc)
        {
            ipcRes.uLastInput = (GetTickCount() - lastInput.dwTime) / 1000;
            fLastInputAvailable = true;
        }
        else
            rc = RTErrConvertFromWin32(GetLastError());
    }

    if (!fLastInputAvailable)
    {
        /* No last input available. */
        ipcRes.uLastInput = UINT32_MAX;
    }

    int rc2 = RTLocalIpcSessionWrite(pSession->hSession, &ipcRes, sizeof(ipcRes));
    if (RT_SUCCESS(rc))
        rc = rc2;

    return rc;
}

/**
 * Initializes the IPC communication.
 *
 * @return  IPRT status code.
 * @param   pEnv                        The IPC service's environment.
 * @param   ppInstance                  The instance pointer which refer to this object.
 * @param   pfStartThread               Pointer to flag whether the IPC service can be started or not.
 */
int VBoxIPCInit(const VBOXSERVICEENV *pEnv, void **ppInstance, bool *pfStartThread)
{
    AssertPtrReturn(pEnv, VERR_INVALID_POINTER);
    /** ppInstance not used here. */
    AssertPtrReturn(pfStartThread, VERR_INVALID_POINTER);

    LogFlowFuncEnter();

    *pfStartThread = false;

    int rc = RTCritSectInit(&gCtx.CritSect);
    if (RT_SUCCESS(rc))
    {
        RTUTF16 wszUserName[255];
        DWORD cchUserName = sizeof(wszUserName) / sizeof(RTUTF16);
        BOOL fRc = GetUserNameW(wszUserName, &cchUserName);
        if (!fRc)
            rc = RTErrConvertFromWin32(GetLastError());

        if (RT_SUCCESS(rc))
        {
            char *pszUserName;
            rc = RTUtf16ToUtf8(wszUserName, &pszUserName);
            if (RT_SUCCESS(rc))
            {
                char szPipeName[255];
                if (RTStrPrintf(szPipeName, sizeof(szPipeName), "%s%s",
                                VBOXTRAY_IPC_PIPE_PREFIX, pszUserName))
                {
                    rc = RTLocalIpcServerCreate(&gCtx.hServer, szPipeName,
                                                RTLOCALIPC_FLAGS_MULTI_SESSION);
                    if (RT_SUCCESS(rc))
                    {
                        RTStrFree(pszUserName);

                        gCtx.pEnv = pEnv;
                        RTListInit(&gCtx.SessionList);

                        *ppInstance = &gCtx;
                        *pfStartThread = true;

                        /* GetLastInputInfo only is available starting at Windows 2000. */
                        s_pfnGetLastInputInfo = (PFNGETLASTINPUTINFO)
                            RTLdrGetSystemSymbol("User32.dll", "GetLastInputInfo");

                        LogRelFunc(("Local IPC server now running at \"%s\"\n",
                                    szPipeName));
                        return VINF_SUCCESS;
                    }

                }
                else
                    rc = VERR_NO_MEMORY;

                RTStrFree(pszUserName);
            }
        }

        RTCritSectDelete(&gCtx.CritSect);
    }

    LogRelFunc(("Creating local IPC server failed with rc=%Rrc\n", rc));
    return rc;
}

void VBoxIPCStop(const VBOXSERVICEENV *pEnv, void *pInstance)
{
    AssertPtrReturnVoid(pEnv);
    AssertPtrReturnVoid(pInstance);

    LogFunc(("Stopping pInstance=%p\n", pInstance));

    /* Shut down local IPC server. */
    PVBOXIPCCONTEXT pCtx = (PVBOXIPCCONTEXT)pInstance;
    AssertPtr(pCtx);

    if (pCtx->hServer != NIL_RTLOCALIPCSERVER)
    {
        int rc2 = RTLocalIpcServerCancel(pCtx->hServer);
        if (RT_FAILURE(rc2))
            LogFunc(("Cancelling current listening call failed with rc=%Rrc\n", rc2));
    }

    /* Stop all remaining session threads. */
    int rc = RTCritSectEnter(&pCtx->CritSect);
    if (RT_SUCCESS(rc))
    {
        PVBOXIPCSESSION pSession;
        RTListForEach(&pCtx->SessionList, pSession, VBOXIPCSESSION, Node)
        {
            int rc2 = vboxIPCSessionStop(pSession);
            if (RT_FAILURE(rc2))
            {
                LogFunc(("Stopping IPC session %p failed with rc=%Rrc\n",
                         pSession, rc2));
                /* Keep going. */
            }
        }
    }
}

void VBoxIPCDestroy(const VBOXSERVICEENV *pEnv, void *pInstance)
{
    AssertPtrReturnVoid(pEnv);
    AssertPtrReturnVoid(pInstance);

    LogFunc(("Destroying pInstance=%p\n", pInstance));

    PVBOXIPCCONTEXT pCtx = (PVBOXIPCCONTEXT)pInstance;
    AssertPtr(pCtx);

    /* Shut down local IPC server. */
    int rc = RTCritSectEnter(&pCtx->CritSect);
    if (RT_SUCCESS(rc))
    {
        rc = RTLocalIpcServerDestroy(pCtx->hServer);
        if (RT_FAILURE(rc))
            LogFunc(("Unable to destroy IPC server, rc=%Rrc\n", rc));

        int rc2 = RTCritSectLeave(&pCtx->CritSect);
        if (RT_SUCCESS(rc))
            rc = rc2;
    }

    LogFunc(("Waiting for remaining IPC sessions to shut down ...\n"));

    /* Wait for all IPC session threads to shut down. */
    bool fListIsEmpty = true;
    do
    {
        int rc2 = RTCritSectEnter(&pCtx->CritSect);
        if (RT_SUCCESS(rc2))
        {
            fListIsEmpty = RTListIsEmpty(&pCtx->SessionList);
            rc2 = RTCritSectLeave(&pCtx->CritSect);

            if (!fListIsEmpty) /* Don't hog CPU while waiting. */
                RTThreadSleep(100);
        }

        if (RT_FAILURE(rc2))
            break;

    } while (!fListIsEmpty);

    AssertMsg(fListIsEmpty,
              ("Session thread list is not empty when it should\n"));

    LogFunc(("All remaining IPC sessions shut down\n"));

    int rc2 = RTCritSectDelete(&pCtx->CritSect);
    if (RT_SUCCESS(rc))
        rc = rc2;

    LogFunc(("Destroyed pInstance=%p, rc=%Rrc\n",
             pInstance, rc));
}

/**
 * Services a client session.
 *
 * @returns VINF_SUCCESS.
 * @param   hThread         The thread handle.
 * @param   pvSession       Pointer to the session instance data.
 */
static DECLCALLBACK(int) vboxIPCSessionThread(RTTHREAD hThread, void *pvSession)
{
    PVBOXIPCSESSION pThis = (PVBOXIPCSESSION)pvSession;
    AssertPtrReturn(pThis, VERR_INVALID_POINTER);
    RTLOCALIPCSESSION hSession = pThis->hSession;
    AssertReturn(hSession != NIL_RTLOCALIPCSESSION, VERR_INVALID_PARAMETER);

    LogFunc(("pThis=%p\n", pThis));

    int rc = VINF_SUCCESS;

    /*
     * Process client requests until it quits or we're cancelled on termination.
     */
    while (   !ASMAtomicUoReadBool(&pThis->fTerminate)
           && RT_SUCCESS(rc))
    {
        /* The next call will be cancelled via VBoxIPCStop if needed. */
        rc = RTLocalIpcSessionWaitForData(hSession, RT_INDEFINITE_WAIT);
        if (RT_FAILURE(rc))
        {
            if (rc == VERR_CANCELLED)
            {
                LogFunc(("Session %p: Waiting for data cancelled\n", pThis));
                rc = VINF_SUCCESS;
                break;
            }
            else
                LogFunc(("Session %p: Waiting for session data failed with rc=%Rrc\n",
                         pThis, rc));
        }
        else
        {
            VBOXTRAYIPCHEADER ipcHdr;
            rc = RTLocalIpcSessionRead(hSession, &ipcHdr, sizeof(ipcHdr),
                                       NULL /* Exact read, blocking */);
            bool fRejected = false; /* Reject current command? */
            if (RT_SUCCESS(rc))
                fRejected =    ipcHdr.uMagic != VBOXTRAY_IPC_HDR_MAGIC
                            || ipcHdr.uHdrVersion != 0; /* We only know version 0 commands for now. */

            if (   !fRejected
                && RT_SUCCESS(rc))
            {
                switch (ipcHdr.uMsgType)
                {
                    case VBOXTRAYIPCMSGTYPE_RESTART:
                        rc = vboxIPCHandleVBoxTrayRestart(pThis, &ipcHdr);
                        break;

                    case VBOXTRAYIPCMSGTYPE_SHOWBALLOONMSG:
                        rc = vboxIPCHandleShowBalloonMsg(pThis, &ipcHdr);
                        break;

                    case VBOXTRAYIPCMSGTYPE_USERLASTINPUT:
                        rc = vboxIPCHandleUserLastInput(pThis, &ipcHdr);
                        break;

                    default:
                    {
                        /* Unknown command, reject. */
                        fRejected = true;
                        break;
                    }
                }

                if (RT_FAILURE(rc))
                    LogFunc(("Session %p: Handling command %RU32 failed with rc=%Rrc\n",
                             pThis, ipcHdr.uMsgType, rc));
            }

            if (fRejected)
            {
                static int s_cRejectedCmds = 0;
                if (++s_cRejectedCmds <= 3)
                {
                    LogRelFunc(("Session %p: Received invalid/unknown command %RU32 (%RU32 bytes), rejecting (%RU32/3)\n",
                                pThis, ipcHdr.uMsgType, ipcHdr.uMsgLen, s_cRejectedCmds + 1));
                    if (ipcHdr.uMsgLen)
                    {
                        /* Get and discard payload data. */
                        size_t cbRead;
                        uint8_t devNull[_1K];
                        while (ipcHdr.uMsgLen)
                        {
                            rc = RTLocalIpcSessionRead(hSession, &devNull, sizeof(devNull), &cbRead);
                            if (RT_FAILURE(rc))
                                break;
                            AssertRelease(cbRead <= ipcHdr.uMsgLen);
                            ipcHdr.uMsgLen -= (uint32_t)cbRead;
                        }
                    }
                }
                else
                    rc = VERR_INVALID_PARAMETER; /* Enough fun, bail out. */
            }
        }
    }

    LogFunc(("Session %p: Handler ended with rc=%Rrc\n",
             pThis, rc));

    /*
     * Close the session.
     */
    int rc2 = RTLocalIpcSessionClose(hSession);
    if (RT_FAILURE(rc2))
        LogFunc(("Session %p: Failed closing session %p, rc=%Rrc\n", pThis, rc2));

    /*
     * Clean up the session.
     */
    PVBOXIPCCONTEXT pCtx = ASMAtomicReadPtrT(&pThis->pCtx, PVBOXIPCCONTEXT);
    AssertMsg(pCtx, ("Session %p: No context found\n", pThis));
    rc2 = RTCritSectEnter(&pCtx->CritSect);
    if (RT_SUCCESS(rc2))
    {
        /* Remove this session from the session list. */
        RTListNodeRemove(&pThis->Node);

        rc2 = RTCritSectLeave(&pCtx->CritSect);
        if (RT_SUCCESS(rc))
            rc = rc2;
    }

    LogFunc(("Session %p: Terminated with rc=%Rrc, freeing ...\n",
             pThis, rc));

    RTMemFree(pThis);
    pThis = NULL;

    return rc;
}

static int vboxIPCSessionCreate(PVBOXIPCCONTEXT pCtx, RTLOCALIPCSESSION hSession)
{
    AssertPtrReturn(pCtx, VERR_INVALID_POINTER);
    AssertReturn(hSession != NIL_RTLOCALIPCSESSION, VERR_INVALID_PARAMETER);

    int rc = RTCritSectEnter(&pCtx->CritSect);
    if (RT_SUCCESS(rc))
    {
        PVBOXIPCSESSION pSession = (PVBOXIPCSESSION)RTMemAllocZ(sizeof(VBOXIPCSESSION));
        if (pSession)
        {
            pSession->pCtx = pCtx;
            pSession->hSession = hSession;
            pSession->fTerminate = false;
            pSession->hThread = NIL_RTTHREAD;

            /* Start IPC session thread. */
            LogFlowFunc(("Creating thread for session %p ...\n", pSession));
            rc = RTThreadCreate(&pSession->hThread, vboxIPCSessionThread,
                                pSession /* pvUser */, 0 /* Default stack size */,
                                RTTHREADTYPE_DEFAULT, 0 /* Flags */, "VBXTRYIPCSESS");
            if (RT_SUCCESS(rc))
            {
                /* Add session thread to session IPC list. */
                RTListAppend(&pCtx->SessionList, &pSession->Node);
            }
            else
            {
                int rc2 = RTLocalIpcSessionClose(hSession);
                if (RT_FAILURE(rc2))
                    LogFunc(("Failed closing session %p, rc=%Rrc\n", pSession, rc2));

                LogFunc(("Failed to create thread for session %p, rc=%Rrc\n", pSession, rc));
                RTMemFree(pSession);
            }
        }
        else
            rc = VERR_NO_MEMORY;

        int rc2 = RTCritSectLeave(&pCtx->CritSect);
        AssertRC(rc2);
    }

    return rc;
}

static int vboxIPCSessionStop(PVBOXIPCSESSION pSession)
{
    AssertPtrReturn(pSession, VERR_INVALID_POINTER);

    ASMAtomicWriteBool(&pSession->fTerminate, true);

    RTLOCALIPCSESSION hSession;
    ASMAtomicXchgHandle(&pSession->hSession, NIL_RTLOCALIPCSESSION, &hSession);
    if (hSession)
        return RTLocalIpcSessionClose(hSession);

    return VINF_SUCCESS;
}

/**
 * Thread function to wait for and process seamless mode change
 * requests
 */
unsigned __stdcall VBoxIPCThread(void *pInstance)
{
    LogFlowFuncEnter();

    PVBOXIPCCONTEXT pCtx = (PVBOXIPCCONTEXT)pInstance;
    AssertPtr(pCtx);

    bool fShutdown = false;
    for (;;)
    {
        RTLOCALIPCSESSION hClientSession = NIL_RTLOCALIPCSESSION;
        int rc = RTLocalIpcServerListen(pCtx->hServer, &hClientSession);
        if (RT_FAILURE(rc))
        {
            if (rc == VERR_CANCELLED)
            {
                LogFlow(("Cancelled\n"));
                fShutdown = true;
            }
            else
                LogRelFunc(("Listening failed with rc=%Rrc\n", rc));
        }

        if (fShutdown)
            break;
        rc = vboxIPCSessionCreate(pCtx, hClientSession);
        if (RT_FAILURE(rc))
        {
            LogRelFunc(("Creating new IPC server session failed with rc=%Rrc\n", rc));
            /* Keep going. */
        }

        AssertPtr(pCtx->pEnv);
        if (WaitForSingleObject(pCtx->pEnv->hStopEvent, 0 /* No waiting */) == WAIT_OBJECT_0)
            break;
    }

    LogFlowFuncLeave();
    return 0;
}

