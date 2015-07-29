/* $Id: VBoxGuestR3LibDragAndDrop.cpp $ */
/** @file
 * VBoxGuestR3Lib - Ring-3 Support Library for VirtualBox guest additions, Drag & Drop.
 */

/*
 * Copyright (C) 2011-2015 Oracle Corporation
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
#include <iprt/path.h>
#include <iprt/dir.h>
#include <iprt/file.h>
#include <iprt/uri.h>
#include <iprt/thread.h>

#include <iprt/cpp/list.h>
#include <iprt/cpp/ministring.h>

#ifdef LOG_GROUP
 #undef LOG_GROUP
#endif
#define LOG_GROUP LOG_GROUP_GUEST_DND
#include <VBox/log.h>

#include <VBox/VBoxGuestLib.h>
#include <VBox/GuestHost/DragAndDrop.h>
#include <VBox/HostServices/DragAndDropSvc.h>

#include "VBGLR3Internal.h"

/* Here all the communication with the host over HGCM is handled platform
 * neutral. Also the receiving of URIs content (directory trees and files) is
 * done here. So the platform code of the guests, should not take care of that.
 *
 * Todo:
 * - Sending dirs/files in the G->H case
 * - Maybe the EOL converting of text MIME types (not fully sure, eventually
 *   better done on the host side)
 */

/******************************************************************************
 *    Private internal functions                                              *
 ******************************************************************************/

static int vbglR3DnDQueryNextHostMessageType(PVBGLR3GUESTDNDCMDCTX pCtx, uint32_t *puMsg, uint32_t *pcParms, bool fWait)
{
    AssertPtrReturn(pCtx,    VERR_INVALID_POINTER);
    AssertPtrReturn(puMsg,   VERR_INVALID_POINTER);
    AssertPtrReturn(pcParms, VERR_INVALID_POINTER);

    DragAndDropSvc::VBOXDNDNEXTMSGMSG Msg;
    RT_ZERO(Msg);
    Msg.hdr.result      = VERR_WRONG_ORDER;
    Msg.hdr.u32ClientID = pCtx->uClientID;
    Msg.hdr.u32Function = DragAndDropSvc::GUEST_DND_GET_NEXT_HOST_MSG;
    Msg.hdr.cParms      = 3;

    Msg.msg.SetUInt32(0);
    Msg.num_parms.SetUInt32(0);
    Msg.block.SetUInt32(fWait ? 1 : 0);

    int rc = vbglR3DoIOCtl(VBOXGUEST_IOCTL_HGCM_CALL(sizeof(Msg)), &Msg, sizeof(Msg));
    if (RT_SUCCESS(rc))
    {
        rc = Msg.hdr.result;
        if (RT_SUCCESS(rc))
        {
            rc = Msg.msg.GetUInt32(puMsg);         AssertRC(rc);
            rc = Msg.num_parms.GetUInt32(pcParms); AssertRC(rc);
        }
    }

    return rc;
}

static int vbglR3DnDHGProcessActionMessage(PVBGLR3GUESTDNDCMDCTX pCtx,
                                           uint32_t  uMsg,
                                           uint32_t *puScreenId,
                                           uint32_t *puX,
                                           uint32_t *puY,
                                           uint32_t *puDefAction,
                                           uint32_t *puAllActions,
                                           char     *pszFormats,
                                           uint32_t  cbFormats,
                                           uint32_t *pcbFormatsRecv)
{
    AssertPtrReturn(pCtx,           VERR_INVALID_POINTER);
    AssertPtrReturn(puScreenId,     VERR_INVALID_POINTER);
    AssertPtrReturn(puX,            VERR_INVALID_POINTER);
    AssertPtrReturn(puY,            VERR_INVALID_POINTER);
    AssertPtrReturn(puDefAction,    VERR_INVALID_POINTER);
    AssertPtrReturn(puAllActions,   VERR_INVALID_POINTER);
    AssertPtrReturn(pszFormats,     VERR_INVALID_POINTER);
    AssertReturn(cbFormats,         VERR_INVALID_PARAMETER);
    AssertPtrReturn(pcbFormatsRecv, VERR_INVALID_POINTER);

    DragAndDropSvc::VBOXDNDHGACTIONMSG Msg;
    RT_ZERO(Msg);
    Msg.hdr.u32ClientID = pCtx->uClientID;
    Msg.hdr.u32Function = uMsg;
    Msg.hdr.cParms      = 7;

    Msg.uScreenId.SetUInt32(0);
    Msg.uX.SetUInt32(0);
    Msg.uY.SetUInt32(0);
    Msg.uDefAction.SetUInt32(0);
    Msg.uAllActions.SetUInt32(0);
    Msg.pvFormats.SetPtr(pszFormats, cbFormats);
    Msg.cFormats.SetUInt32(0);

    int rc = vbglR3DoIOCtl(VBOXGUEST_IOCTL_HGCM_CALL(sizeof(Msg)), &Msg, sizeof(Msg));
    if (RT_SUCCESS(rc))
    {
        rc = Msg.hdr.result;
        if (RT_SUCCESS(rc))
        {
            rc = Msg.uScreenId.GetUInt32(puScreenId);     AssertRC(rc);
            rc = Msg.uX.GetUInt32(puX);                   AssertRC(rc);
            rc = Msg.uY.GetUInt32(puY);                   AssertRC(rc);
            rc = Msg.uDefAction.GetUInt32(puDefAction);   AssertRC(rc);
            rc = Msg.uAllActions.GetUInt32(puAllActions); AssertRC(rc);
            rc = Msg.cFormats.GetUInt32(pcbFormatsRecv);  AssertRC(rc);

            AssertReturn(cbFormats >= *pcbFormatsRecv, VERR_TOO_MUCH_DATA);
        }
    }

    return rc;
}

static int vbglR3DnDHGProcessLeaveMessage(PVBGLR3GUESTDNDCMDCTX pCtx)
{
    AssertPtrReturn(pCtx, VERR_INVALID_POINTER);

    DragAndDropSvc::VBOXDNDHGLEAVEMSG Msg;
    RT_ZERO(Msg);
    Msg.hdr.u32ClientID = pCtx->uClientID;
    Msg.hdr.u32Function = DragAndDropSvc::HOST_DND_HG_EVT_LEAVE;
    Msg.hdr.cParms      = 0;

    int rc = vbglR3DoIOCtl(VBOXGUEST_IOCTL_HGCM_CALL(sizeof(Msg)), &Msg, sizeof(Msg));
    if (RT_SUCCESS(rc))
        rc = Msg.hdr.result;

    return rc;
}

static int vbglR3DnDHGProcessCancelMessage(PVBGLR3GUESTDNDCMDCTX pCtx)
{
    AssertPtrReturn(pCtx, VERR_INVALID_POINTER);

    DragAndDropSvc::VBOXDNDHGCANCELMSG Msg;
    RT_ZERO(Msg);
    Msg.hdr.u32ClientID = pCtx->uClientID;
    Msg.hdr.u32Function = DragAndDropSvc::HOST_DND_HG_EVT_CANCEL;
    Msg.hdr.cParms      = 0;

    int rc = vbglR3DoIOCtl(VBOXGUEST_IOCTL_HGCM_CALL(sizeof(Msg)), &Msg, sizeof(Msg));
    if (RT_SUCCESS(rc))
        rc = Msg.hdr.result;

    return rc;
}

