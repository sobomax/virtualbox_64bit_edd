/* $Id: VBoxServiceControl.cpp $ */
/** @file
 * VBoxServiceControl - Host-driven Guest Control.
 */

/*
 * Copyright (C) 2012-2013 Oracle Corporation
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
#include <iprt/asm.h>
#include <iprt/assert.h>
#include <iprt/env.h>
#include <iprt/file.h>
#include <iprt/getopt.h>
#include <iprt/mem.h>
#include <iprt/path.h>
#include <iprt/process.h>
#include <iprt/semaphore.h>
#include <iprt/thread.h>
#include <VBox/VBoxGuestLib.h>
#include <VBox/HostServices/GuestControlSvc.h>
#include "VBoxServiceInternal.h"
#include "VBoxServiceControl.h"
#include "VBoxServiceUtils.h"

using namespace guestControl;

/*******************************************************************************
*   Global Variables                                                           *
*******************************************************************************/
/** The control interval (milliseconds). */
static uint32_t             g_uControlIntervalMS = 0;
/** The semaphore we're blocking our main control thread on. */
static RTSEMEVENTMULTI      g_hControlEvent = NIL_RTSEMEVENTMULTI;
/** The VM session ID. Changes whenever the VM is restored or reset. */
static uint64_t             g_idControlSession;
/** The guest control service client ID. */
static uint32_t             g_uControlSvcClientID = 0;
/** How many started guest processes are kept into memory for supplying
 *  information to the host. Default is 256 processes. If 0 is specified,
 *  the maximum number of processes is unlimited. */
static uint32_t             g_uControlProcsMaxKept = 256;
/** List of guest control session threads (VBOXSERVICECTRLSESSIONTHREAD).
 *  A guest session thread represents a forked guest session process
 *  of VBoxService.  */
RTLISTANCHOR                g_lstControlSessionThreads;
/** The local session object used for handling all session-related stuff.
 *  When using the legacy guest control protocol (< 2), this session runs
 *  under behalf of the VBoxService main process. On newer protocol versions
 *  each session is a forked version of VBoxService using the appropriate
 *  user credentials for opening a guest session. These forked sessions then
 *  are kept in VBOXSERVICECTRLSESSIONTHREAD structures. */
VBOXSERVICECTRLSESSION      g_Session;

/*******************************************************************************
*   Internal Functions                                                         *
*******************************************************************************/
static int  gstcntlHandleSessionOpen(PVBGLR3GUESTCTRLCMDCTX pHostCtx);
static int  gstcntlHandleSessionClose(PVBGLR3GUESTCTRLCMDCTX pHostCtx);
static void VBoxServiceControlShutdown(void);


/** @copydoc VBOXSERVICE::pfnPreInit */
static DECLCALLBACK(int) VBoxServiceControlPreInit(void)
{
    int rc;
#ifdef VBOX_WITH_GUEST_PROPS
    /*
     * Read the service options from the VM's guest properties.
     * Note that these options can be overridden by the command line options later.
     */
    uint32_t uGuestPropSvcClientID;
    rc = VbglR3GuestPropConnect(&uGuestPropSvcClientID);
    if (RT_FAILURE(rc))
    {
        if (rc == VERR_HGCM_SERVICE_NOT_FOUND) /* Host service is not available. */
        {
            VBoxServiceVerbose(0, "Guest property service is not available, skipping\n");
            rc = VINF_SUCCESS;
        }
        else
            VBoxServiceError("Failed to connect to the guest property service, rc=%Rrc\n", rc);
    }
    else
    {
        VbglR3GuestPropDisconnect(uGuestPropSvcClientID);
    }

    if (rc == VERR_NOT_FOUND) /* If a value is not found, don't be sad! */
        rc = VINF_SUCCESS;
#else
    /* Nothing to do here yet. */
    rc = VINF_SUCCESS;
#endif

    if (RT_SUCCESS(rc))
    {
        /* Init session object. */
        rc = GstCntlSessionInit(&g_Session, 0 /* Flags */);
    }

    return rc;
}


/** @copydoc VBOXSERVICE::pfnOption */
static DECLCALLBACK(int) VBoxServiceControlOption(const char **ppszShort, int argc, char **argv, int *pi)
{
    int rc = -1;
    if (ppszShort)
        /* no short options */;
    else if (!strcmp(argv[*pi], "--control-interval"))
        rc = VBoxServiceArgUInt32(argc, argv, "", pi,
                                  &g_uControlIntervalMS, 1, UINT32_MAX - 1);
#ifdef DEBUG
    else if (!strcmp(argv[*pi], "--control-dump-stdout"))
    {
        g_Session.uFlags |= VBOXSERVICECTRLSESSION_FLAG_DUMPSTDOUT;
        rc = 0; /* Flag this command as parsed. */
    }
    else if (!strcmp(argv[*pi], "--control-dump-stderr"))
    {
        g_Session.uFlags |= VBOXSERVICECTRLSESSION_FLAG_DUMPSTDERR;
        rc = 0; /* Flag this command as parsed. */
    }
#endif
    return rc;
}


/** @copydoc VBOXSERVICE::pfnInit */
static DECLCALLBACK(int) VBoxServiceControlInit(void)
{
    /*
     * If not specified, find the right interval default.
     * Then create the event sem to block on.
     */
    if (!g_uControlIntervalMS)
        g_uControlIntervalMS = 1000;

    int rc = RTSemEventMultiCreate(&g_hControlEvent);
    AssertRCReturn(rc, rc);

    VbglR3GetSessionId(&g_idControlSession);
    /* The status code is ignored as this information is not available with VBox < 3.2.10. */

    if (RT_SUCCESS(rc))
        rc = VbglR3GuestCtrlConnect(&g_uControlSvcClientID);
    if (RT_SUCCESS(rc))
    {
        VBoxServiceVerbose(3, "Guest control service client ID=%RU32\n",
                           g_uControlSvcClientID);

        /* Init session thread list. */
        RTListInit(&g_lstControlSessionThreads);
    }
    else
    {
        /* If the service was not found, we disable this service without
           causing VBoxService to fail. */
        if (rc == VERR_HGCM_SERVICE_NOT_FOUND) /* Host service is not available. */
        {
            VBoxServiceVerbose(0, "Guest control service is not available\n");
            rc = VERR_SERVICE_DISABLED;
        }
        else
            VBoxServiceError("Failed to connect to the guest control service! Error: %Rrc\n", rc);
        RTSemEventMultiDestroy(g_hControlEvent);
        g_hControlEvent = NIL_RTSEMEVENTMULTI;
    }
    return rc;
}


/** @copydoc VBOXSERVICE::pfnWorker */
DECLCALLBACK(int) VBoxServiceControlWorker(bool volatile *pfShutdown)
{
    /*
     * Tell the control thread that it can continue
     * spawning services.
     */
    RTThreadUserSignal(RTThreadSelf());
    Assert(g_uControlSvcClientID > 0);

    int rc = VINF_SUCCESS;

    /* Allocate a scratch buffer for commands which also send
     * payload data with them. */
    uint32_t cbScratchBuf = _64K; /** @todo Make buffer size configurable via guest properties/argv! */
    AssertReturn(RT_IS_POWER_OF_TWO(cbScratchBuf), VERR_INVALID_PARAMETER);
    uint8_t *pvScratchBuf = (uint8_t*)RTMemAlloc(cbScratchBuf);
    AssertPtrReturn(pvScratchBuf, VERR_NO_MEMORY);

    VBGLR3GUESTCTRLCMDCTX ctxHost = { g_uControlSvcClientID };
    /* Set default protocol version to 1. */
    ctxHost.uProtocol = 1;

    for (;;)
    {
        VBoxServiceVerbose(3, "Waiting for host msg ...\n");
        uint32_t uMsg = 0;
        uint32_t cParms = 0;
        rc = VbglR3GuestCtrlMsgWaitFor(g_uControlSvcClientID, &uMsg, &cParms);
        if (rc == VERR_TOO_MUCH_DATA)
        {
#ifdef DEBUG
            VBoxServiceVerbose(4, "Message requires %ld parameters, but only 2 supplied -- retrying request (no error!)...\n", cParms);
#endif
            rc = VINF_SUCCESS; /* Try to get "real" message in next block below. */
        }
        else if (RT_FAILURE(rc))
            VBoxServiceVerbose(3, "Getting host message failed with %Rrc\n", rc); /* VERR_GEN_IO_FAILURE seems to be normal if ran into timeout. */
        if (RT_SUCCESS(rc))
        {
            VBoxServiceVerbose(4, "Msg=%RU32 (%RU32 parms) retrieved\n", uMsg, cParms);

            /* Set number of parameters for current host context. */
            ctxHost.uNumParms = cParms;

            /* Check for VM session change. */
            uint64_t idNewSession = g_idControlSession;
            int rc2 = VbglR3GetSessionId(&idNewSession);
            if (   RT_SUCCESS(rc2)
                && (idNewSession != g_idControlSession))
            {
                VBoxServiceVerbose(1, "The VM session ID changed\n");
                g_idControlSession = idNewSession;

                /* Close all opened guest sessions -- all context IDs, sessions etc.
                 * are now invalid. */
                rc2 = GstCntlSessionClose(&g_Session);
                AssertRC(rc2);
            }

            switch (uMsg)
            {
                case HOST_CANCEL_PENDING_WAITS:
                    VBoxServiceVerbose(1, "We were asked to quit ...\n");
                    break;

                case HOST_SESSION_CREATE:
                    rc = gstcntlHandleSessionOpen(&ctxHost);
                    break;

                case HOST_SESSION_CLOSE:
                    rc = gstcntlHandleSessionClose(&ctxHost);
                    break;

                default:
                {
                    /*
                     * Protocol v1 did not have support for (dedicated)
                     * guest sessions, so all actions need to be performed
                     * under behalf of VBoxService's main executable.
                     *
                     * The global session object then acts as a host for all
                     * started guest processes which bring all their
                     * credentials with them with the actual guest process
                     * execution call.
                     */
                    if (ctxHost.uProtocol == 1)
                    {
                        rc = GstCntlSessionHandler(&g_Session, uMsg, &ctxHost,
                                                   pvScratchBuf, cbScratchBuf, pfShutdown);
                    }
                    else
                    {
                        /*
                         * ... on newer protocols handling all other commands is
                         * up to the guest session fork of VBoxService, so just
                         * skip all not wanted messages here.
                         */
                        rc = VbglR3GuestCtrlMsgSkip(g_uControlSvcClientID);
                        VBoxServiceVerbose(3, "Skipping uMsg=%RU32, cParms=%RU32, rc=%Rrc\n",
                                           uMsg, cParms, rc);
                    }
                    break;
                }
            }
        }

        /* Do we need to shutdown? */
        if (   *pfShutdown
            || (RT_SUCCESS(rc) && uMsg == HOST_CANCEL_PENDING_WAITS))
        {
            break;
        }

        /* Let's sleep for a bit and let others run ... */
        RTThreadYield();
    }

    VBoxServiceVerbose(0, "Guest control service stopped\n");

    /* Delete scratch buffer. */
    if (pvScratchBuf)
        RTMemFree(pvScratchBuf);

    VBoxServiceVerbose(0, "Guest control worker returned with rc=%Rrc\n", rc);
    return rc;
}