static int vbglR3DnDHGProcessSendDirMessage(PVBGLR3GUESTDNDCMDCTX pCtx,
                                            char     *pszDirname,
                                            uint32_t  cbDirname,
                                            uint32_t *pcbDirnameRecv,
                                            uint32_t *pfMode)
{
    AssertPtrReturn(pCtx,           VERR_INVALID_POINTER);
    AssertPtrReturn(pszDirname,     VERR_INVALID_POINTER);
    AssertReturn(cbDirname,         VERR_INVALID_PARAMETER);
    AssertPtrReturn(pcbDirnameRecv, VERR_INVALID_POINTER);
    AssertPtrReturn(pfMode,         VERR_INVALID_POINTER);

    DragAndDropSvc::VBOXDNDHGSENDDIRMSG Msg;
    RT_ZERO(Msg);
    Msg.hdr.u32ClientID = pCtx->uClientID;
    Msg.hdr.u32Function = DragAndDropSvc::HOST_DND_HG_SND_DIR;
    Msg.hdr.cParms      = 3;

    Msg.pvName.SetPtr(pszDirname, cbDirname);
    Msg.cbName.SetUInt32(0);
    Msg.fMode.SetUInt32(0);

    int rc = vbglR3DoIOCtl(VBOXGUEST_IOCTL_HGCM_CALL(sizeof(Msg)), &Msg, sizeof(Msg));
    if (RT_SUCCESS(rc))
    {
        rc = Msg.hdr.result;
        if (RT_SUCCESS(Msg.hdr.result))
        {
            rc = Msg.cbName.GetUInt32(pcbDirnameRecv); AssertRC(rc);
            rc = Msg.fMode.GetUInt32(pfMode);          AssertRC(rc);

            AssertReturn(cbDirname >= *pcbDirnameRecv, VERR_TOO_MUCH_DATA);
        }
    }

    return rc;
}