static int gstcntlHandleSessionOpen(PVBGLR3GUESTCTRLCMDCTX pHostCtx)
{
    AssertPtrReturn(pHostCtx, VERR_INVALID_POINTER);

    VBOXSERVICECTRLSESSIONSTARTUPINFO ssInfo = { 0 };
    int rc = VbglR3GuestCtrlSessionGetOpen(pHostCtx,
                                           &ssInfo.uProtocol,
                                           ssInfo.szUser,     sizeof(ssInfo.szUser),
                                           ssInfo.szPassword, sizeof(ssInfo.szPassword),
                                           ssInfo.szDomain,   sizeof(ssInfo.szDomain),
                                           &ssInfo.uFlags,    &ssInfo.uSessionID);
    if (RT_SUCCESS(rc))
    {
        /* The session open call has the protocol version the host
         * wants to use. So update the current protocol version with the one the
         * host wants to use in subsequent calls. */
        pHostCtx->uProtocol = ssInfo.uProtocol;
        VBoxServiceVerbose(3, "Client ID=%RU32 now is using protocol %RU32\n",
                           pHostCtx->uClientID, pHostCtx->uProtocol);

        rc = GstCntlSessionThreadCreate(&g_lstControlSessionThreads,
                                        &ssInfo, NULL /* ppSessionThread */);
    }

    if (RT_FAILURE(rc))
    {
        /* Report back on failure. On success this will be done
         * by the forked session thread. */
        int rc2 = VbglR3GuestCtrlSessionNotify(pHostCtx,
                                               GUEST_SESSION_NOTIFYTYPE_ERROR, rc /* uint32_t vs. int */);
        if (RT_FAILURE(rc2))
            VBoxServiceError("Reporting session error status on open failed with rc=%Rrc\n", rc2);
    }

    VBoxServiceVerbose(3, "Opening a new guest session returned rc=%Rrc\n", rc);
    return rc;
}