static int vbglR3DnDHGProcessSendFileMessage(PVBGLR3GUESTDNDCMDCTX pCtx,
                                             char                 *pszFilename,
                                             uint32_t              cbFilename,
                                             uint32_t             *pcbFilenameRecv,
                                             void                 *pvData,
                                             uint32_t              cbData,
                                             uint32_t             *pcbDataRecv,
                                             uint32_t             *pfMode)
{
    AssertPtrReturn(pCtx,            VERR_INVALID_POINTER);
    AssertPtrReturn(pszFilename,     VERR_INVALID_POINTER);
    AssertReturn(cbFilename,         VERR_INVALID_PARAMETER);
    AssertPtrReturn(pcbFilenameRecv, VERR_INVALID_POINTER);
    AssertPtrReturn(pvData,          VERR_INVALID_POINTER);
    AssertReturn(cbData,             VERR_INVALID_PARAMETER);
    AssertPtrReturn(pcbDataRecv,     VERR_INVALID_POINTER);
    AssertPtrReturn(pfMode,          VERR_INVALID_POINTER);

    DragAndDropSvc::VBOXDNDHGSENDFILEDATAMSG Msg;
    RT_ZERO(Msg);
    Msg.hdr.u32ClientID = pCtx->uClientID;
    Msg.hdr.u32Function = DragAndDropSvc::HOST_DND_HG_SND_FILE_DATA;

    if (pCtx->uProtocol <= 1)
    {
        Msg.u.v1.pvName.SetPtr(pszFilename, cbFilename);
        Msg.u.v1.cbName.SetUInt32(cbFilename);
        Msg.u.v1.pvData.SetPtr(pvData, cbData);
        Msg.u.v1.cbData.SetUInt32(cbData);
        Msg.u.v1.fMode.SetUInt32(0);

        Msg.hdr.cParms = 5;
    }
    else
    {
        Msg.u.v2.uContext.SetUInt32(0); /** @todo Not used yet. */
        Msg.u.v2.pvData.SetPtr(pvData, cbData);
        Msg.u.v2.cbData.SetUInt32(cbData);

        Msg.hdr.cParms = 3;
    }

    int rc = vbglR3DoIOCtl(VBOXGUEST_IOCTL_HGCM_CALL(sizeof(Msg)), &Msg, sizeof(Msg));
    if (RT_SUCCESS(rc))
    {
        rc = Msg.hdr.result;
        if (RT_SUCCESS(rc))
        {
            if (pCtx->uProtocol <= 1)
            {
                rc = Msg.u.v1.cbName.GetUInt32(pcbFilenameRecv); AssertRC(rc);
                rc = Msg.u.v1.cbData.GetUInt32(pcbDataRecv);     AssertRC(rc);
                rc = Msg.u.v1.fMode.GetUInt32(pfMode);           AssertRC(rc);

                AssertReturn(cbFilename >= *pcbFilenameRecv, VERR_TOO_MUCH_DATA);
                AssertReturn(cbData     >= *pcbDataRecv,     VERR_TOO_MUCH_DATA);
            }
            else
            {
                rc = Msg.u.v2.cbData.GetUInt32(pcbDataRecv);     AssertRC(rc);
                AssertReturn(cbData     >= *pcbDataRecv,     VERR_TOO_MUCH_DATA);
            }
        }
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}

static int vbglR3DnDHGProcessSendFileHdrMessage(PVBGLR3GUESTDNDCMDCTX  pCtx,
                                                char                  *pszFilename,
                                                uint32_t               cbFilename,
                                                uint32_t              *puFlags,
                                                uint32_t              *pfMode,
                                                uint64_t              *pcbTotal)
{
    AssertPtrReturn(pCtx,        VERR_INVALID_POINTER);
    AssertPtrReturn(pszFilename, VERR_INVALID_POINTER);
    AssertReturn(cbFilename,     VERR_INVALID_PARAMETER);
    AssertPtrReturn(puFlags,     VERR_INVALID_POINTER);
    AssertPtrReturn(pfMode,      VERR_INVALID_POINTER);
    AssertReturn(pcbTotal,       VERR_INVALID_POINTER);

    DragAndDropSvc::VBOXDNDHGSENDFILEHDRMSG Msg;
    RT_ZERO(Msg);
    Msg.hdr.u32ClientID = pCtx->uClientID;
    Msg.hdr.u32Function = DragAndDropSvc::HOST_DND_HG_SND_FILE_HDR;

    int rc;

    if (pCtx->uProtocol <= 1)
    {
        rc = VERR_NOT_SUPPORTED;
    }
    else
    {
        Msg.uContext.SetUInt32(0); /** @todo Not used yet. */
        Msg.pvName.SetPtr(pszFilename, cbFilename);
        Msg.cbName.SetUInt32(cbFilename);
        Msg.uFlags.SetUInt32(0);
        Msg.fMode.SetUInt32(0);
        Msg.cbTotal.SetUInt64(0);

        Msg.hdr.cParms = 6;

        rc = VINF_SUCCESS;
    }

    if (RT_SUCCESS(rc))
        rc = vbglR3DoIOCtl(VBOXGUEST_IOCTL_HGCM_CALL(sizeof(Msg)), &Msg, sizeof(Msg));
    if (RT_SUCCESS(rc))
    {
        rc = Msg.hdr.result;
        if (RT_SUCCESS(rc))
        {
            /** @todo Get context ID. */
            rc = Msg.uFlags.GetUInt32(puFlags);   AssertRC(rc);
            rc = Msg.fMode.GetUInt32(pfMode);     AssertRC(rc);
            rc = Msg.cbTotal.GetUInt64(pcbTotal); AssertRC(rc);
        }
    }

    return rc;
}

static int vbglR3DnDHGProcessURIMessages(PVBGLR3GUESTDNDCMDCTX   pCtx,
                                         void                  **ppvData,
                                         uint32_t                cbData,
                                         size_t                 *pcbDataRecv)
{
    AssertPtrReturn(pCtx,        VERR_INVALID_POINTER);
    AssertPtrReturn(ppvData,     VERR_INVALID_POINTER);
    AssertPtrReturn(cbData,      VERR_INVALID_PARAMETER);
    AssertPtrReturn(pcbDataRecv, VERR_INVALID_POINTER);

    /*
     * Allocate chunk buffer.
     */
    uint32_t cbChunkMax = _64K; /** @todo Make this configurable? */
    void *pvChunk = RTMemAlloc(cbChunkMax);
    if (!pvChunk)
        return VERR_NO_MEMORY;
    uint32_t cbChunkRead = 0;

    uint64_t cbFileSize    = 0; /* Total file size (in bytes). */
    uint64_t cbFileWritten = 0; /* Written bytes. */

    /*
     * Create and query the (unique) drop target directory in the user's temporary directory.
     */
    DNDDIRDROPPEDFILES dirDroppedFiles;
    const char *pszDropDir;
    int rc = DnDDirDroppedFilesCreateAndOpenTemp(&dirDroppedFiles);
    if (RT_SUCCESS(rc))
        pszDropDir = DnDDirDroppedFilesGetDirAbs(&dirDroppedFiles);

    /*
     * Enter the main loop of retieving files + directories.
     */
    DnDURIList lstURI;
    DnDURIObject objFile(DnDURIObject::File);

    char szPathName[RTPATH_MAX] = { 0 };
    uint32_t cbPathName = 0;
    uint32_t fFlags = 0;
    uint32_t fMode = 0;

    while (RT_SUCCESS(rc))
    {
        uint32_t uNextMsg;
        uint32_t cNextParms;
        rc = vbglR3DnDQueryNextHostMessageType(pCtx, &uNextMsg, &cNextParms, false /* fWait */);
        if (RT_SUCCESS(rc))
        {
            LogFlowFunc(("uNextMsg=%RU32, cNextParms=%RU32\n", uNextMsg, cNextParms));

            switch (uNextMsg)
            {
                case DragAndDropSvc::HOST_DND_HG_SND_DIR:
                {
                    rc = vbglR3DnDHGProcessSendDirMessage(pCtx,
                                                          szPathName,
                                                          sizeof(szPathName),
                                                          &cbPathName,
                                                          &fMode);
                    LogFlowFunc(("HOST_DND_HG_SND_DIR pszPathName=%s, cbPathName=%RU32, fMode=0x%x, rc=%Rrc\n",
                                 szPathName, cbPathName, fMode, rc));

                    char *pszPathAbs = RTPathJoinA(pszDropDir, szPathName);
                    if (pszPathAbs)
                    {
                        rc = RTDirCreate(pszPathAbs, (fMode & RTFS_UNIX_MASK) | RTFS_UNIX_IRWXU, 0);
                        if (RT_SUCCESS(rc))
                            rc = DnDDirDroppedAddDir(&dirDroppedFiles, pszPathAbs);

                        RTStrFree(pszPathAbs);
                    }
                    else
                        rc = VERR_NO_MEMORY;
                    break;
                }
                case DragAndDropSvc::HOST_DND_HG_SND_FILE_HDR:
                case DragAndDropSvc::HOST_DND_HG_SND_FILE_DATA:
                {
                    if (uNextMsg == DragAndDropSvc::HOST_DND_HG_SND_FILE_HDR)
                    {
                        rc = vbglR3DnDHGProcessSendFileHdrMessage(pCtx,
                                                                  szPathName,
                                                                  sizeof(szPathName),
                                                                  &fFlags,
                                                                  &fMode,
                                                                  &cbFileSize);
                        LogFlowFunc(("HOST_DND_HG_SND_FILE_HDR szPathName=%s, fFlags=0x%x, fMode=0x%x, cbFileSize=%RU64, rc=%Rrc\n",
                                     szPathName, fFlags, fMode, cbFileSize, rc));
                    }
                    else
                    {
                        rc = vbglR3DnDHGProcessSendFileMessage(pCtx,
                                                               szPathName,
                                                               sizeof(szPathName),
                                                               &cbPathName,
                                                               pvChunk,
                                                               cbChunkMax,
                                                               &cbChunkRead,
                                                               &fMode);
                        LogFlowFunc(("HOST_DND_HG_SND_FILE_DATA "
                                     "szPathName=%s, cbPathName=%RU32, cbChunkRead=%RU32, fMode=0x%x, rc=%Rrc\n",
                                     szPathName, cbPathName, cbChunkRead, fMode, rc));
                    }

                    if (   RT_SUCCESS(rc)
                        && (   uNextMsg == DragAndDropSvc::HOST_DND_HG_SND_FILE_HDR
                             /* Protocol v1 always sends the file name, so try opening every time. */
                            || pCtx->uProtocol <= 1)
                       )
                    {
                        char *pszPathAbs = RTPathJoinA(pszDropDir, szPathName);
                        if (pszPathAbs)
                        {
                            LogFlowFunc(("Opening pszPathName=%s, cbPathName=%RU32, fMode=0x%x, cbFileSize=%zu\n",
                                         szPathName, cbPathName, fMode, cbFileSize));

                            uint64_t fOpen = RTFILE_O_WRITE | RTFILE_O_DENY_ALL;
                            if (pCtx->uProtocol <= 1)
                                fOpen |= RTFILE_O_OPEN_CREATE | RTFILE_O_APPEND;
                            else
                                fOpen |= RTFILE_O_CREATE_REPLACE;

                            /* Is there already a file open, e.g. in transfer? */
                            if (!objFile.IsOpen())
                            {
                                RTCString strPathAbs(pszPathAbs);
                                rc = objFile.OpenEx(strPathAbs, DnDURIObject::File, DnDURIObject::Target, fOpen,
                                                    (fMode & RTFS_UNIX_MASK) | RTFS_UNIX_IRUSR | RTFS_UNIX_IWUSR);
                                if (RT_SUCCESS(rc))
                                {
                                    rc = DnDDirDroppedAddFile(&dirDroppedFiles, strPathAbs.c_str());
                                    if (RT_SUCCESS(rc))
                                    {
                                        cbFileWritten = 0;

                                        if (pCtx->uProtocol >= 2) /* Set the expected file size. */
                                            objFile.SetSize(cbFileSize);
                                    }
                                }
                            }
                            else
                                rc = VERR_WRONG_ORDER;

                            RTStrFree(pszPathAbs);
                        }
                        else
                            rc = VERR_NO_MEMORY;
                    }

                    if (   RT_SUCCESS(rc)
                        && uNextMsg == DragAndDropSvc::HOST_DND_HG_SND_FILE_DATA)
                    {
                        bool fClose = false;

                        uint32_t cbChunkWritten;
                        rc = objFile.Write(pvChunk, cbChunkRead, &cbChunkWritten);
                        if (RT_SUCCESS(rc))
                        {
                            if (pCtx->uProtocol >= 2)
                            {
                                /* Data transfer complete? Close the file. */
                                fClose = objFile.IsComplete();
                            }
                            else
                                fClose = true; /* Always close the file after each chunk. */

                            cbFileWritten += cbChunkWritten;
                            Assert(cbFileWritten <= cbFileSize);
                        }

                        if (fClose)
                        {
                            LogFlowFunc(("Closing file\n"));
                            objFile.Close();
                        }
                    }
                    break;
                }
                case DragAndDropSvc::HOST_DND_HG_EVT_CANCEL:
                {
                    rc = vbglR3DnDHGProcessCancelMessage(pCtx);
                    if (RT_SUCCESS(rc))
                        rc = VERR_CANCELLED;
                    break;
                }
                default:
                {
                    LogFlowFunc(("Message %RU32 not supported\n", uNextMsg));
                    rc = VERR_NOT_SUPPORTED;
                    break;
                }
            }
        }

        if (RT_FAILURE(rc))
            break;

    } /* while */

    LogFlowFunc(("Loop ended with %Rrc\n", rc));

    /* All URI data processed? */
    if (rc == VERR_NO_DATA)
        rc = VINF_SUCCESS;

    /* Delete chunk buffer again. */
    if (pvChunk)
        RTMemFree(pvChunk);

    /* Cleanup on failure or if the user has canceled the operation or
     * something else went wrong. */
    if (RT_FAILURE(rc))
    {
        int rc2 = DnDDirDroppedFilesRollback(&dirDroppedFiles);
        AssertRC(rc2); /* Not fatal, don't report back to host. */
    }
    else
    {
        /*
         * Now we need to transform the URI list which came from the host into
         * an URI list which also has the final "Dropped Files" directory as a prefix
         * for each URI entry.
         *
         * So patch the old drop data with the new drop directory to let the drop
         * target on the guest can find the files later.
         */
        void  *pvURIData = *ppvData;
        size_t cbURIData = *pcbDataRecv;

        rc = lstURI.RootFromURIData(pvURIData, cbURIData, 0 /* fFlags */);
        if (RT_SUCCESS(rc))
        {
            /* Cleanup the old data and write the new data back to the event. */
            RTMemFree(pvURIData);

            RTCString strData = lstURI.RootToString(pszDropDir);
            Assert(!strData.isEmpty());
            LogFlowFunc(("New URI list now has %zu bytes (formerly %RU32 bytes)\n", strData.length() + 1, cbURIData));

            pvURIData = RTStrDupN(strData.c_str(), strData.length());
            if (pvURIData)
            {
                cbURIData = strData.length() + 1;
            }
            else
                rc = VERR_NO_MEMORY;
        }

        if (RT_SUCCESS(rc))
        {
            *ppvData     = pvURIData;
            *pcbDataRecv = cbURIData;
        }

        /** @todo Compare the URI list with the dirs/files we really transferred. */
    }

    /*
     * Close the dropped files directory.
     * Don't try to remove it here, however, as the files are being needed
     * by the client's drag'n drop operation lateron.
     */
    int rc2 = DnDDirDroppedFilesClose(&dirDroppedFiles, false);
    if (RT_FAILURE(rc2)) /* Not fatal, don't report back to host. */
        LogFlowFunc(("Closing dropped files directory failed with %Rrc\n", rc2));

    LogFlowFuncLeaveRC(rc);
    return rc;
}

static int vbglR3DnDHGProcessDataMessageInternal(PVBGLR3GUESTDNDCMDCTX pCtx,
                                                 uint32_t *puScreenId,
                                                 char     *pszFormat,
                                                 uint32_t  cbFormat,
                                                 uint32_t *pcbFormatRecv,
                                                 void     *pvData,
                                                 uint32_t  cbData,
                                                 uint32_t *pcbDataTotal)
{
    AssertPtrReturn(pCtx,          VERR_INVALID_POINTER);
    AssertPtrReturn(puScreenId,    VERR_INVALID_POINTER);
    AssertPtrReturn(pszFormat,     VERR_INVALID_POINTER);
    AssertReturn(cbFormat,         VERR_INVALID_PARAMETER);
    AssertPtrReturn(pcbFormatRecv, VERR_INVALID_POINTER);
    AssertPtrReturn(pvData,        VERR_INVALID_POINTER);
    AssertReturn(cbData,           VERR_INVALID_PARAMETER);
    AssertPtrReturn(pcbDataTotal,  VERR_INVALID_POINTER);

    DragAndDropSvc::VBOXDNDHGSENDDATAMSG Msg;
    RT_ZERO(Msg);
    Msg.hdr.u32ClientID = pCtx->uClientID;
    Msg.hdr.u32Function = DragAndDropSvc::HOST_DND_HG_SND_DATA;
    Msg.hdr.cParms      = 5;

    Msg.uScreenId.SetUInt32(0);
    Msg.pvFormat.SetPtr(pszFormat, cbFormat);
    Msg.cFormat.SetUInt32(0);
    Msg.pvData.SetPtr(pvData, cbData);
    Msg.cbData.SetUInt32(0);

    int rc = vbglR3DoIOCtl(VBOXGUEST_IOCTL_HGCM_CALL(sizeof(Msg)), &Msg, sizeof(Msg));
    if (RT_SUCCESS(rc))
    {
        rc = Msg.hdr.result;
        if (   RT_SUCCESS(rc)
            || rc == VERR_BUFFER_OVERFLOW)
        {
            rc = Msg.uScreenId.GetUInt32(puScreenId);  AssertRC(rc);

            /*
             * In case of VERR_BUFFER_OVERFLOW get the data sizes required
             * for the format + data blocks.
             */
            rc = Msg.cFormat.GetUInt32(pcbFormatRecv); AssertRC(rc);
            rc = Msg.cbData.GetUInt32(pcbDataTotal); AssertRC(rc);

            AssertReturn(cbFormat >= *pcbFormatRecv, VERR_TOO_MUCH_DATA);
            AssertReturn(cbData   >= *pcbDataTotal, VERR_TOO_MUCH_DATA);
        }
    }

    return rc;
}

static int vbglR3DnDHGProcessMoreDataMessageInternal(PVBGLR3GUESTDNDCMDCTX pCtx,
                                                     void     *pvData,
                                                     uint32_t  cbData,
                                                     uint32_t *pcbDataTotal)
{
    AssertPtrReturn(pCtx,         VERR_INVALID_POINTER);
    AssertPtrReturn(pvData,       VERR_INVALID_POINTER);
    AssertReturn(cbData,          VERR_INVALID_PARAMETER);
    AssertPtrReturn(pcbDataTotal, VERR_INVALID_POINTER);

    DragAndDropSvc::VBOXDNDHGSENDMOREDATAMSG Msg;
    RT_ZERO(Msg);
    Msg.hdr.u32ClientID = pCtx->uClientID;
    Msg.hdr.u32Function = DragAndDropSvc::HOST_DND_HG_SND_MORE_DATA;
    Msg.hdr.cParms      = 2;

    Msg.pvData.SetPtr(pvData, cbData);
    Msg.cbData.SetUInt32(0);

    int rc = vbglR3DoIOCtl(VBOXGUEST_IOCTL_HGCM_CALL(sizeof(Msg)), &Msg, sizeof(Msg));
    if (RT_SUCCESS(rc))
    {
        rc = Msg.hdr.result;
        if (   RT_SUCCESS(rc)
            || rc == VERR_BUFFER_OVERFLOW)
        {
            rc = Msg.cbData.GetUInt32(pcbDataTotal); AssertRC(rc);
            AssertReturn(cbData >= *pcbDataTotal, VERR_TOO_MUCH_DATA);
        }
    }
    return rc;
}

static int vbglR3DnDHGProcessSendDataMessageLoop(PVBGLR3GUESTDNDCMDCTX pCtx,
                                                 uint32_t *puScreenId,
                                                 char     *pszFormat,
                                                 uint32_t  cbFormat,
                                                 uint32_t *pcbFormatRecv,
                                                 void    **ppvData,
                                                 uint32_t  cbData,
                                                 size_t   *pcbDataRecv)
{
    AssertPtrReturn(pCtx,          VERR_INVALID_POINTER);
    AssertPtrReturn(puScreenId,    VERR_INVALID_POINTER);
    AssertPtrReturn(pszFormat,     VERR_INVALID_POINTER);
    AssertPtrReturn(pcbFormatRecv, VERR_INVALID_POINTER);
    AssertPtrReturn(ppvData,       VERR_INVALID_POINTER);
    /* pcbDataRecv is optional. */

    uint32_t cbDataReq = 0;
    int rc = vbglR3DnDHGProcessDataMessageInternal(pCtx,
                                                   puScreenId,
                                                   pszFormat,
                                                   cbFormat,
                                                   pcbFormatRecv,
                                                   *ppvData,
                                                   cbData,
                                                   &cbDataReq);
    uint32_t cbDataTotal = cbDataReq;
    void *pvData = *ppvData;

    LogFlowFunc(("HOST_DND_HG_SND_DATA cbDataReq=%RU32, rc=%Rrc\n", cbDataTotal, rc));

    while (rc == VERR_BUFFER_OVERFLOW)
    {
        uint32_t uNextMsg;
        uint32_t cNextParms;
        rc = vbglR3DnDQueryNextHostMessageType(pCtx, &uNextMsg, &cNextParms, false);
        if (RT_SUCCESS(rc))
        {
            switch(uNextMsg)
            {
                case DragAndDropSvc::HOST_DND_HG_SND_MORE_DATA:
                {
                    /** @todo r=andy Don't use reallocate here; can go wrong with *really* big URI lists.
                     *               Instead send as many URI entries as possible per chunk and add those entries
                     *               to our to-process list for immediata processing. Repeat the step after processing then. */
                    LogFlowFunc(("HOST_DND_HG_SND_MORE_DATA cbDataTotal: %RU32 -> %RU32\n", cbDataReq, cbDataReq + cbData));
                    pvData = RTMemRealloc(*ppvData, cbDataTotal + cbData);
                    if (!pvData)
                    {
                        rc = VERR_NO_MEMORY;
                        break;
                    }
                    rc = vbglR3DnDHGProcessMoreDataMessageInternal(pCtx,
                                                                   &((char *)pvData)[cbDataTotal],
                                                                   cbData,
                                                                   &cbDataReq);
                    cbDataTotal += cbDataReq;
                    break;
                }
                case DragAndDropSvc::HOST_DND_HG_EVT_CANCEL:
                default:
                {
                    rc = vbglR3DnDHGProcessCancelMessage(pCtx);
                    if (RT_SUCCESS(rc))
                        rc = VERR_CANCELLED;
                    break;
                }
            }
        }
    }

    if (RT_SUCCESS(rc))
    {
        *ppvData         = pvData;
        if (pcbDataRecv)
            *pcbDataRecv = cbDataTotal;
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}

static int vbglR3DnDHGProcessSendDataMessage(PVBGLR3GUESTDNDCMDCTX pCtx,
                                             uint32_t  *puScreenId,
                                             char      *pszFormat,
                                             uint32_t   cbFormat,
                                             uint32_t  *pcbFormatRecv,
                                             void     **ppvData,
                                             uint32_t   cbData,
                                             size_t    *pcbDataRecv)
{
    AssertPtrReturn(pCtx,          VERR_INVALID_POINTER);
    AssertPtrReturn(puScreenId,    VERR_INVALID_POINTER);
    AssertPtrReturn(pszFormat,     VERR_INVALID_POINTER);
    AssertPtrReturn(pcbFormatRecv, VERR_INVALID_POINTER);
    AssertPtrReturn(ppvData,       VERR_INVALID_POINTER);

    int rc = vbglR3DnDHGProcessSendDataMessageLoop(pCtx,
                                                   puScreenId,
                                                   pszFormat,
                                                   cbFormat,
                                                   pcbFormatRecv,
                                                   ppvData,
                                                   cbData,
                                                   pcbDataRecv);
    if (RT_SUCCESS(rc))
    {
        /* Check if this is an URI event. If so, let VbglR3 do all the actual
         * data transfer + file/directory creation internally without letting
         * the caller know.
         *
         * This keeps the actual (guest OS-)dependent client (like VBoxClient /
         * VBoxTray) small by not having too much redundant code. */
        AssertPtr(pcbFormatRecv);
        if (DnDMIMEHasFileURLs(pszFormat, *pcbFormatRecv))
            rc = vbglR3DnDHGProcessURIMessages(pCtx,
                                               ppvData,
                                               cbData,
                                               pcbDataRecv);
        if (RT_FAILURE(rc))
        {
            int rc2 = VbglR3DnDHGSetProgress(pCtx, DragAndDropSvc::DND_PROGRESS_ERROR, 100 /* Percent */, rc);
            AssertRC(rc2);
        }
    }

    LogFlowFuncLeaveRC(rc);
    return rc;
}

static int vbglR3DnDGHProcessRequestPendingMessage(PVBGLR3GUESTDNDCMDCTX pCtx,
                                                   uint32_t *puScreenId)
{
    AssertPtrReturn(pCtx,       VERR_INVALID_POINTER);
    AssertPtrReturn(puScreenId, VERR_INVALID_POINTER);

    DragAndDropSvc::VBOXDNDGHREQPENDINGMSG Msg;
    RT_ZERO(Msg);
    Msg.hdr.u32ClientID = pCtx->uClientID;
    Msg.hdr.u32Function = DragAndDropSvc::HOST_DND_GH_REQ_PENDING;
    Msg.hdr.cParms      = 1;

    Msg.uScreenId.SetUInt32(0);

    int rc = vbglR3DoIOCtl(VBOXGUEST_IOCTL_HGCM_CALL(sizeof(Msg)), &Msg, sizeof(Msg));
    if (RT_SUCCESS(rc))
    {
        rc = Msg.hdr.result;
        if (RT_SUCCESS(rc))
        {
            rc = Msg.uScreenId.GetUInt32(puScreenId); AssertRC(rc);
        }
    }

    return rc;
}

static int vbglR3DnDGHProcessDroppedMessage(PVBGLR3GUESTDNDCMDCTX pCtx,
                                            char     *pszFormat,
                                            uint32_t  cbFormat,
                                            uint32_t *pcbFormatRecv,
                                            uint32_t *puAction)
{
    AssertPtrReturn(pCtx,          VERR_INVALID_POINTER);
    AssertPtrReturn(pszFormat,     VERR_INVALID_POINTER);
    AssertReturn(cbFormat,         VERR_INVALID_PARAMETER);
    AssertPtrReturn(pcbFormatRecv, VERR_INVALID_POINTER);
    AssertPtrReturn(puAction,      VERR_INVALID_POINTER);

    DragAndDropSvc::VBOXDNDGHDROPPEDMSG Msg;
    RT_ZERO(Msg);
    Msg.hdr.u32ClientID = pCtx->uClientID;
    Msg.hdr.u32Function = DragAndDropSvc::HOST_DND_GH_EVT_DROPPED;
    Msg.hdr.cParms      = 3;

    Msg.pvFormat.SetPtr(pszFormat, cbFormat);
    Msg.cFormat.SetUInt32(0);
    Msg.uAction.SetUInt32(0);

    int rc = vbglR3DoIOCtl(VBOXGUEST_IOCTL_HGCM_CALL(sizeof(Msg)), &Msg, sizeof(Msg));
    if (RT_SUCCESS(rc))
    {
        rc = Msg.hdr.result;
        if (RT_SUCCESS(rc))
        {
            rc = Msg.cFormat.GetUInt32(pcbFormatRecv); AssertRC(rc);
            rc = Msg.uAction.GetUInt32(puAction);      AssertRC(rc);

            AssertReturn(cbFormat >= *pcbFormatRecv, VERR_TOO_MUCH_DATA);
        }
    }

    return rc;
}

/******************************************************************************
 *    Public functions                                                        *
 ******************************************************************************/

VBGLR3DECL(int) VbglR3DnDConnect(PVBGLR3GUESTDNDCMDCTX pCtx)
{
    AssertPtrReturn(pCtx, VERR_INVALID_POINTER);

    /* Initialize header */
    VBoxGuestHGCMConnectInfo Info;
    RT_ZERO(Info.Loc.u);
    Info.result      = VERR_WRONG_ORDER;
    Info.u32ClientID = UINT32_MAX;  /* try make valgrind shut up. */
    Info.Loc.type    = VMMDevHGCMLoc_LocalHost_Existing;

    int rc = RTStrCopy(Info.Loc.u.host.achName, sizeof(Info.Loc.u.host.achName), "VBoxDragAndDropSvc");
    if (RT_FAILURE(rc))
        return rc;

    rc = vbglR3DoIOCtl(VBOXGUEST_IOCTL_HGCM_CONNECT, &Info, sizeof(Info));
    if (RT_SUCCESS(rc))
    {
        rc = Info.result;
        if (rc == VERR_HGCM_SERVICE_NOT_FOUND)
            rc = VINF_PERMISSION_DENIED;

        /* Set the protocol version to use. */
        pCtx->uProtocol = 2;

        Assert(Info.u32ClientID);
        pCtx->uClientID = Info.u32ClientID;
    }

    if (RT_SUCCESS(rc))
    {
        /*
         * Try sending the connect message to tell the protocol version to use.
         * Note: This might fail when the Guest Additions run on an older VBox host (< VBox 5.0) which
         *       does not implement this command.
         */
        DragAndDropSvc::VBOXDNDCONNECTPMSG Msg;
        RT_ZERO(Msg);
        Msg.hdr.result      = VERR_WRONG_ORDER;
        Msg.hdr.u32ClientID = pCtx->uClientID;
        Msg.hdr.u32Function = DragAndDropSvc::GUEST_DND_CONNECT;
        Msg.hdr.cParms      = 2;

        Msg.uProtocol.SetUInt32(pCtx->uProtocol);
        Msg.uFlags.SetUInt32(0); /* Unused at the moment. */

        int rc2 = vbglR3DoIOCtl(VBOXGUEST_IOCTL_HGCM_CALL(sizeof(Msg)), &Msg, sizeof(Msg));
        if (RT_SUCCESS(rc2))
            rc2 = Msg.hdr.result; /* Not fatal. */

        LogFlowFunc(("Connection request ended with rc=%Rrc\n", rc2));
    }

    LogFlowFunc(("uClient=%RU32, uProtocol=%RU32, rc=%Rrc\n", pCtx->uClientID, pCtx->uProtocol, rc));
    return rc;
}

VBGLR3DECL(int) VbglR3DnDDisconnect(PVBGLR3GUESTDNDCMDCTX pCtx)
{
    AssertPtrReturn(pCtx, VERR_INVALID_POINTER);

    VBoxGuestHGCMDisconnectInfo Info;
    Info.result      = VERR_WRONG_ORDER;
    Info.u32ClientID = pCtx->uClientID;

    int rc = vbglR3DoIOCtl(VBOXGUEST_IOCTL_HGCM_DISCONNECT, &Info, sizeof(Info));
    if (RT_SUCCESS(rc))
        rc = Info.result;

    return rc;
}

VBGLR3DECL(int) VbglR3DnDProcessNextMessage(PVBGLR3GUESTDNDCMDCTX pCtx, CPVBGLR3DNDHGCMEVENT pEvent)
{
    AssertPtrReturn(pCtx,   VERR_INVALID_POINTER);
    AssertPtrReturn(pEvent, VERR_INVALID_POINTER);

    uint32_t       uMsg       = 0;
    uint32_t       uNumParms  = 0;
    const uint32_t ccbFormats = _64K;
    const uint32_t ccbData    = _64K;
    int rc = vbglR3DnDQueryNextHostMessageType(pCtx, &uMsg, &uNumParms,
                                               true /* fWait */);
    if (RT_SUCCESS(rc))
    {
        pEvent->uType = uMsg;

        switch(uMsg)
        {
            case DragAndDropSvc::HOST_DND_HG_EVT_ENTER:
            case DragAndDropSvc::HOST_DND_HG_EVT_MOVE:
            case DragAndDropSvc::HOST_DND_HG_EVT_DROPPED:
            {
                pEvent->pszFormats = static_cast<char*>(RTMemAlloc(ccbFormats));
                if (!pEvent->pszFormats)
                    rc = VERR_NO_MEMORY;

                if (RT_SUCCESS(rc))
                    rc = vbglR3DnDHGProcessActionMessage(pCtx,
                                                         uMsg,
                                                         &pEvent->uScreenId,
                                                         &pEvent->u.a.uXpos,
                                                         &pEvent->u.a.uYpos,
                                                         &pEvent->u.a.uDefAction,
                                                         &pEvent->u.a.uAllActions,
                                                         pEvent->pszFormats,
                                                         ccbFormats,
                                                         &pEvent->cbFormats);
                break;
            }
            case DragAndDropSvc::HOST_DND_HG_EVT_LEAVE:
            {
                rc = vbglR3DnDHGProcessLeaveMessage(pCtx);
                break;
            }
            case DragAndDropSvc::HOST_DND_HG_SND_DATA:
            {
                pEvent->pszFormats = static_cast<char*>(RTMemAlloc(ccbFormats));
                if (!pEvent->pszFormats)
                    rc = VERR_NO_MEMORY;

                if (RT_SUCCESS(rc))
                {
                    pEvent->u.b.pvData = RTMemAlloc(ccbData);
                    if (!pEvent->u.b.pvData)
                    {
                        RTMemFree(pEvent->pszFormats);
                        pEvent->pszFormats = NULL;

                        rc = VERR_NO_MEMORY;
                    }
                }

                if (RT_SUCCESS(rc))
                    rc = vbglR3DnDHGProcessSendDataMessage(pCtx,
                                                           &pEvent->uScreenId,
                                                           pEvent->pszFormats,
                                                           ccbFormats,
                                                           &pEvent->cbFormats,
                                                           &pEvent->u.b.pvData,
                                                           ccbData,
                                                           &pEvent->u.b.cbData);
                break;
            }
            case DragAndDropSvc::HOST_DND_HG_SND_MORE_DATA:
            case DragAndDropSvc::HOST_DND_HG_SND_DIR:
            case DragAndDropSvc::HOST_DND_HG_SND_FILE_DATA:
            {
                /*
                 * All messages in this case are handled internally
                 * by vbglR3DnDHGProcessSendDataMessage() and must
                 * be specified by a preceding HOST_DND_HG_SND_DATA call.
                 */
                rc = VERR_WRONG_ORDER;
                break;
            }
            case DragAndDropSvc::HOST_DND_HG_EVT_CANCEL:
            {
                rc = vbglR3DnDHGProcessCancelMessage(pCtx);
                break;
            }
#ifdef VBOX_WITH_DRAG_AND_DROP_GH
            case DragAndDropSvc::HOST_DND_GH_REQ_PENDING:
            {
                rc = vbglR3DnDGHProcessRequestPendingMessage(pCtx, &pEvent->uScreenId);
                break;
            }
            case DragAndDropSvc::HOST_DND_GH_EVT_DROPPED:
            {
                pEvent->pszFormats = static_cast<char*>(RTMemAlloc(ccbFormats));
                if (!pEvent->pszFormats)
                    rc = VERR_NO_MEMORY;

                if (RT_SUCCESS(rc))
                    rc = vbglR3DnDGHProcessDroppedMessage(pCtx,
                                                          pEvent->pszFormats,
                                                          ccbFormats,
                                                          &pEvent->cbFormats,
                                                          &pEvent->u.a.uDefAction);
                break;
            }
#endif
            default:
            {
                rc = VERR_NOT_SUPPORTED;
                break;
            }
        }
    }

    return rc;
}

VBGLR3DECL(int) VbglR3DnDHGAcknowledgeOperation(PVBGLR3GUESTDNDCMDCTX pCtx, uint32_t uAction)
{
    AssertPtrReturn(pCtx, VERR_INVALID_POINTER);

    DragAndDropSvc::VBOXDNDHGACKOPMSG Msg;
    RT_ZERO(Msg);
    Msg.hdr.result      = VERR_WRONG_ORDER;
    Msg.hdr.u32ClientID = pCtx->uClientID;
    Msg.hdr.u32Function = DragAndDropSvc::GUEST_DND_HG_ACK_OP;
    Msg.hdr.cParms      = 1;

    Msg.uAction.SetUInt32(uAction);

    int rc = vbglR3DoIOCtl(VBOXGUEST_IOCTL_HGCM_CALL(sizeof(Msg)), &Msg, sizeof(Msg));
    if (RT_SUCCESS(rc))
        rc = Msg.hdr.result;

    return rc;
}

VBGLR3DECL(int) VbglR3DnDHGRequestData(PVBGLR3GUESTDNDCMDCTX pCtx, const char* pcszFormat)
{
    AssertPtrReturn(pCtx,       VERR_INVALID_POINTER);
    AssertPtrReturn(pcszFormat, VERR_INVALID_POINTER);

    DragAndDropSvc::VBOXDNDHGREQDATAMSG Msg;
    RT_ZERO(Msg);
    Msg.hdr.result      = VERR_WRONG_ORDER;
    Msg.hdr.u32ClientID = pCtx->uClientID;
    Msg.hdr.u32Function = DragAndDropSvc::GUEST_DND_HG_REQ_DATA;
    Msg.hdr.cParms      = 1;

    Msg.pFormat.SetPtr((void*)pcszFormat, (uint32_t)strlen(pcszFormat) + 1);

    int rc = vbglR3DoIOCtl(VBOXGUEST_IOCTL_HGCM_CALL(sizeof(Msg)), &Msg, sizeof(Msg));
    if (RT_SUCCESS(rc))
        rc = Msg.hdr.result;

    return rc;
}

VBGLR3DECL(int) VbglR3DnDHGSetProgress(PVBGLR3GUESTDNDCMDCTX pCtx, uint32_t uStatus, uint8_t uPercent, int rcErr)
{
    AssertPtrReturn(pCtx, VERR_INVALID_POINTER);

    DragAndDropSvc::VBOXDNDHGEVTPROGRESSMSG Msg;
    RT_ZERO(Msg);
    Msg.hdr.result      = VERR_WRONG_ORDER;
    Msg.hdr.u32ClientID = pCtx->uClientID;
    Msg.hdr.u32Function = DragAndDropSvc::GUEST_DND_HG_EVT_PROGRESS;
    Msg.hdr.cParms      = 3;

    Msg.uStatus.SetUInt32(uStatus);
    Msg.uPercent.SetUInt32(uPercent);
    Msg.rc.SetUInt32((uint32_t)rcErr); /* uint32_t vs. int. */

    int rc = vbglR3DoIOCtl(VBOXGUEST_IOCTL_HGCM_CALL(sizeof(Msg)), &Msg, sizeof(Msg));
    if (RT_SUCCESS(rc))
        rc = Msg.hdr.result;

    return rc;
}

#ifdef VBOX_WITH_DRAG_AND_DROP_GH
VBGLR3DECL(int) VbglR3DnDGHAcknowledgePending(PVBGLR3GUESTDNDCMDCTX pCtx,
                                              uint32_t uDefAction, uint32_t uAllActions, const char* pcszFormats)
{
    AssertPtrReturn(pCtx,        VERR_INVALID_POINTER);
    AssertPtrReturn(pcszFormats, VERR_INVALID_POINTER);

    DragAndDropSvc::VBOXDNDGHACKPENDINGMSG Msg;
    RT_ZERO(Msg);
    Msg.hdr.result      = VERR_WRONG_ORDER;
    Msg.hdr.u32ClientID = pCtx->uClientID;
    Msg.hdr.u32Function = DragAndDropSvc::GUEST_DND_GH_ACK_PENDING;
    Msg.hdr.cParms      = 3;

    Msg.uDefAction.SetUInt32(uDefAction);
    Msg.uAllActions.SetUInt32(uAllActions);
    Msg.pFormat.SetPtr((void*)pcszFormats, (uint32_t)strlen(pcszFormats) + 1);

    int rc = vbglR3DoIOCtl(VBOXGUEST_IOCTL_HGCM_CALL(sizeof(Msg)), &Msg, sizeof(Msg));
    if (RT_SUCCESS(rc))
        rc = Msg.hdr.result;

    return rc;
}

static int vbglR3DnDGHSendDataInternal(PVBGLR3GUESTDNDCMDCTX pCtx,
                                       void *pvData, uint32_t cbData, uint32_t cbAdditionalData)
{
    AssertPtrReturn(pCtx,   VERR_INVALID_POINTER);
    AssertPtrReturn(pvData, VERR_INVALID_POINTER);
    AssertReturn(cbData,    VERR_INVALID_PARAMETER);
    /* cbAdditionalData is optional. */

    DragAndDropSvc::VBOXDNDGHSENDDATAMSG Msg;
    RT_ZERO(Msg);
    Msg.hdr.result      = VERR_WRONG_ORDER;
    Msg.hdr.u32ClientID = pCtx->uClientID;
    Msg.hdr.u32Function = DragAndDropSvc::GUEST_DND_GH_SND_DATA;
    Msg.hdr.cParms      = 2;

    /* Total amount of bytes to send (including this data block). */
    Msg.cbTotalBytes.SetUInt32(cbData + cbAdditionalData);

    int rc = VINF_SUCCESS;

    uint32_t cbCurChunk;
    uint32_t cbMaxChunk = _64K; /** @todo Transfer max. 64K chunks per message. Configurable? */
    uint32_t cbSent     = 0;

    while (cbSent < cbData)
    {
        cbCurChunk = RT_MIN(cbData - cbSent, cbMaxChunk);
        Msg.pvData.SetPtr(static_cast<uint8_t *>(pvData) + cbSent, cbCurChunk);

        rc = vbglR3DoIOCtl(VBOXGUEST_IOCTL_HGCM_CALL(sizeof(Msg)), &Msg, sizeof(Msg));
        if (RT_SUCCESS(rc))
            rc = Msg.hdr.result;

        if (RT_FAILURE(rc))
            break;

        cbSent += cbCurChunk;
    }

    if (RT_SUCCESS(rc))
        Assert(cbSent == cbData);

    LogFlowFunc(("Returning rc=%Rrc, cbData=%RU32, cbAddtionalData=%RU32, cbSent=%RU32\n",
                 rc, cbData, cbAdditionalData, cbSent));
    return rc;
}

static int vbglR3DnDGHSendDir(PVBGLR3GUESTDNDCMDCTX pCtx, DnDURIObject &obj)
{
    AssertPtrReturn(pCtx,                                  VERR_INVALID_POINTER);
    AssertReturn(obj.GetType() == DnDURIObject::Directory, VERR_INVALID_PARAMETER);

    DragAndDropSvc::VBOXDNDGHSENDDIRMSG Msg;
    RT_ZERO(Msg);
    Msg.hdr.result      = VERR_WRONG_ORDER;
    Msg.hdr.u32ClientID = pCtx->uClientID;
    Msg.hdr.u32Function = DragAndDropSvc::GUEST_DND_GH_SND_DIR;
    Msg.hdr.cParms      = 3;

    RTCString strPath = obj.GetDestPath();
    LogFlowFunc(("strDir=%s (%zu), fMode=0x%x\n",
                 strPath.c_str(), strPath.length(), obj.GetMode()));

    Msg.pvName.SetPtr((void *)strPath.c_str(), (uint32_t)(strPath.length() + 1));
    Msg.cbName.SetUInt32((uint32_t)(strPath.length() + 1));
    Msg.fMode.SetUInt32(obj.GetMode());

    int rc = vbglR3DoIOCtl(VBOXGUEST_IOCTL_HGCM_CALL(sizeof(Msg)), &Msg, sizeof(Msg));
    if (RT_SUCCESS(rc))
        rc = Msg.hdr.result;

    LogFlowFuncLeaveRC(rc);
    return rc;
}

static int vbglR3DnDGHSendFile(PVBGLR3GUESTDNDCMDCTX pCtx, DnDURIObject &obj)
{
    AssertPtrReturn(pCtx,                             VERR_INVALID_POINTER);
    AssertReturn(obj.GetType() == DnDURIObject::File, VERR_INVALID_PARAMETER);

    int rc = obj.Open(DnDURIObject::Source, RTFILE_O_OPEN | RTFILE_O_READ | RTFILE_O_DENY_WRITE);
    if (RT_FAILURE(rc))
    {
        LogFunc(("Opening file \"%s\" failed with rc=%Rrc\n", obj.GetSourcePath().c_str(), rc));
        return rc;
    }

    uint32_t cbBuf = _64K;           /** @todo Make this configurable? */
    void *pvBuf = RTMemAlloc(cbBuf); /** @todo Make this buffer part of PVBGLR3GUESTDNDCMDCTX? */
    if (!pvBuf)
        return VERR_NO_MEMORY;

    RTCString strPath = obj.GetDestPath();

    LogFlowFunc(("strFile=%s (%zu), cbSize=%RU64, fMode=0x%x\n", strPath.c_str(), strPath.length(), obj.GetSize(), obj.GetMode()));
    LogFlowFunc(("uProtocol=%RU32, uClientID=%RU32\n", pCtx->uProtocol, pCtx->uClientID));

    if (pCtx->uProtocol >= 2) /* Protocol version 2 and up sends a file header first. */
    {
        DragAndDropSvc::VBOXDNDGHSENDFILEHDRMSG MsgHdr;
        RT_ZERO(MsgHdr);
        MsgHdr.hdr.result      = VERR_WRONG_ORDER;
        MsgHdr.hdr.u32ClientID = pCtx->uClientID;
        MsgHdr.hdr.u32Function = DragAndDropSvc::GUEST_DND_GH_SND_FILE_HDR;
        MsgHdr.hdr.cParms      = 6;

        MsgHdr.uContext.SetUInt32(0);                                                    /* Context ID; unused at the moment. */
        MsgHdr.pvName.SetPtr((void *)strPath.c_str(), (uint32_t)(strPath.length() + 1));
        MsgHdr.cbName.SetUInt32((uint32_t)(strPath.length() + 1));
        MsgHdr.uFlags.SetUInt32(0);                                                      /* Flags; unused at the moment. */
        MsgHdr.fMode.SetUInt32(obj.GetMode());                                           /* File mode */
        MsgHdr.cbTotal.SetUInt64(obj.GetSize());                                         /* File size (in bytes). */

        rc = vbglR3DoIOCtl(VBOXGUEST_IOCTL_HGCM_CALL(sizeof(MsgHdr)), &MsgHdr, sizeof(MsgHdr));
        if (RT_SUCCESS(rc))
            rc = MsgHdr.hdr.result;

        LogFlowFunc(("Sending file header resulted in %Rrc\n", rc));
    }
    else
        rc = VINF_SUCCESS;

    if (RT_SUCCESS(rc))
    {
        /*
         * Send the actual file data, chunk by chunk.
         */
        DragAndDropSvc::VBOXDNDGHSENDFILEDATAMSG Msg;
        RT_ZERO(Msg);
        Msg.hdr.result      = VERR_WRONG_ORDER;
        Msg.hdr.u32ClientID = pCtx->uClientID;
        Msg.hdr.u32Function = DragAndDropSvc::GUEST_DND_GH_SND_FILE_DATA;

        if (pCtx->uProtocol <= 1)
        {
            Msg.hdr.cParms  = 5;

            Msg.u.v1.pvName.SetPtr((void *)strPath.c_str(), (uint32_t)(strPath.length() + 1));
            Msg.u.v1.cbName.SetUInt32((uint32_t)(strPath.length() + 1));
            Msg.u.v1.fMode.SetUInt32(obj.GetMode());
        }
        else
        {
            /* Only send context ID, file chunk + chunk size. */
            Msg.hdr.cParms  = 3;

            Msg.u.v2.uContext.SetUInt32(0); /** @todo Set context ID. */
        }

        uint64_t cbToReadTotal  = obj.GetSize();
        uint64_t cbWrittenTotal = 0;
        while (cbToReadTotal)
        {
            uint32_t cbToRead = RT_MIN(cbToReadTotal, cbBuf);
            uint32_t cbRead   = 0;
            if (cbToRead)
                rc = obj.Read(pvBuf, cbToRead, &cbRead);

            LogFlowFunc(("cbToReadTotal=%RU64, cbToRead=%RU32, cbRead=%RU32, rc=%Rrc\n",
                         cbToReadTotal, cbToRead, cbRead, rc));

            if (   RT_SUCCESS(rc)
                && cbRead)
            {
                if (pCtx->uProtocol <= 1)
                {
                    Msg.u.v1.pvData.SetPtr(pvBuf, cbRead);
                    Msg.u.v1.cbData.SetUInt32(cbRead);
                }
                else
                {
                    Msg.u.v2.pvData.SetPtr(pvBuf, cbRead);
                    Msg.u.v2.cbData.SetUInt32(cbRead);
                }

                rc = vbglR3DoIOCtl(VBOXGUEST_IOCTL_HGCM_CALL(sizeof(Msg)), &Msg, sizeof(Msg));
                if (RT_SUCCESS(rc))
                    rc = Msg.hdr.result;
            }

            if (RT_FAILURE(rc))
            {
                LogFlowFunc(("Reading failed with rc=%Rrc\n", rc));
                break;
            }

            Assert(cbRead  <= cbToReadTotal);
            cbToReadTotal  -= cbRead;
            cbWrittenTotal += cbRead;

            LogFlowFunc(("%RU64/%RU64 -- %RU8%%\n", cbWrittenTotal, obj.GetSize(), cbWrittenTotal * 100 / obj.GetSize()));
        };
    }

    obj.Close();

    RTMemFree(pvBuf);

    LogFlowFuncLeaveRC(rc);
    return rc;
}

static int vbglR3DnDGHSendURIObject(PVBGLR3GUESTDNDCMDCTX pCtx, DnDURIObject &obj)
{
    AssertPtrReturn(pCtx, VERR_INVALID_POINTER);

    int rc;

    switch (obj.GetType())
    {
        case DnDURIObject::Directory:
            rc = vbglR3DnDGHSendDir(pCtx, obj);
            break;

        case DnDURIObject::File:
            rc = vbglR3DnDGHSendFile(pCtx, obj);
            break;

        default:
            AssertMsgFailed(("URI type %ld not implemented\n", obj.GetType()));
            rc = VERR_NOT_IMPLEMENTED;
            break;
    }

    return rc;
}

static int vbglR3DnDGHProcessURIMessages(PVBGLR3GUESTDNDCMDCTX pCtx,
                                         const void *pvData, uint32_t cbData)
{
    AssertPtrReturn(pCtx,   VERR_INVALID_POINTER);
    AssertPtrReturn(pvData, VERR_INVALID_POINTER);
    AssertReturn(cbData,    VERR_INVALID_PARAMETER);

    RTCList<RTCString> lstPaths =
        RTCString((const char *)pvData, cbData).split("\r\n");

    DnDURIList lstURI;
    int rc = lstURI.AppendURIPathsFromList(lstPaths, 0 /* fFlags */);
    if (RT_SUCCESS(rc))
    {
        /* Send metadata; in this case it's the (non-recursive) file/directory
         * URI list the host needs to know to initialize the drag'n drop operation. */
        RTCString strRootDest = lstURI.RootToString();
        Assert(strRootDest.isNotEmpty());

        void *pvToSend = (void *)strRootDest.c_str();
        uint32_t cbToSend = (uint32_t)strRootDest.length() + 1; /* Include string termination. */

        rc = vbglR3DnDGHSendDataInternal(pCtx, pvToSend, cbToSend,
                                         /* Include total bytes of all file paths,
                                          * file sizes etc. */
                                         lstURI.TotalBytes());
    }

    if (RT_SUCCESS(rc))
    {
        while (!lstURI.IsEmpty())
        {
            DnDURIObject &nextObj = lstURI.First();

            rc = vbglR3DnDGHSendURIObject(pCtx, nextObj);
            if (RT_FAILURE(rc))
                break;

            lstURI.RemoveFirst();
        }
    }

    return rc;
}

VBGLR3DECL(int) VbglR3DnDGHSendData(PVBGLR3GUESTDNDCMDCTX pCtx, const char *pszFormat, void *pvData, uint32_t cbData)
{
    AssertPtrReturn(pCtx,      VERR_INVALID_POINTER);
    AssertPtrReturn(pszFormat, VERR_INVALID_POINTER);
    AssertPtrReturn(pvData,    VERR_INVALID_POINTER);
    AssertReturn(cbData,       VERR_INVALID_PARAMETER);

    LogFlowFunc(("pszFormat=%s, pvData=%p, cbData=%RU32\n", pszFormat, pvData, cbData));

    int rc;
    if (DnDMIMEHasFileURLs(pszFormat, strlen(pszFormat)))
    {
        rc = vbglR3DnDGHProcessURIMessages(pCtx, pvData, cbData);
    }
    else
    {
        rc = vbglR3DnDGHSendDataInternal(pCtx, pvData, cbData, 0 /* cbAdditionalData */);
    }

    if (RT_FAILURE(rc))
    {
        int rc2 = VbglR3DnDGHSendError(pCtx, rc);
        if (RT_FAILURE(rc2))
            LogFlowFunc(("Unable to send error (%Rrc) to host, rc=%Rrc\n", rc, rc2));
    }

    return rc;
}

VBGLR3DECL(int) VbglR3DnDGHSendError(PVBGLR3GUESTDNDCMDCTX pCtx, int rcErr)
{
    AssertPtrReturn(pCtx, VERR_INVALID_POINTER);

    DragAndDropSvc::VBOXDNDGHEVTERRORMSG Msg;
    RT_ZERO(Msg);
    Msg.hdr.result      = VERR_WRONG_ORDER;
    Msg.hdr.u32ClientID = pCtx->uClientID;
    Msg.hdr.u32Function = DragAndDropSvc::GUEST_DND_GH_EVT_ERROR;
    Msg.hdr.cParms      = 1;

    Msg.rc.SetUInt32((uint32_t)rcErr); /* uint32_t vs. int. */

    int rc = vbglR3DoIOCtl(VBOXGUEST_IOCTL_HGCM_CALL(sizeof(Msg)), &Msg, sizeof(Msg));
    if (RT_SUCCESS(rc))
        rc = Msg.hdr.result;

    LogFlowFunc(("Sending error %Rrc returned with rc=%Rrc\n", rcErr, rc));
    return rc;
}
#endif /* VBOX_WITH_DRAG_AND_DROP_GH */