static int gstcntlHandleSessionClose(PVBGLR3GUESTCTRLCMDCTX pHostCtx)
{
    AssertPtrReturn(pHostCtx, VERR_INVALID_POINTER);

    uint32_t uSessionID, uFlags;
    int rc = VbglR3GuestCtrlSessionGetClose(pHostCtx, &uFlags, &uSessionID);
    if (RT_SUCCESS(rc))
    {
        rc = VERR_NOT_FOUND;

        PVBOXSERVICECTRLSESSIONTHREAD pThread;
        RTListForEach(&g_lstControlSessionThreads, pThread, VBOXSERVICECTRLSESSIONTHREAD, Node)
        {
            if (pThread->StartupInfo.uSessionID == uSessionID)
            {
                rc = GstCntlSessionThreadDestroy(pThread, uFlags);
                break;
            }
        }
#if 0
        if (RT_FAILURE(rc))
        {
            /* Report back on failure. On success this will be done
             * by the forked session thread. */
            int rc2 = VbglR3GuestCtrlSessionNotify(pHostCtx,
                                                   GUEST_SESSION_NOTIFYTYPE_ERROR, rc);
            if (RT_FAILURE(rc2))
            {
                VBoxServiceError("Reporting session error status on close failed with rc=%Rrc\n", rc2);
                if (RT_SUCCESS(rc))
                    rc = rc2;
            }
        }
#endif
        VBoxServiceVerbose(2, "Closing guest session %RU32 returned rc=%Rrc\n",
                           uSessionID, rc);
    }
    else
        VBoxServiceError("Closing guest session %RU32 failed with rc=%Rrc\n",
                         uSessionID, rc);
    return rc;
}


/** @copydoc VBOXSERVICE::pfnStop */
static DECLCALLBACK(void) VBoxServiceControlStop(void)
{
    VBoxServiceVerbose(3, "Stopping ...\n");

    /** @todo Later, figure what to do if we're in RTProcWait(). It's a very
     *        annoying call since doesn't support timeouts in the posix world. */
    if (g_hControlEvent != NIL_RTSEMEVENTMULTI)
        RTSemEventMultiSignal(g_hControlEvent);

    /*
     * Ask the host service to cancel all pending requests for the main
     * control thread so that we can shutdown properly here.
     */
    if (g_uControlSvcClientID)
    {
        VBoxServiceVerbose(3, "Cancelling pending waits (client ID=%u) ...\n",
                           g_uControlSvcClientID);

        int rc = VbglR3GuestCtrlCancelPendingWaits(g_uControlSvcClientID);
        if (RT_FAILURE(rc))
            VBoxServiceError("Cancelling pending waits failed; rc=%Rrc\n", rc);
    }
}


/**
 * Destroys all guest process threads which are still active.
 */
static void VBoxServiceControlShutdown(void)
{
    VBoxServiceVerbose(2, "Shutting down ...\n");

    int rc2 = GstCntlSessionThreadDestroyAll(&g_lstControlSessionThreads,
                                             0 /* Flags */);
    if (RT_FAILURE(rc2))
        VBoxServiceError("Closing session threads failed with rc=%Rrc\n", rc2);

    rc2 = GstCntlSessionClose(&g_Session);
    if (RT_FAILURE(rc2))
        VBoxServiceError("Closing session failed with rc=%Rrc\n", rc2);

    VBoxServiceVerbose(2, "Shutting down complete\n");
}


/** @copydoc VBOXSERVICE::pfnTerm */
static DECLCALLBACK(void) VBoxServiceControlTerm(void)
{
    VBoxServiceVerbose(3, "Terminating ...\n");

    VBoxServiceControlShutdown();

    VBoxServiceVerbose(3, "Disconnecting client ID=%u ...\n",
                       g_uControlSvcClientID);
    VbglR3GuestCtrlDisconnect(g_uControlSvcClientID);
    g_uControlSvcClientID = 0;

    if (g_hControlEvent != NIL_RTSEMEVENTMULTI)
    {
        RTSemEventMultiDestroy(g_hControlEvent);
        g_hControlEvent = NIL_RTSEMEVENTMULTI;
    }
}


/**
 * The 'vminfo' service description.
 */
VBOXSERVICE g_Control =
{
    /* pszName. */
    "control",
    /* pszDescription. */
    "Host-driven Guest Control",
    /* pszUsage. */
#ifdef DEBUG
    "              [--control-dump-stderr] [--control-dump-stdout]\n"
#endif
    "              [--control-interval <ms>]"
    ,
    /* pszOptions. */
#ifdef DEBUG
    "    --control-dump-stderr   Dumps all guest proccesses stderr data to the\n"
    "                            temporary directory.\n"
    "    --control-dump-stdout   Dumps all guest proccesses stdout data to the\n"
    "                            temporary directory.\n"
#endif
    "    --control-interval      Specifies the interval at which to check for\n"
    "                            new control commands. The default is 1000 ms.\n"
    ,
    /* methods */
    VBoxServiceControlPreInit,
    VBoxServiceControlOption,
    VBoxServiceControlInit,
    VBoxServiceControlWorker,
    VBoxServiceControlStop,
    VBoxServiceControlTerm
};

