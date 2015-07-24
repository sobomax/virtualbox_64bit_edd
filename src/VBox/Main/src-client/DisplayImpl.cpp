/* $Id: DisplayImpl.cpp $ */
/** @file
 * VirtualBox COM class implementation
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
 */

#include "DisplayImpl.h"
#include "DisplayUtils.h"
#include "ConsoleImpl.h"
#include "ConsoleVRDPServer.h"
#include "GuestImpl.h"
#include "VMMDev.h"

#include "AutoCaller.h"
#include "Logging.h"

/* generated header */
#include "VBoxEvents.h"

#include <iprt/semaphore.h>
#include <iprt/thread.h>
#include <iprt/asm.h>
#include <iprt/time.h>
#include <iprt/cpp/utils.h>
#include <iprt/alloca.h>

#include <VBox/vmm/pdmdrv.h>
#if defined(DEBUG) || defined(VBOX_STRICT) /* for VM_ASSERT_EMT(). */
# include <VBox/vmm/vm.h>
#endif

#ifdef VBOX_WITH_VIDEOHWACCEL
# include <VBox/VBoxVideo.h>
#endif

#if defined(VBOX_WITH_CROGL) || defined(VBOX_WITH_CRHGSMI)
# include <VBox/HostServices/VBoxCrOpenGLSvc.h>
#endif

#include <VBox/com/array.h>

#ifdef VBOX_WITH_VPX
# include <iprt/path.h>
# include "VideoRec.h"
#endif

#ifdef VBOX_WITH_CROGL
typedef enum
{
    CRVREC_STATE_IDLE,
    CRVREC_STATE_SUBMITTED
} CRVREC_STATE;
#endif

/**
 * Display driver instance data.
 *
 * @implements PDMIDISPLAYCONNECTOR
 */
typedef struct DRVMAINDISPLAY
{
    /** Pointer to the display object. */
    Display                    *pDisplay;
    /** Pointer to the driver instance structure. */
    PPDMDRVINS                  pDrvIns;
    /** Pointer to the keyboard port interface of the driver/device above us. */
    PPDMIDISPLAYPORT            pUpPort;
    /** Our display connector interface. */
    PDMIDISPLAYCONNECTOR        IConnector;
#if defined(VBOX_WITH_VIDEOHWACCEL) || defined(VBOX_WITH_CRHGSMI)
    /** VBVA callbacks */
    PPDMIDISPLAYVBVACALLBACKS   pVBVACallbacks;
#endif
} DRVMAINDISPLAY, *PDRVMAINDISPLAY;

/** Converts PDMIDISPLAYCONNECTOR pointer to a DRVMAINDISPLAY pointer. */
#define PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface)  RT_FROM_MEMBER(pInterface, DRVMAINDISPLAY, IConnector)

#ifdef DEBUG_sunlover
static STAMPROFILE g_StatDisplayRefresh;
static int g_stam = 0;
#endif /* DEBUG_sunlover */

// constructor / destructor
/////////////////////////////////////////////////////////////////////////////

Display::Display()
    : mParent(NULL), mfIsCr3DEnabled(false)
{
}

Display::~Display()
{
}


HRESULT Display::FinalConstruct()
{
    mpVbvaMemory = NULL;
    mfVideoAccelEnabled = false;
    mfVideoAccelVRDP = false;
    mfu32SupportedOrders = 0;
    mcVideoAccelVRDPRefs = 0;

    mpPendingVbvaMemory = NULL;
    mfPendingVideoAccelEnable = false;

    mfMachineRunning = false;
#ifdef VBOX_WITH_CROGL
    mfCrOglDataHidden = false;
#endif

    mpu8VbvaPartial = NULL;
    mcbVbvaPartial = 0;

    mpDrv = NULL;
    mpVMMDev = NULL;
    mfVMMDevInited = false;

    mLastAddress = NULL;
    mLastBytesPerLine = 0;
    mLastBitsPerPixel = 0,
    mLastWidth = 0;
    mLastHeight = 0;

    int rc = RTCritSectInit(&mVBVALock);
    AssertRC(rc);

    rc = RTCritSectInit(&mSaveSeamlessRectLock);
    AssertRC(rc);

    mfu32PendingVideoAccelDisable = false;

#ifdef VBOX_WITH_HGSMI
    mu32UpdateVBVAFlags = 0;
    mfVMMDevSupportsGraphics = false;
    mfGuestVBVACapabilities = 0;
    mfHostCursorCapabilities = 0;
#endif
#ifdef VBOX_WITH_VPX
    mpVideoRecCtx = NULL;
    for (unsigned i = 0; i < RT_ELEMENTS(maVideoRecEnabled); i++)
        maVideoRecEnabled[i] = true;
#endif

#ifdef VBOX_WITH_CRHGSMI
    mhCrOglSvc = NULL;
    rc = RTCritSectRwInit(&mCrOglLock);
    AssertRC(rc);
#endif
#ifdef VBOX_WITH_CROGL
    RT_ZERO(mCrOglCallbacks);
    RT_ZERO(mCrOglScreenshotData);
    mfCrOglVideoRecState = CRVREC_STATE_IDLE;
    mCrOglScreenshotData.u32Screen = CRSCREEN_ALL;
    mCrOglScreenshotData.pvContext = this;
    mCrOglScreenshotData.pfnScreenshotBegin = displayCrVRecScreenshotBegin;
    mCrOglScreenshotData.pfnScreenshotPerform = displayCrVRecScreenshotPerform;
    mCrOglScreenshotData.pfnScreenshotEnd = displayCrVRecScreenshotEnd;
#endif

    return BaseFinalConstruct();
}

void Display::FinalRelease()
{
    uninit();

    if (RTCritSectIsInitialized (&mVBVALock))
    {
        RTCritSectDelete (&mVBVALock);
        RT_ZERO(mVBVALock);
    }

    if (RTCritSectIsInitialized(&mSaveSeamlessRectLock))
    {
        RTCritSectDelete(&mSaveSeamlessRectLock);
        RT_ZERO(mSaveSeamlessRectLock);
    }

#ifdef VBOX_WITH_CRHGSMI
    if (RTCritSectRwIsInitialized (&mCrOglLock))
    {
        RTCritSectRwDelete (&mCrOglLock);
        RT_ZERO(mCrOglLock);
    }
#endif
    BaseFinalRelease();
}

// public initializer/uninitializer for internal purposes only
/////////////////////////////////////////////////////////////////////////////

#define kMaxSizeThumbnail 64

/**
 * Save thumbnail and screenshot of the guest screen.
 */
static int displayMakeThumbnail(uint8_t *pu8Data, uint32_t cx, uint32_t cy,
                                uint8_t **ppu8Thumbnail, uint32_t *pcbThumbnail, uint32_t *pcxThumbnail, uint32_t *pcyThumbnail)
{
    int rc = VINF_SUCCESS;

    uint8_t *pu8Thumbnail = NULL;
    uint32_t cbThumbnail = 0;
    uint32_t cxThumbnail = 0;
    uint32_t cyThumbnail = 0;

    if (cx > cy)
    {
        cxThumbnail = kMaxSizeThumbnail;
        cyThumbnail = (kMaxSizeThumbnail * cy) / cx;
    }
    else
    {
        cyThumbnail = kMaxSizeThumbnail;
        cxThumbnail = (kMaxSizeThumbnail * cx) / cy;
    }

    LogRelFlowFunc(("%dx%d -> %dx%d\n", cx, cy, cxThumbnail, cyThumbnail));

    cbThumbnail = cxThumbnail * 4 * cyThumbnail;
    pu8Thumbnail = (uint8_t *)RTMemAlloc(cbThumbnail);

    if (pu8Thumbnail)
    {
        uint8_t *dst = pu8Thumbnail;
        uint8_t *src = pu8Data;
        int dstW = cxThumbnail;
        int dstH = cyThumbnail;
        int srcW = cx;
        int srcH = cy;
        int iDeltaLine = cx * 4;

        BitmapScale32 (dst,
                       dstW, dstH,
                       src,
                       iDeltaLine,
                       srcW, srcH);

        *ppu8Thumbnail = pu8Thumbnail;
        *pcbThumbnail = cbThumbnail;
        *pcxThumbnail = cxThumbnail;
        *pcyThumbnail = cyThumbnail;
    }
    else
    {
        rc = VERR_NO_MEMORY;
    }

    return rc;
}

#ifdef VBOX_WITH_CROGL
typedef struct
{
    CRVBOXHGCMTAKESCREENSHOT Base;

    /* 32bpp small RGB image. */
    uint8_t *pu8Thumbnail;
    uint32_t cbThumbnail;
    uint32_t cxThumbnail;
    uint32_t cyThumbnail;

    /* PNG screenshot. */
    uint8_t *pu8PNG;
    uint32_t cbPNG;
    uint32_t cxPNG;
    uint32_t cyPNG;
} VBOX_DISPLAY_SAVESCREENSHOT_DATA;

static DECLCALLBACK(void) displaySaveScreenshotReport(void *pvCtx, uint32_t uScreen,
        uint32_t x, uint32_t y, uint32_t uBitsPerPixel,
        uint32_t uBytesPerLine, uint32_t uGuestWidth, uint32_t uGuestHeight,
        uint8_t *pu8BufferAddress, uint64_t u64TimeStamp)
{
    VBOX_DISPLAY_SAVESCREENSHOT_DATA *pData = (VBOX_DISPLAY_SAVESCREENSHOT_DATA*)pvCtx;
    displayMakeThumbnail(pu8BufferAddress, uGuestWidth, uGuestHeight, &pData->pu8Thumbnail, &pData->cbThumbnail, &pData->cxThumbnail, &pData->cyThumbnail);
    int rc = DisplayMakePNG(pu8BufferAddress, uGuestWidth, uGuestHeight, &pData->pu8PNG, &pData->cbPNG, &pData->cxPNG, &pData->cyPNG, 1);
    if (RT_FAILURE(rc))
    {
        AssertMsgFailed(("DisplayMakePNG failed %d\n", rc));
        if (pData->pu8PNG)
        {
            RTMemFree(pData->pu8PNG);
            pData->pu8PNG = NULL;
        }
        pData->cbPNG = 0;
        pData->cxPNG = 0;
        pData->cyPNG = 0;
    }
}
#endif

DECLCALLBACK(void)
Display::displaySSMSaveScreenshot(PSSMHANDLE pSSM, void *pvUser)
{
    Display *that = static_cast<Display*>(pvUser);

    /* 32bpp small RGB image. */
    uint8_t *pu8Thumbnail = NULL;
    uint32_t cbThumbnail = 0;
    uint32_t cxThumbnail = 0;
    uint32_t cyThumbnail = 0;

    /* PNG screenshot. */
    uint8_t *pu8PNG = NULL;
    uint32_t cbPNG = 0;
    uint32_t cxPNG = 0;
    uint32_t cyPNG = 0;

    Console::SafeVMPtr ptrVM(that->mParent);
    if (ptrVM.isOk())
    {
        /* Query RGB bitmap. */
        uint8_t *pu8Data = NULL;
        size_t cbData = 0;
        uint32_t cx = 0;
        uint32_t cy = 0;

#if defined(VBOX_WITH_HGCM) && defined(VBOX_WITH_CROGL)
        BOOL f3DSnapshot = FALSE;
        if (   that->mfIsCr3DEnabled
            && that->mCrOglCallbacks.pfnHasData
            && that->mCrOglCallbacks.pfnHasData())
        {
            VMMDev *pVMMDev = that->mParent->getVMMDev();
            if (pVMMDev)
            {
                VBOX_DISPLAY_SAVESCREENSHOT_DATA *pScreenshot =
                    (VBOX_DISPLAY_SAVESCREENSHOT_DATA*)RTMemAllocZ(sizeof (*pScreenshot));
                if (pScreenshot)
                {
                    /* screen id or CRSCREEN_ALL to specify all enabled */
                    pScreenshot->Base.u32Screen = 0;
                    pScreenshot->Base.u32Width = 0;
                    pScreenshot->Base.u32Height = 0;
                    pScreenshot->Base.u32Pitch = 0;
                    pScreenshot->Base.pvBuffer = NULL;
                    pScreenshot->Base.pvContext = pScreenshot;
                    pScreenshot->Base.pfnScreenshotBegin = NULL;
                    pScreenshot->Base.pfnScreenshotPerform = displaySaveScreenshotReport;
                    pScreenshot->Base.pfnScreenshotEnd = NULL;

                    VBOXCRCMDCTL_HGCM data;
                    data.Hdr.enmType = VBOXCRCMDCTL_TYPE_HGCM;
                    data.Hdr.u32Function = SHCRGL_HOST_FN_TAKE_SCREENSHOT;

                    data.aParms[0].type = VBOX_HGCM_SVC_PARM_PTR;
                    data.aParms[0].u.pointer.addr = &pScreenshot->Base;
                    data.aParms[0].u.pointer.size = sizeof (pScreenshot->Base);

                    int rc = that->crCtlSubmitSync(&data.Hdr, sizeof (data));
                    if (RT_SUCCESS(rc))
                    {
                        if (pScreenshot->pu8PNG)
                        {
                            pu8Thumbnail = pScreenshot->pu8Thumbnail;
                            cbThumbnail = pScreenshot->cbThumbnail;
                            cxThumbnail = pScreenshot->cxThumbnail;
                            cyThumbnail = pScreenshot->cyThumbnail;

                            /* PNG screenshot. */
                            pu8PNG = pScreenshot->pu8PNG;
                            cbPNG = pScreenshot->cbPNG;
                            cxPNG = pScreenshot->cxPNG;
                            cyPNG = pScreenshot->cyPNG;
                            f3DSnapshot = TRUE;
                        }
                        else
                            AssertMsgFailed(("no png\n"));
                    }
                    else
                        AssertMsgFailed(("SHCRGL_HOST_FN_TAKE_SCREENSHOT failed %d\n", rc));


                    RTMemFree(pScreenshot);
                }
            }
        }

        if (!f3DSnapshot)
#endif
        {
            /* SSM code is executed on EMT(0), therefore no need to use VMR3ReqCallWait. */
            int rc = Display::displayTakeScreenshotEMT(that, VBOX_VIDEO_PRIMARY_SCREEN, &pu8Data, &cbData, &cx, &cy);

            /*
             * It is possible that success is returned but everything is 0 or NULL.
             * (no display attached if a VM is running with VBoxHeadless on OSE for example)
             */
            if (RT_SUCCESS(rc) && pu8Data)
            {
                Assert(cx && cy);

                /* Prepare a small thumbnail and a PNG screenshot. */
                displayMakeThumbnail(pu8Data, cx, cy, &pu8Thumbnail, &cbThumbnail, &cxThumbnail, &cyThumbnail);
                rc = DisplayMakePNG(pu8Data, cx, cy, &pu8PNG, &cbPNG, &cxPNG, &cyPNG, 1);
                if (RT_FAILURE(rc))
                {
                    if (pu8PNG)
                    {
                        RTMemFree(pu8PNG);
                        pu8PNG = NULL;
                    }
                    cbPNG = 0;
                    cxPNG = 0;
                    cyPNG = 0;
                }

                /* This can be called from any thread. */
                that->mpDrv->pUpPort->pfnFreeScreenshot(that->mpDrv->pUpPort, pu8Data);
            }
        }
    }
    else
    {
        LogFunc(("Failed to get VM pointer 0x%x\n", ptrVM.rc()));
    }

    /* Regardless of rc, save what is available:
     * Data format:
     *    uint32_t cBlocks;
     *    [blocks]
     *
     *  Each block is:
     *    uint32_t cbBlock;        if 0 - no 'block data'.
     *    uint32_t typeOfBlock;    0 - 32bpp RGB bitmap, 1 - PNG, ignored if 'cbBlock' is 0.
     *    [block data]
     *
     *  Block data for bitmap and PNG:
     *    uint32_t cx;
     *    uint32_t cy;
     *    [image data]
     */
    SSMR3PutU32(pSSM, 2); /* Write thumbnail and PNG screenshot. */

    /* First block. */
    SSMR3PutU32(pSSM, cbThumbnail + 2 * sizeof (uint32_t));
    SSMR3PutU32(pSSM, 0); /* Block type: thumbnail. */

    if (cbThumbnail)
    {
        SSMR3PutU32(pSSM, cxThumbnail);
        SSMR3PutU32(pSSM, cyThumbnail);
        SSMR3PutMem(pSSM, pu8Thumbnail, cbThumbnail);
    }

    /* Second block. */
    SSMR3PutU32(pSSM, cbPNG + 2 * sizeof (uint32_t));
    SSMR3PutU32(pSSM, 1); /* Block type: png. */

    if (cbPNG)
    {
        SSMR3PutU32(pSSM, cxPNG);
        SSMR3PutU32(pSSM, cyPNG);
        SSMR3PutMem(pSSM, pu8PNG, cbPNG);
    }

    RTMemFree(pu8PNG);
    RTMemFree(pu8Thumbnail);
}

DECLCALLBACK(int)
Display::displaySSMLoadScreenshot(PSSMHANDLE pSSM, void *pvUser, uint32_t uVersion, uint32_t uPass)
{
    Display *that = static_cast<Display*>(pvUser);

    if (uVersion != sSSMDisplayScreenshotVer)
        return VERR_SSM_UNSUPPORTED_DATA_UNIT_VERSION;
    Assert(uPass == SSM_PASS_FINAL); NOREF(uPass);

    /* Skip data. */
    uint32_t cBlocks;
    int rc = SSMR3GetU32(pSSM, &cBlocks);
    AssertRCReturn(rc, rc);

    for (uint32_t i = 0; i < cBlocks; i++)
    {
        uint32_t cbBlock;
        rc = SSMR3GetU32(pSSM, &cbBlock);
        AssertRCBreak(rc);

        uint32_t typeOfBlock;
        rc = SSMR3GetU32(pSSM, &typeOfBlock);
        AssertRCBreak(rc);

        LogRelFlowFunc(("[%d] type %d, size %d bytes\n", i, typeOfBlock, cbBlock));

        /* Note: displaySSMSaveScreenshot writes size of a block = 8 and
         * do not write any data if the image size was 0.
         * @todo Fix and increase saved state version.
         */
        if (cbBlock > 2 * sizeof (uint32_t))
        {
            rc = SSMR3Skip(pSSM, cbBlock);
            AssertRCBreak(rc);
        }
    }

    return rc;
}

/**
 * Save/Load some important guest state
 */
DECLCALLBACK(void)
Display::displaySSMSave(PSSMHANDLE pSSM, void *pvUser)
{
    Display *that = static_cast<Display*>(pvUser);

    SSMR3PutU32(pSSM, that->mcMonitors);
    for (unsigned i = 0; i < that->mcMonitors; i++)
    {
        SSMR3PutU32(pSSM, that->maFramebuffers[i].u32Offset);
        SSMR3PutU32(pSSM, that->maFramebuffers[i].u32MaxFramebufferSize);
        SSMR3PutU32(pSSM, that->maFramebuffers[i].u32InformationSize);
        SSMR3PutU32(pSSM, that->maFramebuffers[i].w);
        SSMR3PutU32(pSSM, that->maFramebuffers[i].h);
        SSMR3PutS32(pSSM, that->maFramebuffers[i].xOrigin);
        SSMR3PutS32(pSSM, that->maFramebuffers[i].yOrigin);
        SSMR3PutU32(pSSM, that->maFramebuffers[i].flags);
    }
    SSMR3PutS32(pSSM, that->xInputMappingOrigin);
    SSMR3PutS32(pSSM, that->yInputMappingOrigin);
    SSMR3PutU32(pSSM, that->cxInputMapping);
    SSMR3PutU32(pSSM, that->cyInputMapping);
    SSMR3PutU32(pSSM, that->mfGuestVBVACapabilities);
    SSMR3PutU32(pSSM, that->mfHostCursorCapabilities);
}

DECLCALLBACK(int)
Display::displaySSMLoad(PSSMHANDLE pSSM, void *pvUser, uint32_t uVersion, uint32_t uPass)
{
    Display *that = static_cast<Display*>(pvUser);

    if (   uVersion != sSSMDisplayVer
        && uVersion != sSSMDisplayVer2
        && uVersion != sSSMDisplayVer3
        && uVersion != sSSMDisplayVer4
        && uVersion != sSSMDisplayVer5)
        return VERR_SSM_UNSUPPORTED_DATA_UNIT_VERSION;
    Assert(uPass == SSM_PASS_FINAL); NOREF(uPass);

    uint32_t cMonitors;
    int rc = SSMR3GetU32(pSSM, &cMonitors);
    if (cMonitors != that->mcMonitors)
        return SSMR3SetCfgError(pSSM, RT_SRC_POS, N_("Number of monitors changed (%d->%d)!"), cMonitors, that->mcMonitors);

    for (uint32_t i = 0; i < cMonitors; i++)
    {
        SSMR3GetU32(pSSM, &that->maFramebuffers[i].u32Offset);
        SSMR3GetU32(pSSM, &that->maFramebuffers[i].u32MaxFramebufferSize);
        SSMR3GetU32(pSSM, &that->maFramebuffers[i].u32InformationSize);
        if (   uVersion == sSSMDisplayVer2
            || uVersion == sSSMDisplayVer3
            || uVersion == sSSMDisplayVer4
            || uVersion == sSSMDisplayVer5)
        {
            uint32_t w;
            uint32_t h;
            SSMR3GetU32(pSSM, &w);
            SSMR3GetU32(pSSM, &h);
            that->maFramebuffers[i].w = w;
            that->maFramebuffers[i].h = h;
        }
        if (   uVersion == sSSMDisplayVer3
            || uVersion == sSSMDisplayVer4
            || uVersion == sSSMDisplayVer5)
        {
            int32_t xOrigin;
            int32_t yOrigin;
            uint32_t flags;
            SSMR3GetS32(pSSM, &xOrigin);
            SSMR3GetS32(pSSM, &yOrigin);
            SSMR3GetU32(pSSM, &flags);
            that->maFramebuffers[i].xOrigin = xOrigin;
            that->maFramebuffers[i].yOrigin = yOrigin;
            that->maFramebuffers[i].flags = (uint16_t)flags;
            that->maFramebuffers[i].fDisabled = (that->maFramebuffers[i].flags & VBVA_SCREEN_F_DISABLED) != 0;
        }
    }
    if (   uVersion == sSSMDisplayVer4
        || uVersion == sSSMDisplayVer5)
    {
        SSMR3GetS32(pSSM, &that->xInputMappingOrigin);
        SSMR3GetS32(pSSM, &that->yInputMappingOrigin);
        SSMR3GetU32(pSSM, &that->cxInputMapping);
        SSMR3GetU32(pSSM, &that->cyInputMapping);
    }
    if (uVersion == sSSMDisplayVer5)
    {
        SSMR3GetU32(pSSM, &that->mfGuestVBVACapabilities);
        SSMR3GetU32(pSSM, &that->mfHostCursorCapabilities);
    }

    return VINF_SUCCESS;
}

/**
 * Initializes the display object.
 *
 * @returns COM result indicator
 * @param parent          handle of our parent object
 * @param qemuConsoleData address of common console data structure
 */
HRESULT Display::init(Console *aParent)
{
    ComAssertRet(aParent, E_INVALIDARG);
    /* Enclose the state transition NotReady->InInit->Ready */
    AutoInitSpan autoInitSpan(this);
    AssertReturn(autoInitSpan.isOk(), E_FAIL);

    unconst(mParent) = aParent;

    ULONG ul;
    mParent->machine()->COMGETTER(MonitorCount)(&ul);
    mcMonitors = ul;
    xInputMappingOrigin = 0;
    yInputMappingOrigin = 0;
    cxInputMapping = 0;
    cyInputMapping = 0;

    for (ul = 0; ul < mcMonitors; ul++)
    {
        maFramebuffers[ul].u32Offset = 0;
        maFramebuffers[ul].u32MaxFramebufferSize = 0;
        maFramebuffers[ul].u32InformationSize = 0;

        maFramebuffers[ul].pFramebuffer = NULL;
        /* All secondary monitors are disabled at startup. */
        maFramebuffers[ul].fDisabled = ul > 0;

        maFramebuffers[ul].xOrigin = 0;
        maFramebuffers[ul].yOrigin = 0;

        maFramebuffers[ul].w = 0;
        maFramebuffers[ul].h = 0;

        maFramebuffers[ul].flags = maFramebuffers[ul].fDisabled? VBVA_SCREEN_F_DISABLED: 0;

        maFramebuffers[ul].u16BitsPerPixel = 0;
        maFramebuffers[ul].pu8FramebufferVRAM = NULL;
        maFramebuffers[ul].u32LineSize = 0;

        maFramebuffers[ul].pHostEvents = NULL;

        maFramebuffers[ul].u32ResizeStatus = ResizeStatus_Void;

        maFramebuffers[ul].fDefaultFormat = false;

        maFramebuffers[ul].mcSavedVisibleRegion = 0;
        maFramebuffers[ul].mpSavedVisibleRegion = NULL;

        RT_ZERO(maFramebuffers[ul].dirtyRect);
        RT_ZERO(maFramebuffers[ul].pendingResize);
#ifdef VBOX_WITH_HGSMI
        maFramebuffers[ul].fVBVAEnabled = false;
        maFramebuffers[ul].fRenderThreadMode = false;
        maFramebuffers[ul].cVBVASkipUpdate = 0;
        RT_ZERO(maFramebuffers[ul].vbvaSkippedRect);
        maFramebuffers[ul].pVBVAHostFlags = NULL;
#endif /* VBOX_WITH_HGSMI */
#ifdef VBOX_WITH_CROGL
        RT_ZERO(maFramebuffers[ul].pendingViewportInfo);
#endif
    }

    {
        // register listener for state change events
        ComPtr<IEventSource> es;
        mParent->COMGETTER(EventSource)(es.asOutParam());
        com::SafeArray <VBoxEventType_T> eventTypes;
        eventTypes.push_back(VBoxEventType_OnStateChanged);
        es->RegisterListener(this, ComSafeArrayAsInParam(eventTypes), true);
    }

    /* Cache the 3D settings. */
    BOOL fIs3DEnabled = FALSE;
    mParent->machine()->COMGETTER(Accelerate3DEnabled)(&fIs3DEnabled);
    GraphicsControllerType_T enmGpuType = (GraphicsControllerType_T)GraphicsControllerType_VBoxVGA;
    mParent->machine()->COMGETTER(GraphicsControllerType)(&enmGpuType);
    mfIsCr3DEnabled = fIs3DEnabled && enmGpuType == GraphicsControllerType_VBoxVGA;

    /* Confirm a successful initialization */
    autoInitSpan.setSucceeded();

    return S_OK;
}

/**
 *  Uninitializes the instance and sets the ready flag to FALSE.
 *  Called either from FinalRelease() or by the parent when it gets destroyed.
 */
void Display::uninit()
{
    LogRelFlowFunc(("this=%p\n", this));

    /* Enclose the state transition Ready->InUninit->NotReady */
    AutoUninitSpan autoUninitSpan(this);
    if (autoUninitSpan.uninitDone())
        return;

    ULONG ul;
    for (ul = 0; ul < mcMonitors; ul++)
        maFramebuffers[ul].pFramebuffer = NULL;

    if (mParent)
    {
        ComPtr<IEventSource> es;
        mParent->COMGETTER(EventSource)(es.asOutParam());
        es->UnregisterListener(this);
    }

    unconst(mParent) = NULL;

    if (mpDrv)
        mpDrv->pDisplay = NULL;

    mpDrv = NULL;
    mpVMMDev = NULL;
    mfVMMDevInited = true;
}

/**
 * Register the SSM methods. Called by the power up thread to be able to
 * pass pVM
 */
int Display::registerSSM(PUVM pUVM)
{
    /* Version 2 adds width and height of the framebuffer; version 3 adds
     * the framebuffer offset in the virtual desktop and the framebuffer flags;
     * version 4 adds guest to host input event mapping and version 5 adds
     * guest VBVA and host cursor capabilities.
     */
    int rc = SSMR3RegisterExternal(pUVM, "DisplayData", 0, sSSMDisplayVer5,
                                   mcMonitors * sizeof(uint32_t) * 8 + sizeof(uint32_t),
                                   NULL, NULL, NULL,
                                   NULL, displaySSMSave, NULL,
                                   NULL, displaySSMLoad, NULL, this);
    AssertRCReturn(rc, rc);

    /*
     * Register loaders for old saved states where iInstance was
     * 3 * sizeof(uint32_t *) due to a code mistake.
     */
    rc = SSMR3RegisterExternal(pUVM, "DisplayData", 12 /*uInstance*/, sSSMDisplayVer, 0 /*cbGuess*/,
                               NULL, NULL, NULL,
                               NULL, NULL, NULL,
                               NULL, displaySSMLoad, NULL, this);
    AssertRCReturn(rc, rc);

    rc = SSMR3RegisterExternal(pUVM, "DisplayData", 24 /*uInstance*/, sSSMDisplayVer, 0 /*cbGuess*/,
                               NULL, NULL, NULL,
                               NULL, NULL, NULL,
                               NULL, displaySSMLoad, NULL, this);
    AssertRCReturn(rc, rc);

    /* uInstance is an arbitrary value greater than 1024. Such a value will ensure a quick seek in saved state file. */
    rc = SSMR3RegisterExternal(pUVM, "DisplayScreenshot", 1100 /*uInstance*/, sSSMDisplayScreenshotVer, 0 /*cbGuess*/,
                               NULL, NULL, NULL,
                               NULL, displaySSMSaveScreenshot, NULL,
                               NULL, displaySSMLoadScreenshot, NULL, this);

    AssertRCReturn(rc, rc);

    return VINF_SUCCESS;
}

DECLCALLBACK(void) Display::displayCrCmdFree(struct VBOXCRCMDCTL* pCmd, uint32_t cbCmd, int rc, void *pvCompletion)
{
    Assert(pvCompletion);
    RTMemFree(pvCompletion);
}

#if defined(VBOX_WITH_HGCM) && defined(VBOX_WITH_CROGL)
int Display::crOglWindowsShow(bool fShow)
{
    if (!mfCrOglDataHidden == !!fShow)
        return VINF_SUCCESS;

    if (!mhCrOglSvc)
    {
        /* No 3D or the VMSVGA3d kind. */
        Assert(!mfIsCr3DEnabled);
        return VERR_INVALID_STATE;
    }

    VMMDev *pVMMDev = mParent->getVMMDev();
    if (!pVMMDev)
    {
        AssertMsgFailed(("no vmmdev\n"));
        return VERR_INVALID_STATE;
    }

    VBOXCRCMDCTL_HGCM *pData = (VBOXCRCMDCTL_HGCM*)RTMemAlloc(sizeof (VBOXCRCMDCTL_HGCM));
    if (!pData)
    {
        AssertMsgFailed(("RTMemAlloc failed\n"));
        return VERR_NO_MEMORY;
    }

    pData->Hdr.enmType = VBOXCRCMDCTL_TYPE_HGCM;
    pData->Hdr.u32Function = SHCRGL_HOST_FN_WINDOWS_SHOW;

    pData->aParms[0].type = VBOX_HGCM_SVC_PARM_32BIT;
    pData->aParms[0].u.uint32 = (uint32_t)fShow;

    int rc = crCtlSubmit(&pData->Hdr, sizeof (*pData), displayCrCmdFree, pData);
    if (RT_SUCCESS(rc))
        mfCrOglDataHidden = !fShow;
    else
    {
        AssertMsgFailed(("crCtlSubmit failed rc %d\n", rc));
        RTMemFree(pData);
    }

    return rc;
}
#endif


// IEventListener method
STDMETHODIMP Display::HandleEvent(IEvent * aEvent)
{
    VBoxEventType_T aType = VBoxEventType_Invalid;

    aEvent->COMGETTER(Type)(&aType);
    switch (aType)
    {
        case VBoxEventType_OnStateChanged:
        {
            ComPtr<IStateChangedEvent> scev = aEvent;
            Assert(scev);
            MachineState_T machineState;
            scev->COMGETTER(State)(&machineState);
            if (   machineState == MachineState_Running
                   || machineState == MachineState_Teleporting
                   || machineState == MachineState_LiveSnapshotting
                   )
            {
                LogRelFlowFunc(("Machine is running.\n"));

                mfMachineRunning = true;

#ifdef VBOX_WITH_CROGL
                crOglWindowsShow(true);
#endif
            }
            else
            {
                mfMachineRunning = false;

#ifdef VBOX_WITH_CROGL
                if (machineState == MachineState_Paused)
                    crOglWindowsShow(false);
#endif
            }
            break;
        }
        default:
            AssertFailed();
    }

    return S_OK;
}

// public methods only for internal purposes
/////////////////////////////////////////////////////////////////////////////

/**
 *  @thread EMT
 */
static int callFramebufferResize (IFramebuffer *pFramebuffer, unsigned uScreenId,
                                  ULONG pixelFormat, void *pvVRAM,
                                  uint32_t bpp, uint32_t cbLine,
                                  uint32_t w, uint32_t h)
{
    Assert (pFramebuffer);

    /* Call the framebuffer to try and set required pixelFormat. */
    BOOL finished = TRUE;

    pFramebuffer->RequestResize (uScreenId, pixelFormat, (BYTE *) pvVRAM,
                                 bpp, cbLine, w, h, &finished);

    if (!finished)
    {
        LogRelFlowFunc(("External framebuffer wants us to wait!\n"));
        return VINF_VGA_RESIZE_IN_PROGRESS;
    }

    return VINF_SUCCESS;
}

int Display::notifyCroglResize(const PVBVAINFOVIEW pView, const PVBVAINFOSCREEN pScreen, void *pvVRAM)
{
#if defined(VBOX_WITH_HGCM) && defined(VBOX_WITH_CROGL)
    if (maFramebuffers[pScreen->u32ViewIndex].fRenderThreadMode)
        return VINF_SUCCESS; /* nop it */

    if (mfIsCr3DEnabled)
    {
        int rc = VERR_INVALID_STATE;
        if (mhCrOglSvc)
        {
            VMMDev *pVMMDev = mParent->getVMMDev();
            if (pVMMDev)
            {
                VBOXCRCMDCTL_HGCM *pCtl;
                pCtl = (VBOXCRCMDCTL_HGCM*)RTMemAlloc(sizeof(CRVBOXHGCMDEVRESIZE) + sizeof(VBOXCRCMDCTL_HGCM));
                if (pCtl)
                {
                    CRVBOXHGCMDEVRESIZE *pData = (CRVBOXHGCMDEVRESIZE*)(pCtl+1);
                    pData->Screen = *pScreen;
                    pData->pvVRAM = pvVRAM;

                    pCtl->Hdr.enmType = VBOXCRCMDCTL_TYPE_HGCM;
                    pCtl->Hdr.u32Function = SHCRGL_HOST_FN_DEV_RESIZE;
                    pCtl->aParms[0].type = VBOX_HGCM_SVC_PARM_PTR;
                    pCtl->aParms[0].u.pointer.addr = pData;
                    pCtl->aParms[0].u.pointer.size = sizeof (*pData);

                    rc = crCtlSubmit(&pCtl->Hdr, sizeof (*pCtl), displayCrCmdFree, pCtl);
                    if (RT_FAILURE(rc))
                    {
                        AssertMsgFailed(("crCtlSubmit failed rc %d\n", rc));
                        RTMemFree(pCtl);
                    }
                }
                else
                    rc = VERR_NO_MEMORY;
            }
        }

        return rc;
    }
#endif /* #if defined(VBOX_WITH_HGCM) && defined(VBOX_WITH_CROGL) */
    return VINF_SUCCESS;
}

/**
 *  Handles display resize event.
 *  Disables access to VGA device;
 *  calls the framebuffer RequestResize method;
 *  if framebuffer resizes synchronously,
 *      updates the display connector data and enables access to the VGA device.
 *
 *  @param w New display width
 *  @param h New display height
 *
 *  @thread EMT
 */
int Display::handleDisplayResize (unsigned uScreenId, uint32_t bpp, void *pvVRAM,
                                  uint32_t cbLine, uint32_t w, uint32_t h, uint16_t flags)
{
    LogRel(("Display::handleDisplayResize(): uScreenId = %d, pvVRAM=%p "
            "w=%d h=%d bpp=%d cbLine=0x%X, flags=0x%X\n",
            uScreenId, pvVRAM, w, h, bpp, cbLine, flags));

    /* If there is no framebuffer, this call is not interesting. */
    if (   uScreenId >= mcMonitors
        || maFramebuffers[uScreenId].pFramebuffer.isNull())
    {
        return VINF_SUCCESS;
    }

    if (uScreenId == VBOX_VIDEO_PRIMARY_SCREEN)
    {
        mLastAddress = pvVRAM;
        mLastBytesPerLine = cbLine;
        mLastBitsPerPixel = bpp;
        mLastWidth = w;
        mLastHeight = h;
        mLastFlags = flags;
    }

    ULONG pixelFormat;

    switch (bpp)
    {
        case 32:
        case 24:
        case 16:
            pixelFormat = FramebufferPixelFormat_FOURCC_RGB;
            break;
        default:
            pixelFormat = FramebufferPixelFormat_Opaque;
            bpp = cbLine = 0;
            break;
    }

    /* Atomically set the resize status before calling the framebuffer. The new InProgress status will
     * disable access to the VGA device by the EMT thread.
     */
    bool f = ASMAtomicCmpXchgU32 (&maFramebuffers[uScreenId].u32ResizeStatus,
                                  ResizeStatus_InProgress, ResizeStatus_Void);
    if (!f)
    {
        /* This could be a result of the screenshot taking call Display::TakeScreenShot:
         * if the framebuffer is processing the resize request and GUI calls the TakeScreenShot
         * and the guest has reprogrammed the virtual VGA devices again so a new resize is required.
         *
         * Save the resize information and return the pending status code.
         *
         * Note: the resize information is only accessed on EMT so no serialization is required.
         */
        LogRel(("Display::handleDisplayResize(): Warning: resize postponed.\n"));

        maFramebuffers[uScreenId].pendingResize.fPending    = true;
        maFramebuffers[uScreenId].pendingResize.pixelFormat = pixelFormat;
        maFramebuffers[uScreenId].pendingResize.pvVRAM      = pvVRAM;
        maFramebuffers[uScreenId].pendingResize.bpp         = bpp;
        maFramebuffers[uScreenId].pendingResize.cbLine      = cbLine;
        maFramebuffers[uScreenId].pendingResize.w           = w;
        maFramebuffers[uScreenId].pendingResize.h           = h;
        maFramebuffers[uScreenId].pendingResize.flags       = flags;

        return VINF_VGA_RESIZE_IN_PROGRESS;
    }

    /* Framebuffer will be invalid during resize, make sure that it is not accessed. */
    if (uScreenId == VBOX_VIDEO_PRIMARY_SCREEN)
        mpDrv->pUpPort->pfnSetRenderVRAM (mpDrv->pUpPort, false);

    int rc = callFramebufferResize (maFramebuffers[uScreenId].pFramebuffer, uScreenId,
                                    pixelFormat, pvVRAM, bpp, cbLine, w, h);
    if (rc == VINF_VGA_RESIZE_IN_PROGRESS)
    {
        /* Immediately return to the caller. ResizeCompleted will be called back by the
         * GUI thread. The ResizeCompleted callback will change the resize status from
         * InProgress to UpdateDisplayData. The latter status will be checked by the
         * display timer callback on EMT and all required adjustments will be done there.
         */
        return rc;
    }

    /* Set the status so the 'handleResizeCompleted' would work.  */
    f = ASMAtomicCmpXchgU32 (&maFramebuffers[uScreenId].u32ResizeStatus,
                             ResizeStatus_UpdateDisplayData, ResizeStatus_InProgress);
    AssertRelease(f);NOREF(f);

    AssertRelease(!maFramebuffers[uScreenId].pendingResize.fPending);

    /* The method also unlocks the framebuffer. */
    handleResizeCompletedEMT(TRUE);

    return VINF_SUCCESS;
}

/**
 *  Framebuffer has been resized.
 *  Read the new display data and unlock the framebuffer.
 *
 *  @thread EMT
 */
void Display::handleResizeCompletedEMT(BOOL fResizeContext)
{
    LogRelFlowFunc(("\n"));

    unsigned uScreenId;
    for (uScreenId = 0; uScreenId < mcMonitors; uScreenId++)
    {
        DISPLAYFBINFO *pFBInfo = &maFramebuffers[uScreenId];

        /* Try to into non resizing state. */
        bool f = ASMAtomicCmpXchgU32 (&pFBInfo->u32ResizeStatus, ResizeStatus_Void, ResizeStatus_UpdateDisplayData);

        if (f == false)
        {
            /* This is not the display that has completed resizing. */
            continue;
        }

        /* Check whether a resize is pending for this framebuffer. */
        if (pFBInfo->pendingResize.fPending)
        {
            /* Reset the condition, call the display resize with saved data and continue.
             *
             * Note: handleDisplayResize can call handleResizeCompletedEMT back,
             *       but infinite recursion is not possible, because when the handleResizeCompletedEMT
             *       is called, the pFBInfo->pendingResize.fPending is equal to false.
             */
            pFBInfo->pendingResize.fPending = false;
            handleDisplayResize (uScreenId, pFBInfo->pendingResize.bpp, pFBInfo->pendingResize.pvVRAM,
                                 pFBInfo->pendingResize.cbLine, pFBInfo->pendingResize.w, pFBInfo->pendingResize.h, pFBInfo->pendingResize.flags);
            continue;
        }

        /* Inform VRDP server about the change of display parameters.
         * Must be done before calling NotifyUpdate below.
         */
        LogRelFlowFunc(("Calling VRDP\n"));
        mParent->consoleVRDPServer()->SendResize();

        /* @todo Merge these two 'if's within one 'if (!pFBInfo->pFramebuffer.isNull())' */
        if (uScreenId == VBOX_VIDEO_PRIMARY_SCREEN && !pFBInfo->pFramebuffer.isNull())
        {
            /* Primary framebuffer has completed the resize. Update the connector data for VGA device. */
            int rc2 = updateDisplayData();

            /* Check the framebuffer pixel format to setup the rendering in VGA device. */
            BOOL usesGuestVRAM = FALSE;
            pFBInfo->pFramebuffer->COMGETTER(UsesGuestVRAM) (&usesGuestVRAM);

            pFBInfo->fDefaultFormat = (usesGuestVRAM == FALSE);

            /* If the primary framebuffer is disabled, tell the VGA device to not to copy
             * pixels from VRAM to the framebuffer.
             */
            if (pFBInfo->fDisabled || RT_FAILURE(rc2))
                mpDrv->pUpPort->pfnSetRenderVRAM (mpDrv->pUpPort, false);
            else
                mpDrv->pUpPort->pfnSetRenderVRAM (mpDrv->pUpPort,
                                                  pFBInfo->fDefaultFormat);

            /* If the screen resize was because of disabling, tell framebuffer to repaint.
             * The framebuffer if now in default format so it will not use guest VRAM
             * and will show usually black image which is there after framebuffer resize.
             */
            if (pFBInfo->fDisabled)
                pFBInfo->pFramebuffer->NotifyUpdate(0, 0, mpDrv->IConnector.cx, mpDrv->IConnector.cy);
        }
        else if (!pFBInfo->pFramebuffer.isNull())
        {
            BOOL usesGuestVRAM = FALSE;
            pFBInfo->pFramebuffer->COMGETTER(UsesGuestVRAM) (&usesGuestVRAM);

            pFBInfo->fDefaultFormat = (usesGuestVRAM == FALSE);

            /* If the screen resize was because of disabling, tell framebuffer to repaint.
             * The framebuffer if now in default format so it will not use guest VRAM
             * and will show usually black image which is there after framebuffer resize.
             */
            if (pFBInfo->fDisabled)
                pFBInfo->pFramebuffer->NotifyUpdate(0, 0, pFBInfo->w, pFBInfo->h);
        }
        LogRelFlow(("[%d]: default format %d\n", uScreenId, pFBInfo->fDefaultFormat));

        /* Handle the case if there are some saved visible region that needs to be
         * applied after the resize of the framebuffer is completed
         */
        SaveSeamlessRectLock();
        PRTRECT pSavedVisibleRegion = pFBInfo->mpSavedVisibleRegion;
        uint32_t cSavedVisibleRegion = pFBInfo->mcSavedVisibleRegion;
        pFBInfo->mpSavedVisibleRegion = NULL;
        pFBInfo->mcSavedVisibleRegion = 0;
        SaveSeamlessRectUnLock();

        if (pSavedVisibleRegion)
        {
            handleSetVisibleRegion(cSavedVisibleRegion, pSavedVisibleRegion);
            RTMemFree(pSavedVisibleRegion);
        }

#ifdef DEBUG_sunlover
        if (!g_stam)
        {
            Console::SafeVMPtr ptrVM(mParent);
            AssertComRC(ptrVM.rc());
            STAMR3RegisterU(ptrVM.rawUVM(), &g_StatDisplayRefresh, STAMTYPE_PROFILE, STAMVISIBILITY_ALWAYS,
                            "/PROF/Display/Refresh", STAMUNIT_TICKS_PER_CALL, "Time spent in EMT for display updates.");
            g_stam = 1;
        }
#endif /* DEBUG_sunlover */
    }
}

static void checkCoordBounds (int *px, int *py, int *pw, int *ph, int cx, int cy)
{
    /* Correct negative x and y coordinates. */
    if (*px < 0)
    {
        *px += *pw; /* Compute xRight which is also the new width. */

        *pw = (*px < 0)? 0: *px;

        *px = 0;
    }

    if (*py < 0)
    {
        *py += *ph; /* Compute xBottom, which is also the new height. */

        *ph = (*py < 0)? 0: *py;

        *py = 0;
    }

    /* Also check if coords are greater than the display resolution. */
    if (*px + *pw > cx)
    {
        *pw = cx > *px? cx - *px: 0;
    }

    if (*py + *ph > cy)
    {
        *ph = cy > *py? cy - *py: 0;
    }
}

unsigned mapCoordsToScreen(DISPLAYFBINFO *pInfos, unsigned cInfos, int *px, int *py, int *pw, int *ph)
{
    DISPLAYFBINFO *pInfo = pInfos;
    unsigned uScreenId;
    LogSunlover(("mapCoordsToScreen: %d,%d %dx%d\n", *px, *py, *pw, *ph));
    for (uScreenId = 0; uScreenId < cInfos; uScreenId++, pInfo++)
    {
        LogSunlover(("    [%d] %d,%d %dx%d\n", uScreenId, pInfo->xOrigin, pInfo->yOrigin, pInfo->w, pInfo->h));
        if (   (pInfo->xOrigin <= *px && *px < pInfo->xOrigin + (int)pInfo->w)
            && (pInfo->yOrigin <= *py && *py < pInfo->yOrigin + (int)pInfo->h))
        {
            /* The rectangle belongs to the screen. Correct coordinates. */
            *px -= pInfo->xOrigin;
            *py -= pInfo->yOrigin;
            LogSunlover(("    -> %d,%d", *px, *py));
            break;
        }
    }
    if (uScreenId == cInfos)
    {
        /* Map to primary screen. */
        uScreenId = 0;
    }
    LogSunlover((" scr %d\n", uScreenId));
    return uScreenId;
}


/**
 *  Handles display update event.
 *
 *  @param x Update area x coordinate
 *  @param y Update area y coordinate
 *  @param w Update area width
 *  @param h Update area height
 *
 *  @thread EMT
 */
void Display::handleDisplayUpdateLegacy (int x, int y, int w, int h)
{
    unsigned uScreenId = mapCoordsToScreen(maFramebuffers, mcMonitors, &x, &y, &w, &h);

#ifdef DEBUG_sunlover
    LogFlowFunc(("%d,%d %dx%d (checked)\n", x, y, w, h));
#endif /* DEBUG_sunlover */

    handleDisplayUpdate (uScreenId, x, y, w, h);
}

void Display::handleDisplayUpdate (unsigned uScreenId, int x, int y, int w, int h)
{
    /*
     * Always runs under either VBVA lock or, for HGSMI, DevVGA lock.
     * Safe to use VBVA vars and take the framebuffer lock.
     */

#ifdef DEBUG_sunlover
    LogFlowFunc(("[%d] %d,%d %dx%d (%d,%d)\n",
                 uScreenId, x, y, w, h, mpDrv->IConnector.cx, mpDrv->IConnector.cy));
#endif /* DEBUG_sunlover */

    IFramebuffer *pFramebuffer = maFramebuffers[uScreenId].pFramebuffer;

    // if there is no framebuffer, this call is not interesting
    if (   pFramebuffer == NULL
        || maFramebuffers[uScreenId].fDisabled)
        return;

    pFramebuffer->Lock();

    if (uScreenId == VBOX_VIDEO_PRIMARY_SCREEN)
        checkCoordBounds (&x, &y, &w, &h, mpDrv->IConnector.cx, mpDrv->IConnector.cy);
    else
        checkCoordBounds (&x, &y, &w, &h, maFramebuffers[uScreenId].w,
                                          maFramebuffers[uScreenId].h);

    if (w != 0 && h != 0)
        pFramebuffer->NotifyUpdate(x, y, w, h);

    pFramebuffer->Unlock();

#ifndef VBOX_WITH_HGSMI
    if (!mfVideoAccelEnabled)
    {
#else
    if (!mfVideoAccelEnabled && !maFramebuffers[uScreenId].fVBVAEnabled)
    {
#endif /* VBOX_WITH_HGSMI */
        /* When VBVA is enabled, the VRDP server is informed in the VideoAccelFlush.
         * Inform the server here only if VBVA is disabled.
         */
        if (maFramebuffers[uScreenId].u32ResizeStatus == ResizeStatus_Void)
            mParent->consoleVRDPServer()->SendUpdateBitmap(uScreenId, x, y, w, h);
    }
}

void Display::i_updateGuestGraphicsFacility(void)
{
    Guest* pGuest = mParent->getGuest();
    AssertPtrReturnVoid(pGuest);
    /* The following is from GuestImpl.cpp. */
    /** @todo A nit: The timestamp is wrong on saved state restore. Would be better
     *  to move the graphics and seamless capability -> facility translation to
     *  VMMDev so this could be saved.  */
    RTTIMESPEC TimeSpecTS;
    RTTimeNow(&TimeSpecTS);

    if (   mfVMMDevSupportsGraphics
        || (mfGuestVBVACapabilities & VBVACAPS_VIDEO_MODE_HINTS) != 0)
        pGuest->setAdditionsStatus(VBoxGuestFacilityType_Graphics,
                                   VBoxGuestFacilityStatus_Active,
                                   0 /*fFlags*/, &TimeSpecTS);
    else
        pGuest->setAdditionsStatus(VBoxGuestFacilityType_Graphics,
                                   VBoxGuestFacilityStatus_Inactive,
                                   0 /*fFlags*/, &TimeSpecTS);
}

void Display::i_handleUpdateVMMDevSupportsGraphics(bool fSupportsGraphics)
{
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
    if (mfVMMDevSupportsGraphics == fSupportsGraphics)
        return;
    mfVMMDevSupportsGraphics = fSupportsGraphics;
    i_updateGuestGraphicsFacility();
    /* The VMMDev interface notifies the console. */
}

void Display::i_handleUpdateGuestVBVACapabilities(uint32_t fNewCapabilities)
{
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
    bool fNotify = (fNewCapabilities & VBVACAPS_VIDEO_MODE_HINTS) != 0;

    mfGuestVBVACapabilities = fNewCapabilities;
    if (!fNotify)
        return;
    i_updateGuestGraphicsFacility();
    /* Tell the console about it */
    mParent->onAdditionsStateChange();
}

void Display::i_handleUpdateVBVAInputMapping(int32_t xOrigin, int32_t yOrigin, uint32_t cx, uint32_t cy)
{
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    xInputMappingOrigin = xOrigin;
    yInputMappingOrigin = yOrigin;
    cxInputMapping      = cx;
    cyInputMapping      = cy;
}

/**
 * Returns the upper left and lower right corners of the virtual framebuffer.
 * The lower right is "exclusive" (i.e. first pixel beyond the framebuffer),
 * and the origin is (0, 0), not (1, 1) like the GUI returns.
 */
void Display::getFramebufferDimensions(int32_t *px1, int32_t *py1,
                                       int32_t *px2, int32_t *py2)
{
    int32_t x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    AssertPtrReturnVoid(px1);
    AssertPtrReturnVoid(py1);
    AssertPtrReturnVoid(px2);
    AssertPtrReturnVoid(py2);
    LogRelFlowFunc(("\n"));

    if (!mpDrv)
        return;
    /* If VBVA is not in use then this flag will not be set and this
     * will still work as it should. */
    if (!maFramebuffers[0].fDisabled)
    {
        x1 = (int32_t)maFramebuffers[0].xOrigin;
        y1 = (int32_t)maFramebuffers[0].yOrigin;
        x2 = mpDrv->IConnector.cx + (int32_t)maFramebuffers[0].xOrigin;
        y2 = mpDrv->IConnector.cy + (int32_t)maFramebuffers[0].yOrigin;
    }
    if (cxInputMapping && cyInputMapping)
    {
        x1 = xInputMappingOrigin;
        y1 = yInputMappingOrigin;
        x2 = xInputMappingOrigin + cxInputMapping;
        y2 = yInputMappingOrigin + cyInputMapping;
    }
    else
        for (unsigned i = 1; i < mcMonitors; ++i)
        {
            if (!maFramebuffers[i].fDisabled)
            {
                x1 = RT_MIN(x1, maFramebuffers[i].xOrigin);
                y1 = RT_MIN(y1, maFramebuffers[i].yOrigin);
                x2 = RT_MAX(x2, maFramebuffers[i].xOrigin + (int32_t)maFramebuffers[i].w);
                y2 = RT_MAX(y2, maFramebuffers[i].yOrigin + (int32_t)maFramebuffers[i].h);
            }
        }
    *px1 = x1;
    *py1 = y1;
    *px2 = x2;
    *py2 = y2;
}

HRESULT Display::i_reportHostCursorCapabilities(uint32_t fCapabilitiesAdded, uint32_t fCapabilitiesRemoved)
{
    /* Do we need this to access mParent?  I presume that the safe VM pointer
     * ensures that mpDrv will remain valid. */
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);
    uint32_t fHostCursorCapabilities =   (mfHostCursorCapabilities | fCapabilitiesAdded)
                                       & ~fCapabilitiesRemoved;

    Console::SafeVMPtr ptrVM(mParent);
    if (!ptrVM.isOk())
        return ptrVM.rc();
    if (mfHostCursorCapabilities == fHostCursorCapabilities)
        return S_OK;
    CHECK_CONSOLE_DRV(mpDrv);
    alock.release();  /* Release before calling up for lock order reasons. */
    mpDrv->pUpPort->pfnReportHostCursorCapabilities (mpDrv->pUpPort, fCapabilitiesAdded, fCapabilitiesRemoved);
    mfHostCursorCapabilities = fHostCursorCapabilities;
    return S_OK;
}

HRESULT Display::i_reportHostCursorPosition(int32_t x, int32_t y)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);
    uint32_t xAdj = (uint32_t)RT_MAX(x - xInputMappingOrigin, 0);
    uint32_t yAdj = (uint32_t)RT_MAX(y - yInputMappingOrigin, 0);
    xAdj = RT_MIN(xAdj, cxInputMapping);
    yAdj = RT_MIN(yAdj, cyInputMapping);

    Console::SafeVMPtr ptrVM(mParent);
    if (!ptrVM.isOk())
        return ptrVM.rc();
    CHECK_CONSOLE_DRV(mpDrv);
    alock.release();  /* Release before calling up for lock order reasons. */
    mpDrv->pUpPort->pfnReportHostCursorPosition(mpDrv->pUpPort, xAdj, yAdj);
    return S_OK;
}

static bool displayIntersectRect(RTRECT *prectResult,
                                 const RTRECT *prect1,
                                 const RTRECT *prect2)
{
    /* Initialize result to an empty record. */
    memset (prectResult, 0, sizeof (RTRECT));

    int xLeftResult = RT_MAX(prect1->xLeft, prect2->xLeft);
    int xRightResult = RT_MIN(prect1->xRight, prect2->xRight);

    if (xLeftResult < xRightResult)
    {
        /* There is intersection by X. */

        int yTopResult = RT_MAX(prect1->yTop, prect2->yTop);
        int yBottomResult = RT_MIN(prect1->yBottom, prect2->yBottom);

        if (yTopResult < yBottomResult)
        {
            /* There is intersection by Y. */

            prectResult->xLeft   = xLeftResult;
            prectResult->yTop    = yTopResult;
            prectResult->xRight  = xRightResult;
            prectResult->yBottom = yBottomResult;

            return true;
        }
    }

    return false;
}

int Display::handleSetVisibleRegion(uint32_t cRect, PRTRECT pRect)
{
    RTRECT *pVisibleRegion = (RTRECT *)RTMemTmpAlloc(  RT_MAX(cRect, 1)
                                                     * sizeof (RTRECT));
    if (!pVisibleRegion)
    {
        return VERR_NO_TMP_MEMORY;
    }

    unsigned uScreenId;
    for (uScreenId = 0; uScreenId < mcMonitors; uScreenId++)
    {
        DISPLAYFBINFO *pFBInfo = &maFramebuffers[uScreenId];

        if (!pFBInfo->pFramebuffer.isNull())
        {
            if (pFBInfo->u32ResizeStatus != ResizeStatus_Void)
            {
                /* handle the case where new rectangles are received from the GA
                 * when framebuffer resizing is in progress.
                 * Just save the rectangles to be applied for later time when FB resizing is complete
                 * (from handleResizeCompletedEMT).
                 * This is done to prevent a race condition where a new rectangles are received
                 * from the GA after a resize event and framebuffer resizing is still in progress
                 * As a result the coordinates of the framebuffer are still
                 * not updated and hence there is no intersection with the new rectangles passed
                 * for the new region (THis is checked in the above if condition ). With 0 intersection,
                 * cRectVisibleRegions = 0  is returned to the GUI and if GUI has invalidated its
                 * earlier region then it draws nothihing and seamless mode doesn't display the
                 * guest desktop.
                 */
                SaveSeamlessRectLock();
                RTMemFree(pFBInfo->mpSavedVisibleRegion);

                pFBInfo->mpSavedVisibleRegion = (RTRECT *)RTMemAlloc( RT_MAX(cRect, 1)
                                                                     * sizeof (RTRECT));
                if (pFBInfo->mpSavedVisibleRegion)
                {
                    memcpy(pFBInfo->mpSavedVisibleRegion, pRect, cRect * sizeof(RTRECT));
                    pFBInfo->mcSavedVisibleRegion = cRect;
                }
                else
                {
                    pFBInfo->mcSavedVisibleRegion = 0;
                }
                SaveSeamlessRectUnLock();
                continue;
            }
            /* Prepare a new array of rectangles which intersect with the framebuffer.
             */
            RTRECT rectFramebuffer;
            if (uScreenId == VBOX_VIDEO_PRIMARY_SCREEN)
            {
                rectFramebuffer.xLeft   = 0;
                rectFramebuffer.yTop    = 0;
                if (mpDrv)
                {
                    rectFramebuffer.xRight  = mpDrv->IConnector.cx;
                    rectFramebuffer.yBottom = mpDrv->IConnector.cy;
                }
                else
                {
                    rectFramebuffer.xRight  = 0;
                    rectFramebuffer.yBottom = 0;
                }
            }
            else
            {
                rectFramebuffer.xLeft   = pFBInfo->xOrigin;
                rectFramebuffer.yTop    = pFBInfo->yOrigin;
                rectFramebuffer.xRight  = pFBInfo->xOrigin + pFBInfo->w;
                rectFramebuffer.yBottom = pFBInfo->yOrigin + pFBInfo->h;
            }

            uint32_t cRectVisibleRegion = 0;

            uint32_t i;
            for (i = 0; i < cRect; i++)
            {
                if (displayIntersectRect(&pVisibleRegion[cRectVisibleRegion], &pRect[i], &rectFramebuffer))
                {
                    pVisibleRegion[cRectVisibleRegion].xLeft -= pFBInfo->xOrigin;
                    pVisibleRegion[cRectVisibleRegion].yTop -= pFBInfo->yOrigin;
                    pVisibleRegion[cRectVisibleRegion].xRight -= pFBInfo->xOrigin;
                    pVisibleRegion[cRectVisibleRegion].yBottom -= pFBInfo->yOrigin;

                    cRectVisibleRegion++;
                }
            }
            pFBInfo->pFramebuffer->SetVisibleRegion((BYTE *)pVisibleRegion, cRectVisibleRegion);
        }
    }

#if defined(VBOX_WITH_HGCM) && defined(VBOX_WITH_CROGL)
    VMMDev *vmmDev = mParent->getVMMDev();
    if (mfIsCr3DEnabled && vmmDev)
    {
        if (mhCrOglSvc)
        {
            VBOXCRCMDCTL_HGCM *pCtl;
            pCtl = (VBOXCRCMDCTL_HGCM*)RTMemAlloc(RT_MAX(cRect, 1) * sizeof(RTRECT) + sizeof(VBOXCRCMDCTL_HGCM));
            if (pCtl)
            {
                RTRECT *pRectsCopy = (RTRECT*)(pCtl+1);
                memcpy(pRectsCopy, pRect, cRect * sizeof (RTRECT));

                pCtl->Hdr.enmType = VBOXCRCMDCTL_TYPE_HGCM;
                pCtl->Hdr.u32Function = SHCRGL_HOST_FN_SET_VISIBLE_REGION;

                pCtl->aParms[0].type = VBOX_HGCM_SVC_PARM_PTR;
                pCtl->aParms[0].u.pointer.addr = pRectsCopy;
                pCtl->aParms[0].u.pointer.size = cRect * sizeof (RTRECT);

                int rc = crCtlSubmit(&pCtl->Hdr, sizeof (*pCtl), displayCrCmdFree, pCtl);
                if (!RT_SUCCESS(rc))
                {
                    AssertMsgFailed(("crCtlSubmit failed rc %d\n", rc));
                    RTMemFree(pCtl);
                }
            }
            else
                AssertMsgFailed(("failed to allocate rects memory\n"));
        }
        else
            AssertMsgFailed(("mhCrOglSvc is NULL\n"));
    }
#endif

    RTMemTmpFree(pVisibleRegion);

    return VINF_SUCCESS;
}

int Display::handleQueryVisibleRegion(uint32_t *pcRect, PRTRECT pRect)
{
    // @todo Currently not used by the guest and is not implemented in framebuffers. Remove?
    return VERR_NOT_SUPPORTED;
}

typedef struct _VBVADIRTYREGION
{
    /* Copies of object's pointers used by vbvaRgn functions. */
    DISPLAYFBINFO    *paFramebuffers;
    unsigned          cMonitors;
    Display          *pDisplay;
    PPDMIDISPLAYPORT  pPort;

} VBVADIRTYREGION;

static void vbvaRgnInit (VBVADIRTYREGION *prgn, DISPLAYFBINFO *paFramebuffers, unsigned cMonitors, Display *pd, PPDMIDISPLAYPORT pp)
{
    prgn->paFramebuffers = paFramebuffers;
    prgn->cMonitors = cMonitors;
    prgn->pDisplay = pd;
    prgn->pPort = pp;

    unsigned uScreenId;
    for (uScreenId = 0; uScreenId < cMonitors; uScreenId++)
    {
        DISPLAYFBINFO *pFBInfo = &prgn->paFramebuffers[uScreenId];

        RT_ZERO(pFBInfo->dirtyRect);
    }
}

static void vbvaRgnDirtyRect (VBVADIRTYREGION *prgn, unsigned uScreenId, VBVACMDHDR *phdr)
{
    LogSunlover(("x = %d, y = %d, w = %d, h = %d\n",
                 phdr->x, phdr->y, phdr->w, phdr->h));

    /*
     * Here update rectangles are accumulated to form an update area.
     * @todo
     * Now the simplest method is used which builds one rectangle that
     * includes all update areas. A bit more advanced method can be
     * employed here. The method should be fast however.
     */
    if (phdr->w == 0 || phdr->h == 0)
    {
        /* Empty rectangle. */
        return;
    }

    int32_t xRight  = phdr->x + phdr->w;
    int32_t yBottom = phdr->y + phdr->h;

    DISPLAYFBINFO *pFBInfo = &prgn->paFramebuffers[uScreenId];

    if (pFBInfo->dirtyRect.xRight == 0)
    {
        /* This is the first rectangle to be added. */
        pFBInfo->dirtyRect.xLeft   = phdr->x;
        pFBInfo->dirtyRect.yTop    = phdr->y;
        pFBInfo->dirtyRect.xRight  = xRight;
        pFBInfo->dirtyRect.yBottom = yBottom;
    }
    else
    {
        /* Adjust region coordinates. */
        if (pFBInfo->dirtyRect.xLeft > phdr->x)
        {
            pFBInfo->dirtyRect.xLeft = phdr->x;
        }

        if (pFBInfo->dirtyRect.yTop > phdr->y)
        {
            pFBInfo->dirtyRect.yTop = phdr->y;
        }

        if (pFBInfo->dirtyRect.xRight < xRight)
        {
            pFBInfo->dirtyRect.xRight = xRight;
        }

        if (pFBInfo->dirtyRect.yBottom < yBottom)
        {
            pFBInfo->dirtyRect.yBottom = yBottom;
        }
    }

    if (pFBInfo->fDefaultFormat)
    {
        //@todo pfnUpdateDisplayRect must take the vram offset parameter for the framebuffer
        prgn->pPort->pfnUpdateDisplayRect (prgn->pPort, phdr->x, phdr->y, phdr->w, phdr->h);
        prgn->pDisplay->handleDisplayUpdateLegacy (phdr->x + pFBInfo->xOrigin,
                                             phdr->y + pFBInfo->yOrigin, phdr->w, phdr->h);
    }

    return;
}

static void vbvaRgnUpdateFramebuffer (VBVADIRTYREGION *prgn, unsigned uScreenId)
{
    DISPLAYFBINFO *pFBInfo = &prgn->paFramebuffers[uScreenId];

    uint32_t w = pFBInfo->dirtyRect.xRight - pFBInfo->dirtyRect.xLeft;
    uint32_t h = pFBInfo->dirtyRect.yBottom - pFBInfo->dirtyRect.yTop;

    if (!pFBInfo->fDefaultFormat && pFBInfo->pFramebuffer && w != 0 && h != 0)
    {
        //@todo pfnUpdateDisplayRect must take the vram offset parameter for the framebuffer
        prgn->pPort->pfnUpdateDisplayRect (prgn->pPort, pFBInfo->dirtyRect.xLeft, pFBInfo->dirtyRect.yTop, w, h);
        prgn->pDisplay->handleDisplayUpdateLegacy (pFBInfo->dirtyRect.xLeft + pFBInfo->xOrigin,
                                             pFBInfo->dirtyRect.yTop + pFBInfo->yOrigin, w, h);
    }
}

static void vbvaSetMemoryFlags (VBVAMEMORY *pVbvaMemory,
                                bool fVideoAccelEnabled,
                                bool fVideoAccelVRDP,
                                uint32_t fu32SupportedOrders,
                                DISPLAYFBINFO *paFBInfos,
                                unsigned cFBInfos)
{
    if (pVbvaMemory)
    {
        /* This called only on changes in mode. So reset VRDP always. */
        uint32_t fu32Flags = VBVA_F_MODE_VRDP_RESET;

        if (fVideoAccelEnabled)
        {
            fu32Flags |= VBVA_F_MODE_ENABLED;

            if (fVideoAccelVRDP)
            {
                fu32Flags |= VBVA_F_MODE_VRDP | VBVA_F_MODE_VRDP_ORDER_MASK;

                pVbvaMemory->fu32SupportedOrders = fu32SupportedOrders;
            }
        }

        pVbvaMemory->fu32ModeFlags = fu32Flags;
    }

    unsigned uScreenId;
    for (uScreenId = 0; uScreenId < cFBInfos; uScreenId++)
    {
        if (paFBInfos[uScreenId].pHostEvents)
        {
            paFBInfos[uScreenId].pHostEvents->fu32Events |= VBOX_VIDEO_INFO_HOST_EVENTS_F_VRDP_RESET;
        }
    }
}

#ifdef VBOX_WITH_HGSMI
static void vbvaSetMemoryFlagsHGSMI (unsigned uScreenId,
                                     uint32_t fu32SupportedOrders,
                                     bool fVideoAccelVRDP,
                                     DISPLAYFBINFO *pFBInfo)
{
    LogRelFlowFunc(("HGSMI[%d]: %p\n", uScreenId, pFBInfo->pVBVAHostFlags));

    if (pFBInfo->pVBVAHostFlags)
    {
        uint32_t fu32HostEvents = VBOX_VIDEO_INFO_HOST_EVENTS_F_VRDP_RESET;

        if (pFBInfo->fVBVAEnabled)
        {
            fu32HostEvents |= VBVA_F_MODE_ENABLED;

            if (fVideoAccelVRDP)
            {
                fu32HostEvents |= VBVA_F_MODE_VRDP;
            }
        }

        ASMAtomicWriteU32(&pFBInfo->pVBVAHostFlags->u32HostEvents, fu32HostEvents);
        ASMAtomicWriteU32(&pFBInfo->pVBVAHostFlags->u32SupportedOrders, fu32SupportedOrders);

        LogRelFlowFunc(("    fu32HostEvents = 0x%08X, fu32SupportedOrders = 0x%08X\n", fu32HostEvents, fu32SupportedOrders));
    }
}

static void vbvaSetMemoryFlagsAllHGSMI (uint32_t fu32SupportedOrders,
                                        bool fVideoAccelVRDP,
                                        DISPLAYFBINFO *paFBInfos,
                                        unsigned cFBInfos)
{
    unsigned uScreenId;

    for (uScreenId = 0; uScreenId < cFBInfos; uScreenId++)
    {
        vbvaSetMemoryFlagsHGSMI(uScreenId, fu32SupportedOrders, fVideoAccelVRDP, &paFBInfos[uScreenId]);
    }
}
#endif /* VBOX_WITH_HGSMI */

bool Display::VideoAccelAllowed (void)
{
    return true;
}

int Display::vbvaLock(void)
{
    return RTCritSectEnter(&mVBVALock);
}

void Display::vbvaUnlock(void)
{
    RTCritSectLeave(&mVBVALock);
}

int Display::SaveSeamlessRectLock(void)
{
    return RTCritSectEnter(&mSaveSeamlessRectLock);
}

void Display::SaveSeamlessRectUnLock(void)
{
    RTCritSectLeave(&mSaveSeamlessRectLock);
}


/**
 * @thread EMT
 */
int Display::VideoAccelEnable (bool fEnable, VBVAMEMORY *pVbvaMemory)
{
    int rc;
    vbvaLock();
    rc = videoAccelEnable (fEnable, pVbvaMemory);
    vbvaUnlock();
    return rc;
}

int Display::videoAccelEnable (bool fEnable, VBVAMEMORY *pVbvaMemory)
{
    int rc = VINF_SUCCESS;

    /* Called each time the guest wants to use acceleration,
     * or when the VGA device disables acceleration,
     * or when restoring the saved state with accel enabled.
     *
     * VGA device disables acceleration on each video mode change
     * and on reset.
     *
     * Guest enabled acceleration at will. And it has to enable
     * acceleration after a mode change.
     */
    LogRelFlowFunc(("mfVideoAccelEnabled = %d, fEnable = %d, pVbvaMemory = %p\n",
                  mfVideoAccelEnabled, fEnable, pVbvaMemory));

    /* Strictly check parameters. Callers must not pass anything in the case. */
    Assert((fEnable && pVbvaMemory) || (!fEnable && pVbvaMemory == NULL));

    if (!VideoAccelAllowed ())
        return VERR_NOT_SUPPORTED;

    /*
     * Verify that the VM is in running state. If it is not,
     * then this must be postponed until it goes to running.
     */
    if (!mfMachineRunning)
    {
        Assert (!mfVideoAccelEnabled);

        LogRelFlowFunc(("Machine is not yet running.\n"));

        if (fEnable)
        {
            mfPendingVideoAccelEnable = fEnable;
            mpPendingVbvaMemory = pVbvaMemory;
        }

        return rc;
    }

    /* Check that current status is not being changed */
    if (mfVideoAccelEnabled == fEnable)
        return rc;

    if (mfVideoAccelEnabled)
    {
        /* Process any pending orders and empty the VBVA ring buffer. */
        videoAccelFlush ();
    }

    if (!fEnable && mpVbvaMemory)
        mpVbvaMemory->fu32ModeFlags &= ~VBVA_F_MODE_ENABLED;

    /* Safety precaution. There is no more VBVA until everything is setup! */
    mpVbvaMemory = NULL;
    mfVideoAccelEnabled = false;

    /* Update entire display. */
    if (maFramebuffers[VBOX_VIDEO_PRIMARY_SCREEN].u32ResizeStatus == ResizeStatus_Void)
        mpDrv->pUpPort->pfnUpdateDisplayAll(mpDrv->pUpPort);

    /* Everything OK. VBVA status can be changed. */

    /* Notify the VMMDev, which saves VBVA status in the saved state,
     * and needs to know current status.
     */
    VMMDev *pVMMDev = mParent->getVMMDev();
    if (pVMMDev)
    {
        PPDMIVMMDEVPORT pVMMDevPort = pVMMDev->getVMMDevPort();
        if (pVMMDevPort)
            pVMMDevPort->pfnVBVAChange(pVMMDevPort, fEnable);
    }

    if (fEnable)
    {
        mpVbvaMemory = pVbvaMemory;
        mfVideoAccelEnabled = true;

        /* Initialize the hardware memory. */
        vbvaSetMemoryFlags(mpVbvaMemory, mfVideoAccelEnabled, mfVideoAccelVRDP, mfu32SupportedOrders, maFramebuffers, mcMonitors);
        mpVbvaMemory->off32Data = 0;
        mpVbvaMemory->off32Free = 0;

        memset(mpVbvaMemory->aRecords, 0, sizeof (mpVbvaMemory->aRecords));
        mpVbvaMemory->indexRecordFirst = 0;
        mpVbvaMemory->indexRecordFree = 0;

        mfu32PendingVideoAccelDisable = false;

        LogRel(("VBVA: Enabled.\n"));
    }
    else
    {
        LogRel(("VBVA: Disabled.\n"));
    }

    LogRelFlowFunc(("VideoAccelEnable: rc = %Rrc.\n", rc));

    return rc;
}

/* Called always by one VRDP server thread. Can be thread-unsafe.
 */
void Display::VideoAccelVRDP (bool fEnable)
{
    LogRelFlowFunc(("fEnable = %d\n", fEnable));

    vbvaLock();

    int c = fEnable?
                ASMAtomicIncS32 (&mcVideoAccelVRDPRefs):
                ASMAtomicDecS32 (&mcVideoAccelVRDPRefs);

    Assert (c >= 0);

    if (c == 0)
    {
        /* The last client has disconnected, and the accel can be
         * disabled.
         */
        Assert (fEnable == false);

        mfVideoAccelVRDP = false;
        mfu32SupportedOrders = 0;

        vbvaSetMemoryFlags (mpVbvaMemory, mfVideoAccelEnabled, mfVideoAccelVRDP, mfu32SupportedOrders, maFramebuffers, mcMonitors);
#ifdef VBOX_WITH_HGSMI
        /* Here is VRDP-IN thread. Process the request in vbvaUpdateBegin under DevVGA lock on an EMT. */
        ASMAtomicIncU32(&mu32UpdateVBVAFlags);
#endif /* VBOX_WITH_HGSMI */

        LogRel(("VBVA: VRDP acceleration has been disabled.\n"));
    }
    else if (   c == 1
             && !mfVideoAccelVRDP)
    {
        /* The first client has connected. Enable the accel.
         */
        Assert (fEnable == true);

        mfVideoAccelVRDP = true;
        /* Supporting all orders. */
        mfu32SupportedOrders = ~0;

        vbvaSetMemoryFlags (mpVbvaMemory, mfVideoAccelEnabled, mfVideoAccelVRDP, mfu32SupportedOrders, maFramebuffers, mcMonitors);
#ifdef VBOX_WITH_HGSMI
        /* Here is VRDP-IN thread. Process the request in vbvaUpdateBegin under DevVGA lock on an EMT. */
        ASMAtomicIncU32(&mu32UpdateVBVAFlags);
#endif /* VBOX_WITH_HGSMI */

        LogRel(("VBVA: VRDP acceleration has been requested.\n"));
    }
    else
    {
        /* A client is connected or disconnected but there is no change in the
         * accel state. It remains enabled.
         */
        Assert (mfVideoAccelVRDP == true);
    }
    vbvaUnlock();
}

static bool vbvaVerifyRingBuffer (VBVAMEMORY *pVbvaMemory)
{
    return true;
}

static void vbvaFetchBytes (VBVAMEMORY *pVbvaMemory, uint8_t *pu8Dst, uint32_t cbDst)
{
    if (cbDst >= VBVA_RING_BUFFER_SIZE)
    {
        AssertMsgFailed (("cbDst = 0x%08X, ring buffer size 0x%08X\n", cbDst, VBVA_RING_BUFFER_SIZE));
        return;
    }

    uint32_t u32BytesTillBoundary = VBVA_RING_BUFFER_SIZE - pVbvaMemory->off32Data;
    uint8_t  *src                 = &pVbvaMemory->au8RingBuffer[pVbvaMemory->off32Data];
    int32_t i32Diff               = cbDst - u32BytesTillBoundary;

    if (i32Diff <= 0)
    {
        /* Chunk will not cross buffer boundary. */
        memcpy (pu8Dst, src, cbDst);
    }
    else
    {
        /* Chunk crosses buffer boundary. */
        memcpy (pu8Dst, src, u32BytesTillBoundary);
        memcpy (pu8Dst + u32BytesTillBoundary, &pVbvaMemory->au8RingBuffer[0], i32Diff);
    }

    /* Advance data offset. */
    pVbvaMemory->off32Data = (pVbvaMemory->off32Data + cbDst) % VBVA_RING_BUFFER_SIZE;

    return;
}


static bool vbvaPartialRead (uint8_t **ppu8, uint32_t *pcb, uint32_t cbRecord, VBVAMEMORY *pVbvaMemory)
{
    uint8_t *pu8New;

    LogFlow(("MAIN::DisplayImpl::vbvaPartialRead: p = %p, cb = %d, cbRecord 0x%08X\n",
             *ppu8, *pcb, cbRecord));

    if (*ppu8)
    {
        Assert (*pcb);
        pu8New = (uint8_t *)RTMemRealloc (*ppu8, cbRecord);
    }
    else
    {
        Assert (!*pcb);
        pu8New = (uint8_t *)RTMemAlloc (cbRecord);
    }

    if (!pu8New)
    {
        /* Memory allocation failed, fail the function. */
        Log(("MAIN::vbvaPartialRead: failed to (re)alocate memory for partial record!!! cbRecord 0x%08X\n",
             cbRecord));

        if (*ppu8)
        {
            RTMemFree (*ppu8);
        }

        *ppu8 = NULL;
        *pcb = 0;

        return false;
    }

    /* Fetch data from the ring buffer. */
    vbvaFetchBytes (pVbvaMemory, pu8New + *pcb, cbRecord - *pcb);

    *ppu8 = pu8New;
    *pcb = cbRecord;

    return true;
}

/* For contiguous chunks just return the address in the buffer.
 * For crossing boundary - allocate a buffer from heap.
 */
bool Display::vbvaFetchCmd (VBVACMDHDR **ppHdr, uint32_t *pcbCmd)
{
    uint32_t indexRecordFirst = mpVbvaMemory->indexRecordFirst;
    uint32_t indexRecordFree = mpVbvaMemory->indexRecordFree;

#ifdef DEBUG_sunlover
    LogFlowFunc(("first = %d, free = %d\n",
                 indexRecordFirst, indexRecordFree));
#endif /* DEBUG_sunlover */

    if (!vbvaVerifyRingBuffer (mpVbvaMemory))
    {
        return false;
    }

    if (indexRecordFirst == indexRecordFree)
    {
        /* No records to process. Return without assigning output variables. */
        return true;
    }

    VBVARECORD *pRecord = &mpVbvaMemory->aRecords[indexRecordFirst];

#ifdef DEBUG_sunlover
    LogFlowFunc(("cbRecord = 0x%08X\n", pRecord->cbRecord));
#endif /* DEBUG_sunlover */

    uint32_t cbRecord = pRecord->cbRecord & ~VBVA_F_RECORD_PARTIAL;

    if (mcbVbvaPartial)
    {
        /* There is a partial read in process. Continue with it. */

        Assert (mpu8VbvaPartial);

        LogFlowFunc(("continue partial record mcbVbvaPartial = %d cbRecord 0x%08X, first = %d, free = %d\n",
                      mcbVbvaPartial, pRecord->cbRecord, indexRecordFirst, indexRecordFree));

        if (cbRecord > mcbVbvaPartial)
        {
            /* New data has been added to the record. */
            if (!vbvaPartialRead (&mpu8VbvaPartial, &mcbVbvaPartial, cbRecord, mpVbvaMemory))
            {
                return false;
            }
        }

        if (!(pRecord->cbRecord & VBVA_F_RECORD_PARTIAL))
        {
            /* The record is completed by guest. Return it to the caller. */
            *ppHdr = (VBVACMDHDR *)mpu8VbvaPartial;
            *pcbCmd = mcbVbvaPartial;

            mpu8VbvaPartial = NULL;
            mcbVbvaPartial = 0;

            /* Advance the record index. */
            mpVbvaMemory->indexRecordFirst = (indexRecordFirst + 1) % VBVA_MAX_RECORDS;

#ifdef DEBUG_sunlover
            LogFlowFunc(("partial done ok, data = %d, free = %d\n",
                          mpVbvaMemory->off32Data, mpVbvaMemory->off32Free));
#endif /* DEBUG_sunlover */
        }

        return true;
    }

    /* A new record need to be processed. */
    if (pRecord->cbRecord & VBVA_F_RECORD_PARTIAL)
    {
        /* Current record is being written by guest. '=' is important here. */
        if (cbRecord >= VBVA_RING_BUFFER_SIZE - VBVA_RING_BUFFER_THRESHOLD)
        {
            /* Partial read must be started. */
            if (!vbvaPartialRead (&mpu8VbvaPartial, &mcbVbvaPartial, cbRecord, mpVbvaMemory))
            {
                return false;
            }

            LogFlowFunc(("started partial record mcbVbvaPartial = 0x%08X cbRecord 0x%08X, first = %d, free = %d\n",
                          mcbVbvaPartial, pRecord->cbRecord, indexRecordFirst, indexRecordFree));
        }

        return true;
    }

    /* Current record is complete. If it is not empty, process it. */
    if (cbRecord)
    {
        /* The size of largest contiguous chunk in the ring biffer. */
        uint32_t u32BytesTillBoundary = VBVA_RING_BUFFER_SIZE - mpVbvaMemory->off32Data;

        /* The ring buffer pointer. */
        uint8_t *au8RingBuffer = &mpVbvaMemory->au8RingBuffer[0];

        /* The pointer to data in the ring buffer. */
        uint8_t *src = &au8RingBuffer[mpVbvaMemory->off32Data];

        /* Fetch or point the data. */
        if (u32BytesTillBoundary >= cbRecord)
        {
            /* The command does not cross buffer boundary. Return address in the buffer. */
            *ppHdr = (VBVACMDHDR *)src;

            /* Advance data offset. */
            mpVbvaMemory->off32Data = (mpVbvaMemory->off32Data + cbRecord) % VBVA_RING_BUFFER_SIZE;
        }
        else
        {
            /* The command crosses buffer boundary. Rare case, so not optimized. */
            uint8_t *dst = (uint8_t *)RTMemAlloc (cbRecord);

            if (!dst)
            {
                LogRelFlowFunc(("could not allocate %d bytes from heap!!!\n", cbRecord));
                mpVbvaMemory->off32Data = (mpVbvaMemory->off32Data + cbRecord) % VBVA_RING_BUFFER_SIZE;
                return false;
            }

            vbvaFetchBytes (mpVbvaMemory, dst, cbRecord);

            *ppHdr = (VBVACMDHDR *)dst;

#ifdef DEBUG_sunlover
            LogFlowFunc(("Allocated from heap %p\n", dst));
#endif /* DEBUG_sunlover */
        }
    }

    *pcbCmd = cbRecord;

    /* Advance the record index. */
    mpVbvaMemory->indexRecordFirst = (indexRecordFirst + 1) % VBVA_MAX_RECORDS;

#ifdef DEBUG_sunlover
    LogFlowFunc(("done ok, data = %d, free = %d\n",
                 mpVbvaMemory->off32Data, mpVbvaMemory->off32Free));
#endif /* DEBUG_sunlover */

    return true;
}

void Display::vbvaReleaseCmd (VBVACMDHDR *pHdr, int32_t cbCmd)
{
    uint8_t *au8RingBuffer = mpVbvaMemory->au8RingBuffer;

    if (   (uint8_t *)pHdr >= au8RingBuffer
        && (uint8_t *)pHdr < &au8RingBuffer[VBVA_RING_BUFFER_SIZE])
    {
        /* The pointer is inside ring buffer. Must be continuous chunk. */
        Assert (VBVA_RING_BUFFER_SIZE - ((uint8_t *)pHdr - au8RingBuffer) >= cbCmd);

        /* Do nothing. */

        Assert (!mpu8VbvaPartial && mcbVbvaPartial == 0);
    }
    else
    {
        /* The pointer is outside. It is then an allocated copy. */

#ifdef DEBUG_sunlover
        LogFlowFunc(("Free heap %p\n", pHdr));
#endif /* DEBUG_sunlover */

        if ((uint8_t *)pHdr == mpu8VbvaPartial)
        {
            mpu8VbvaPartial = NULL;
            mcbVbvaPartial = 0;
        }
        else
        {
            Assert (!mpu8VbvaPartial && mcbVbvaPartial == 0);
        }

        RTMemFree (pHdr);
    }

    return;
}


/**
 * Called regularly on the DisplayRefresh timer.
 * Also on behalf of guest, when the ring buffer is full.
 *
 * @thread EMT
 */
void Display::VideoAccelFlush (void)
{
    vbvaLock();
    videoAccelFlush();
    vbvaUnlock();
}

/* Under VBVA lock. DevVGA is not taken. */
void Display::videoAccelFlush (void)
{
#ifdef DEBUG_sunlover_2
    LogFlowFunc(("mfVideoAccelEnabled = %d\n", mfVideoAccelEnabled));
#endif /* DEBUG_sunlover_2 */

    if (!mfVideoAccelEnabled)
    {
        Log(("Display::VideoAccelFlush: called with disabled VBVA!!! Ignoring.\n"));
        return;
    }

    /* Here VBVA is enabled and we have the accelerator memory pointer. */
    Assert(mpVbvaMemory);

#ifdef DEBUG_sunlover_2
    LogFlowFunc(("indexRecordFirst = %d, indexRecordFree = %d, off32Data = %d, off32Free = %d\n",
                  mpVbvaMemory->indexRecordFirst, mpVbvaMemory->indexRecordFree, mpVbvaMemory->off32Data, mpVbvaMemory->off32Free));
#endif /* DEBUG_sunlover_2 */

    /* Quick check for "nothing to update" case. */
    if (mpVbvaMemory->indexRecordFirst == mpVbvaMemory->indexRecordFree)
    {
        return;
    }

    /* Process the ring buffer */
    unsigned uScreenId;

    /* Initialize dirty rectangles accumulator. */
    VBVADIRTYREGION rgn;
    vbvaRgnInit (&rgn, maFramebuffers, mcMonitors, this, mpDrv->pUpPort);

    for (;;)
    {
        VBVACMDHDR *phdr = NULL;
        uint32_t cbCmd = ~0;

        /* Fetch the command data. */
        if (!vbvaFetchCmd (&phdr, &cbCmd))
        {
            Log(("Display::VideoAccelFlush: unable to fetch command. off32Data = %d, off32Free = %d. Disabling VBVA!!!\n",
                  mpVbvaMemory->off32Data, mpVbvaMemory->off32Free));

            /* Disable VBVA on those processing errors. */
            videoAccelEnable (false, NULL);

            break;
        }

        if (cbCmd == uint32_t(~0))
        {
            /* No more commands yet in the queue. */
            break;
        }

        if (cbCmd != 0)
        {
#ifdef DEBUG_sunlover
            LogFlowFunc(("hdr: cbCmd = %d, x=%d, y=%d, w=%d, h=%d\n",
                         cbCmd, phdr->x, phdr->y, phdr->w, phdr->h));
#endif /* DEBUG_sunlover */

            VBVACMDHDR hdrSaved = *phdr;

            int x = phdr->x;
            int y = phdr->y;
            int w = phdr->w;
            int h = phdr->h;

            uScreenId = mapCoordsToScreen(maFramebuffers, mcMonitors, &x, &y, &w, &h);

            phdr->x = (int16_t)x;
            phdr->y = (int16_t)y;
            phdr->w = (uint16_t)w;
            phdr->h = (uint16_t)h;

            DISPLAYFBINFO *pFBInfo = &maFramebuffers[uScreenId];

            if (pFBInfo->u32ResizeStatus == ResizeStatus_Void)
            {
                /* Handle the command.
                 *
                 * Guest is responsible for updating the guest video memory.
                 * The Windows guest does all drawing using Eng*.
                 *
                 * For local output, only dirty rectangle information is used
                 * to update changed areas.
                 *
                 * Dirty rectangles are accumulated to exclude overlapping updates and
                 * group small updates to a larger one.
                 */

                /* Accumulate the update. */
                vbvaRgnDirtyRect (&rgn, uScreenId, phdr);

                /* Forward the command to VRDP server. */
                mParent->consoleVRDPServer()->SendUpdate (uScreenId, phdr, cbCmd);

                *phdr = hdrSaved;
            }
        }

        vbvaReleaseCmd (phdr, cbCmd);
    }

    for (uScreenId = 0; uScreenId < mcMonitors; uScreenId++)
    {
        if (maFramebuffers[uScreenId].u32ResizeStatus == ResizeStatus_Void)
        {
            /* Draw the framebuffer. */
            vbvaRgnUpdateFramebuffer (&rgn, uScreenId);
        }
    }
}

int Display::videoAccelRefreshProcess(void)
{
    int rc = VWRN_INVALID_STATE; /* Default is to do a display update in VGA device. */

    vbvaLock();

    if (ASMAtomicCmpXchgU32(&mfu32PendingVideoAccelDisable, false, true))
    {
        videoAccelEnable (false, NULL);
    }
    else if (mfPendingVideoAccelEnable)
    {
        /* Acceleration was enabled while machine was not yet running
         * due to restoring from saved state. Update entire display and
         * actually enable acceleration.
         */
        Assert(mpPendingVbvaMemory);

        /* Acceleration can not be yet enabled.*/
        Assert(mpVbvaMemory == NULL);
        Assert(!mfVideoAccelEnabled);

        if (mfMachineRunning)
        {
            videoAccelEnable (mfPendingVideoAccelEnable,
                              mpPendingVbvaMemory);

            /* Reset the pending state. */
            mfPendingVideoAccelEnable = false;
            mpPendingVbvaMemory = NULL;
        }

        rc = VINF_TRY_AGAIN;
    }
    else
    {
        Assert(mpPendingVbvaMemory == NULL);

        if (mfVideoAccelEnabled)
        {
            Assert(mpVbvaMemory);
            videoAccelFlush ();

            rc = VINF_SUCCESS; /* VBVA processed, no need to a display update. */
        }
    }

    vbvaUnlock();

    return rc;
}


// IDisplay methods
/////////////////////////////////////////////////////////////////////////////
STDMETHODIMP Display::GetScreenResolution (ULONG aScreenId,
    ULONG *aWidth, ULONG *aHeight, ULONG *aBitsPerPixel,
    LONG *aXOrigin, LONG *aYOrigin)
{
    LogRelFlowFunc(("aScreenId = %d\n", aScreenId));

    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    uint32_t u32Width = 0;
    uint32_t u32Height = 0;
    uint32_t u32BitsPerPixel = 0;
    int32_t xOrigin = 0;
    int32_t yOrigin = 0;

    if (aScreenId == VBOX_VIDEO_PRIMARY_SCREEN)
    {
        CHECK_CONSOLE_DRV(mpDrv);

        u32Width = mpDrv->IConnector.cx;
        u32Height = mpDrv->IConnector.cy;
        int rc = mpDrv->pUpPort->pfnQueryColorDepth(mpDrv->pUpPort, &u32BitsPerPixel);
        AssertRC(rc);
    }
    else if (aScreenId < mcMonitors)
    {
        DISPLAYFBINFO *pFBInfo = &maFramebuffers[aScreenId];
        u32Width = pFBInfo->w;
        u32Height = pFBInfo->h;
        u32BitsPerPixel = pFBInfo->u16BitsPerPixel;
        xOrigin = pFBInfo->xOrigin;
        yOrigin = pFBInfo->yOrigin;
    }
    else
    {
        return E_INVALIDARG;
    }

    if (aWidth)
        *aWidth = u32Width;
    if (aHeight)
        *aHeight = u32Height;
    if (aBitsPerPixel)
        *aBitsPerPixel = u32BitsPerPixel;
    if (aXOrigin)
        *aXOrigin = xOrigin;
    if (aYOrigin)
        *aYOrigin = yOrigin;

    return S_OK;
}

STDMETHODIMP Display::SetFramebuffer(ULONG aScreenId, IFramebuffer *aFramebuffer)
{
    LogRelFlowFunc(("\n"));

    if (aFramebuffer != NULL)
        CheckComArgOutPointerValid(aFramebuffer);

    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc()))
        return autoCaller.rc();

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    Console::SafeVMPtrQuiet ptrVM(mParent);
    if (ptrVM.isOk())
    {
        /* Must release the lock here because the changeFramebuffer will
         * also obtain it. */
        alock.release();

        /* send request to the EMT thread */
        int vrc = VMR3ReqCallWaitU(ptrVM.rawUVM(), VMCPUID_ANY,
                                   (PFNRT)changeFramebuffer, 3, this, aFramebuffer, aScreenId);

        alock.acquire();

        ComAssertRCRet (vrc, E_FAIL);

#if defined(VBOX_WITH_HGCM) && defined(VBOX_WITH_CROGL)
        {
            if (mfIsCr3DEnabled)
            {
                VBOXCRCMDCTL_HGCM data;
                data.Hdr.enmType = VBOXCRCMDCTL_TYPE_HGCM;
                data.Hdr.u32Function = SHCRGL_HOST_FN_SCREEN_CHANGED;

                data.aParms[0].type = VBOX_HGCM_SVC_PARM_32BIT;
                data.aParms[0].u.uint32 = aScreenId;

                alock.release();

                crCtlSubmitSync(&data.Hdr, sizeof (data));

                alock.acquire();
            }
        }
#endif /* #if defined(VBOX_WITH_HGCM) && defined(VBOX_WITH_CROGL) */
    }
    else
    {
        /* No VM is created (VM is powered off), do a direct call */
        int vrc = changeFramebuffer (this, aFramebuffer, aScreenId);
        ComAssertRCRet (vrc, E_FAIL);
    }

    return S_OK;
}

STDMETHODIMP Display::GetFramebuffer(ULONG aScreenId,
                                     IFramebuffer **aFramebuffer, LONG *aXOrigin, LONG *aYOrigin)
{
    LogRelFlowFunc(("aScreenId = %d\n", aScreenId));

    CheckComArgOutPointerValid(aFramebuffer);

    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    if (aScreenId != 0 && aScreenId >= mcMonitors)
        return E_INVALIDARG;

    /* @todo this should be actually done on EMT. */
    DISPLAYFBINFO *pFBInfo = &maFramebuffers[aScreenId];

    *aFramebuffer = pFBInfo->pFramebuffer;
    if (*aFramebuffer)
        (*aFramebuffer)->AddRef ();
    if (aXOrigin)
        *aXOrigin = pFBInfo->xOrigin;
    if (aYOrigin)
        *aYOrigin = pFBInfo->yOrigin;

    return S_OK;
}

STDMETHODIMP Display::SetVideoModeHint(ULONG aDisplay, BOOL aEnabled,
                                       BOOL aChangeOrigin, LONG aOriginX, LONG aOriginY,
                                       ULONG aWidth, ULONG aHeight, ULONG aBitsPerPixel)
{
    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    CHECK_CONSOLE_DRV(mpDrv);

    /*
     * Do some rough checks for valid input
     */
    ULONG width  = aWidth;
    if (!width)
        width    = mpDrv->IConnector.cx;
    ULONG height = aHeight;
    if (!height)
        height   = mpDrv->IConnector.cy;
    ULONG bpp    = aBitsPerPixel;
    if (!bpp)
    {
        uint32_t cBits = 0;
        int rc = mpDrv->pUpPort->pfnQueryColorDepth(mpDrv->pUpPort, &cBits);
        AssertRC(rc);
        bpp = cBits;
    }
    ULONG cMonitors;
    mParent->machine()->COMGETTER(MonitorCount)(&cMonitors);
    if (cMonitors == 0 && aDisplay > 0)
        return E_INVALIDARG;
    if (aDisplay >= cMonitors)
        return E_INVALIDARG;

   /*
    * sunlover 20070614: It is up to the guest to decide whether the hint is
    * valid. Therefore don't do any VRAM sanity checks here!
    */

    /* Have to release the lock because the pfnRequestDisplayChange
     * will call EMT.  */
    alock.release();

    /* We always send the hint to the graphics card in case the guest enables
     * support later.  For now we notify exactly when support is enabled. */
    mpDrv->pUpPort->pfnSendModeHint(mpDrv->pUpPort, aWidth, aHeight,
                                    aBitsPerPixel, aDisplay,
                                    aChangeOrigin ? aOriginX : ~0,
                                    aChangeOrigin ? aOriginY : ~0,
                                    RT_BOOL(aEnabled),
                                      mfGuestVBVACapabilities
                                    & VBVACAPS_VIDEO_MODE_HINTS);
    if (   mfGuestVBVACapabilities & VBVACAPS_VIDEO_MODE_HINTS
        && !(mfGuestVBVACapabilities & VBVACAPS_IRQ))
    {
        HRESULT hrc = mParent->i_sendACPIMonitorHotPlugEvent();
        if (FAILED(hrc))
            return hrc;
    }

    /* We currently never suppress the VMMDev hint if the guest has requested
     * it.  Specifically the video graphics driver may not be responsible for
     * screen positioning in the guest virtual desktop, and the component
     * responsible may want to get the hint from VMMDev. */
    VMMDev *pVMMDev = mParent->getVMMDev();
    if (pVMMDev)
    {
        PPDMIVMMDEVPORT pVMMDevPort = pVMMDev->getVMMDevPort();
        if (pVMMDevPort)
            pVMMDevPort->pfnRequestDisplayChange(pVMMDevPort, aWidth, aHeight, aBitsPerPixel,
                                                 aDisplay, aOriginX, aOriginY,
                                                 RT_BOOL(aEnabled), RT_BOOL(aChangeOrigin));
    }
    return S_OK;
}

STDMETHODIMP Display::SetSeamlessMode (BOOL enabled)
{
    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    /* Have to release the lock because the pfnRequestSeamlessChange will call EMT.  */
    alock.release();

    VMMDev *pVMMDev = mParent->getVMMDev();
    if (pVMMDev)
    {
        PPDMIVMMDEVPORT pVMMDevPort = pVMMDev->getVMMDevPort();
        if (pVMMDevPort)
            pVMMDevPort->pfnRequestSeamlessChange(pVMMDevPort, !!enabled);
    }

#if defined(VBOX_WITH_HGCM) && defined(VBOX_WITH_CROGL)
    if (!enabled)
    {
        VMMDev *vmmDev = mParent->getVMMDev();
        if (mfIsCr3DEnabled && vmmDev)
        {
            VBOXCRCMDCTL_HGCM *pData = (VBOXCRCMDCTL_HGCM*)RTMemAlloc(sizeof (VBOXCRCMDCTL_HGCM));
            if (!pData)
            {
                AssertMsgFailed(("RTMemAlloc failed\n"));
                return VERR_NO_MEMORY;
            }

            pData->Hdr.enmType = VBOXCRCMDCTL_TYPE_HGCM;
            pData->Hdr.u32Function = SHCRGL_HOST_FN_SET_VISIBLE_REGION;

            pData->aParms[0].type = VBOX_HGCM_SVC_PARM_PTR;
            pData->aParms[0].u.pointer.addr = NULL;
            pData->aParms[0].u.pointer.size = 0; /* <- means null rects, NULL pRects address and 0 rects means "disable" */

            int rc = crCtlSubmit(&pData->Hdr, sizeof (*pData), displayCrCmdFree, pData);
            if (!RT_SUCCESS(rc))
            {
                AssertMsgFailed(("crCtlSubmit failed rc %d\n", rc));
                RTMemFree(pData);
            }
        }
    }
#endif
    return S_OK;
}

#if defined(VBOX_WITH_HGCM) && defined(VBOX_WITH_CROGL)
BOOL Display::displayCheckTakeScreenshotCrOgl(Display *pDisplay, ULONG aScreenId, uint8_t *pu8Data,
                                              uint32_t u32Width, uint32_t u32Height)
{
    if (   pDisplay->mfIsCr3DEnabled
        && pDisplay->mCrOglCallbacks.pfnHasData
        && pDisplay->mCrOglCallbacks.pfnHasData())
    {
        VMMDev *pVMMDev = pDisplay->mParent->getVMMDev();
        if (pVMMDev)
        {
            CRVBOXHGCMTAKESCREENSHOT *pScreenshot = (CRVBOXHGCMTAKESCREENSHOT*)RTMemAlloc(sizeof (*pScreenshot));
            if (pScreenshot)
            {
                /* screen id or CRSCREEN_ALL to specify all enabled */
                pScreenshot->u32Screen = aScreenId;
                pScreenshot->u32Width = u32Width;
                pScreenshot->u32Height = u32Height;
                pScreenshot->u32Pitch = u32Width * 4;
                pScreenshot->pvBuffer = pu8Data;
                pScreenshot->pvContext = NULL;
                pScreenshot->pfnScreenshotBegin = NULL;
                pScreenshot->pfnScreenshotPerform = NULL;
                pScreenshot->pfnScreenshotEnd = NULL;

                VBOXCRCMDCTL_HGCM data;
                data.Hdr.enmType = VBOXCRCMDCTL_TYPE_HGCM;
                data.Hdr.u32Function = SHCRGL_HOST_FN_TAKE_SCREENSHOT;

                data.aParms[0].type = VBOX_HGCM_SVC_PARM_PTR;
                data.aParms[0].u.pointer.addr = pScreenshot;
                data.aParms[0].u.pointer.size = sizeof (*pScreenshot);

                int rc = pDisplay->crCtlSubmitSync(&data.Hdr, sizeof (data));

                RTMemFree(pScreenshot);

                if (RT_SUCCESS(rc))
                    return TRUE;
                else
                {
                    AssertMsgFailed(("failed to get screenshot data from crOgl %d\n", rc));
                    /* fall back to the non-3d mechanism */
                }
            }
        }
    }
    return FALSE;
}
#endif

int Display::displayTakeScreenshotEMT(Display *pDisplay, ULONG aScreenId, uint8_t **ppu8Data, size_t *pcbData, uint32_t *pu32Width, uint32_t *pu32Height)
{
    int rc;
    pDisplay->vbvaLock();
    if (   aScreenId == VBOX_VIDEO_PRIMARY_SCREEN
        && pDisplay->maFramebuffers[aScreenId].fVBVAEnabled == false) /* A non-VBVA mode. */
    {
        rc = pDisplay->mpDrv->pUpPort->pfnTakeScreenshot(pDisplay->mpDrv->pUpPort, ppu8Data, pcbData, pu32Width, pu32Height);
    }
    else if (aScreenId < pDisplay->mcMonitors)
    {
        DISPLAYFBINFO *pFBInfo = &pDisplay->maFramebuffers[aScreenId];

        uint32_t width = pFBInfo->w;
        uint32_t height = pFBInfo->h;

        /* Allocate 32 bit per pixel bitmap. */
        size_t cbRequired = width * 4 * height;

        if (cbRequired)
        {
            uint8_t *pu8Data = (uint8_t *)RTMemAlloc(cbRequired);

            if (pu8Data == NULL)
            {
                rc = VERR_NO_MEMORY;
            }
            else
            {
                /* Copy guest VRAM to the allocated 32bpp buffer. */
                const uint8_t *pu8Src       = pFBInfo->pu8FramebufferVRAM;
                int32_t xSrc                = 0;
                int32_t ySrc                = 0;
                uint32_t u32SrcWidth        = width;
                uint32_t u32SrcHeight       = height;
                uint32_t u32SrcLineSize     = pFBInfo->u32LineSize;
                uint32_t u32SrcBitsPerPixel = pFBInfo->u16BitsPerPixel;

                uint8_t *pu8Dst             = pu8Data;
                int32_t xDst                = 0;
                int32_t yDst                = 0;
                uint32_t u32DstWidth        = u32SrcWidth;
                uint32_t u32DstHeight       = u32SrcHeight;
                uint32_t u32DstLineSize     = u32DstWidth * 4;
                uint32_t u32DstBitsPerPixel = 32;

                rc = pDisplay->mpDrv->pUpPort->pfnCopyRect(pDisplay->mpDrv->pUpPort,
                                                           width, height,
                                                           pu8Src,
                                                           xSrc, ySrc,
                                                           u32SrcWidth, u32SrcHeight,
                                                           u32SrcLineSize, u32SrcBitsPerPixel,
                                                           pu8Dst,
                                                           xDst, yDst,
                                                           u32DstWidth, u32DstHeight,
                                                           u32DstLineSize, u32DstBitsPerPixel);
                if (RT_SUCCESS(rc))
                {
                    *ppu8Data = pu8Data;
                    *pcbData = cbRequired;
                    *pu32Width = width;
                    *pu32Height = height;
                }
                else
                {
                    RTMemFree(pu8Data);

                    /* CopyRect can fail if VBVA was paused in VGA device, retry using the generic method. */
                    if (   rc == VERR_INVALID_STATE
                        && aScreenId == VBOX_VIDEO_PRIMARY_SCREEN)
                    {
                        rc = pDisplay->mpDrv->pUpPort->pfnTakeScreenshot(pDisplay->mpDrv->pUpPort,
                                                                         ppu8Data, pcbData, pu32Width, pu32Height);
                    }
                }
            }
        }
        else
        {
            /* No image. */
            *ppu8Data = NULL;
            *pcbData = 0;
            *pu32Width = 0;
            *pu32Height = 0;
            rc = VINF_SUCCESS;
        }
    }
    else
    {
        rc = VERR_INVALID_PARAMETER;
    }
    pDisplay->vbvaUnlock();
    return rc;
}

static int displayTakeScreenshot(PUVM pUVM, Display *pDisplay, struct DRVMAINDISPLAY *pDrv, ULONG aScreenId,
                                 BYTE *address, ULONG width, ULONG height)
{
    uint8_t *pu8Data = NULL;
    size_t cbData = 0;
    uint32_t cx = 0;
    uint32_t cy = 0;
    int vrc = VINF_SUCCESS;

# if defined(VBOX_WITH_HGCM) && defined(VBOX_WITH_CROGL)
    if (Display::displayCheckTakeScreenshotCrOgl(pDisplay, aScreenId, (uint8_t*)address, width, height))
        return VINF_SUCCESS;
#endif

    int cRetries = 5;

    while (cRetries-- > 0)
    {
        /* Note! Not sure if the priority call is such a good idea here, but
                 it would be nice to have an accurate screenshot for the bug
                 report if the VM deadlocks. */
        vrc = VMR3ReqPriorityCallWaitU(pUVM, VMCPUID_ANY, (PFNRT)Display::displayTakeScreenshotEMT, 6,
                                       pDisplay, aScreenId, &pu8Data, &cbData, &cx, &cy);
        if (vrc != VERR_TRY_AGAIN)
        {
            break;
        }

        RTThreadSleep(10);
    }

    if (RT_SUCCESS(vrc) && pu8Data)
    {
        if (cx == width && cy == height)
        {
            /* No scaling required. */
            memcpy(address, pu8Data, cbData);
        }
        else
        {
            /* Scale. */
            LogRelFlowFunc(("SCALE: %dx%d -> %dx%d\n", cx, cy, width, height));

            uint8_t *dst = address;
            uint8_t *src = pu8Data;
            int dstW = width;
            int dstH = height;
            int srcW = cx;
            int srcH = cy;
            int iDeltaLine = cx * 4;

            BitmapScale32(dst,
                          dstW, dstH,
                          src,
                          iDeltaLine,
                          srcW, srcH);
        }

        if (aScreenId == VBOX_VIDEO_PRIMARY_SCREEN)
        {
            /* This can be called from any thread. */
            pDrv->pUpPort->pfnFreeScreenshot(pDrv->pUpPort, pu8Data);
        }
        else
        {
            RTMemFree(pu8Data);
        }
    }

    return vrc;
}

STDMETHODIMP Display::TakeScreenShot(ULONG aScreenId, BYTE *address, ULONG width, ULONG height)
{
    /// @todo (r=dmik) this function may take too long to complete if the VM
    //  is doing something like saving state right now. Which, in case if it
    //  is called on the GUI thread, will make it unresponsive. We should
    //  check the machine state here (by enclosing the check and VMRequCall
    //  within the Console lock to make it atomic).

    LogRelFlowFunc(("address=%p, width=%d, height=%d\n",
                    address, width, height));

    CheckComArgNotNull(address);
    CheckComArgExpr(width, width != 0);
    CheckComArgExpr(height, height != 0);

    /* Do not allow too large screenshots. This also filters out negative
     * values passed as either 'width' or 'height'.
     */
    CheckComArgExpr(width, width <= 32767);
    CheckComArgExpr(height, height <= 32767);

    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    if (!mpDrv)
        return E_FAIL;

    Console::SafeVMPtr ptrVM(mParent);
    if (!ptrVM.isOk())
        return ptrVM.rc();

    HRESULT rc = S_OK;

    LogRelFlowFunc(("Sending SCREENSHOT request\n"));

    /* Release lock because other thread (EMT) is called and it may initiate a resize
     * which also needs lock.
     *
     * This method does not need the lock anymore.
     */
    alock.release();

    int vrc = displayTakeScreenshot(ptrVM.rawUVM(), this, mpDrv, aScreenId, address, width, height);

    if (vrc == VERR_NOT_IMPLEMENTED)
        rc = setError(E_NOTIMPL,
                      tr("This feature is not implemented"));
    else if (vrc == VERR_TRY_AGAIN)
        rc = setError(E_UNEXPECTED,
                      tr("This feature is not available at this time"));
    else if (RT_FAILURE(vrc))
        rc = setError(VBOX_E_IPRT_ERROR,
                      tr("Could not take a screenshot (%Rrc)"), vrc);

    LogRelFlowFunc(("rc=%Rhrc\n", rc));
    return rc;
}

STDMETHODIMP Display::TakeScreenShotToArray(ULONG aScreenId, ULONG width, ULONG height,
                                            ComSafeArrayOut(BYTE, aScreenData))
{
    LogRelFlowFunc(("width=%d, height=%d\n", width, height));

    CheckComArgOutSafeArrayPointerValid(aScreenData);
    CheckComArgExpr(width, width != 0);
    CheckComArgExpr(height, height != 0);

    /* Do not allow too large screenshots. This also filters out negative
     * values passed as either 'width' or 'height'.
     */
    CheckComArgExpr(width, width <= 32767);
    CheckComArgExpr(height, height <= 32767);

    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    if (!mpDrv)
        return E_FAIL;

    Console::SafeVMPtr ptrVM(mParent);
    if (!ptrVM.isOk())
        return ptrVM.rc();

    HRESULT rc = S_OK;

    LogRelFlowFunc(("Sending SCREENSHOT request\n"));

    /* Release lock because other thread (EMT) is called and it may initiate a resize
     * which also needs lock.
     *
     * This method does not need the lock anymore.
     */
    alock.release();

    size_t cbData = width * 4 * height;
    uint8_t *pu8Data = (uint8_t *)RTMemAlloc(cbData);

    if (!pu8Data)
        return E_OUTOFMEMORY;

    int vrc = displayTakeScreenshot(ptrVM.rawUVM(), this, mpDrv, aScreenId, pu8Data, width, height);

    if (RT_SUCCESS(vrc))
    {
        /* Convert pixels to format expected by the API caller: [0] R, [1] G, [2] B, [3] A. */
        uint8_t *pu8 = pu8Data;
        unsigned cPixels = width * height;
        while (cPixels)
        {
            uint8_t u8 = pu8[0];
            pu8[0] = pu8[2];
            pu8[2] = u8;
            pu8[3] = 0xff;
            cPixels--;
            pu8 += 4;
        }

        com::SafeArray<BYTE> screenData(cbData);
        screenData.initFrom(pu8Data, cbData);
        screenData.detachTo(ComSafeArrayOutArg(aScreenData));
    }
    else if (vrc == VERR_NOT_IMPLEMENTED)
        rc = setError(E_NOTIMPL,
                      tr("This feature is not implemented"));
    else
        rc = setError(VBOX_E_IPRT_ERROR,
                      tr("Could not take a screenshot (%Rrc)"), vrc);

    RTMemFree(pu8Data);

    LogRelFlowFunc(("rc=%Rhrc\n", rc));
    return rc;
}

STDMETHODIMP Display::TakeScreenShotPNGToArray(ULONG aScreenId, ULONG width, ULONG height,
                                               ComSafeArrayOut(BYTE, aScreenData))
{
    LogRelFlowFunc(("width=%d, height=%d\n", width, height));

    CheckComArgOutSafeArrayPointerValid(aScreenData);
    CheckComArgExpr(width, width != 0);
    CheckComArgExpr(height, height != 0);

    /* Do not allow too large screenshots. This also filters out negative
     * values passed as either 'width' or 'height'.
     */
    CheckComArgExpr(width, width <= 32767);
    CheckComArgExpr(height, height <= 32767);

    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    CHECK_CONSOLE_DRV(mpDrv);

    Console::SafeVMPtr ptrVM(mParent);
    if (!ptrVM.isOk())
        return ptrVM.rc();

    HRESULT rc = S_OK;

    LogRelFlowFunc(("Sending SCREENSHOT request\n"));

    /* Release lock because other thread (EMT) is called and it may initiate a resize
     * which also needs lock.
     *
     * This method does not need the lock anymore.
     */
    alock.release();

    size_t cbData = width * 4 * height;
    uint8_t *pu8Data = (uint8_t *)RTMemAlloc(cbData);

    if (!pu8Data)
        return E_OUTOFMEMORY;

    int vrc = displayTakeScreenshot(ptrVM.rawUVM(), this, mpDrv, aScreenId, pu8Data, width, height);

    if (RT_SUCCESS(vrc))
    {
        uint8_t *pu8PNG = NULL;
        uint32_t cbPNG = 0;
        uint32_t cxPNG = 0;
        uint32_t cyPNG = 0;

        vrc = DisplayMakePNG(pu8Data, width, height, &pu8PNG, &cbPNG, &cxPNG, &cyPNG, 0);
        if (RT_SUCCESS(vrc))
        {
            com::SafeArray<BYTE> screenData(cbPNG);
            screenData.initFrom(pu8PNG, cbPNG);
            if (pu8PNG)
                RTMemFree(pu8PNG);

            screenData.detachTo(ComSafeArrayOutArg(aScreenData));
        }
        else
        {
            if (pu8PNG)
                RTMemFree(pu8PNG);
            rc = setError(VBOX_E_IPRT_ERROR,
                          tr("Could not convert screenshot to PNG (%Rrc)"), vrc);
        }
    }
    else if (vrc == VERR_NOT_IMPLEMENTED)
        rc = setError(E_NOTIMPL,
                      tr("This feature is not implemented"));
    else
        rc = setError(VBOX_E_IPRT_ERROR,
                      tr("Could not take a screenshot (%Rrc)"), vrc);

    RTMemFree(pu8Data);

    LogRelFlowFunc(("rc=%Rhrc\n", rc));
    return rc;
}

int Display::VideoCaptureEnableScreens(ComSafeArrayIn(BOOL, aScreens))
{
#ifdef VBOX_WITH_VPX
    com::SafeArray<BOOL> Screens(ComSafeArrayInArg(aScreens));
    for (unsigned i = 0; i < Screens.size(); i++)
        maVideoRecEnabled[i] = RT_BOOL(Screens[i]);
    return VINF_SUCCESS;
#else
    return VERR_NOT_IMPLEMENTED;
#endif
}

/**
 * Start video capturing. Does nothing if capturing is already active.
 */
int Display::VideoCaptureStart()
{
#ifdef VBOX_WITH_VPX
    if (VideoRecIsEnabled(mpVideoRecCtx))
        return VINF_SUCCESS;

    int rc = VideoRecContextCreate(&mpVideoRecCtx, mcMonitors);
    if (RT_FAILURE(rc))
    {
        LogFlow(("Failed to create video recording context (%Rrc)!\n", rc));
        return rc;
    }
    ComPtr<IMachine> pMachine = mParent->machine();
    com::SafeArray<BOOL> screens;
    HRESULT hrc = pMachine->COMGETTER(VideoCaptureScreens)(ComSafeArrayAsOutParam(screens));
    AssertComRCReturn(hrc, VERR_COM_UNEXPECTED);
    for (unsigned i = 0; i < RT_ELEMENTS(maVideoRecEnabled); i++)
        maVideoRecEnabled[i] = i < screens.size() && screens[i];
    ULONG ulWidth;
    hrc = pMachine->COMGETTER(VideoCaptureWidth)(&ulWidth);
    AssertComRCReturn(hrc, VERR_COM_UNEXPECTED);
    ULONG ulHeight;
    hrc = pMachine->COMGETTER(VideoCaptureHeight)(&ulHeight);
    AssertComRCReturn(hrc, VERR_COM_UNEXPECTED);
    ULONG ulRate;
    hrc = pMachine->COMGETTER(VideoCaptureRate)(&ulRate);
    AssertComRCReturn(hrc, VERR_COM_UNEXPECTED);
    ULONG ulFPS;
    hrc = pMachine->COMGETTER(VideoCaptureFPS)(&ulFPS);
    AssertComRCReturn(hrc, VERR_COM_UNEXPECTED);
    BSTR strFile;
    hrc = pMachine->COMGETTER(VideoCaptureFile)(&strFile);
    AssertComRCReturn(hrc, VERR_COM_UNEXPECTED);
    RTTIMESPEC ts;
    RTTimeNow(&ts);
    RTTIME time;
    RTTimeExplode(&time, &ts);
    for (unsigned uScreen = 0; uScreen < mcMonitors; uScreen++)
    {
        char *pszAbsPath = RTPathAbsDup(com::Utf8Str(strFile).c_str());
        char *pszExt = RTPathExt(pszAbsPath);
        if (pszExt)
            pszExt = RTStrDup(pszExt);
        RTPathStripExt(pszAbsPath);
        if (!pszAbsPath)
            rc = VERR_INVALID_PARAMETER;
        if (!pszExt)
            pszExt = RTStrDup(".webm");
        char *pszName = NULL;
        if (RT_SUCCESS(rc))
        {
            if (mcMonitors > 1)
                rc = RTStrAPrintf(&pszName, "%s-%u%s", pszAbsPath, uScreen+1, pszExt);
            else
                rc = RTStrAPrintf(&pszName, "%s%s", pszAbsPath, pszExt);
        }
        if (RT_SUCCESS(rc))
        {
            rc = VideoRecStrmInit(mpVideoRecCtx, uScreen,
                                  pszName, ulWidth, ulHeight, ulRate, ulFPS);
            if (rc == VERR_ALREADY_EXISTS)
            {
                RTStrFree(pszName);
                pszName = NULL;

                if (mcMonitors > 1)
                    rc = RTStrAPrintf(&pszName, "%s-%04d-%02u-%02uT%02u-%02u-%02u-%09uZ-%u%s",
                                      pszAbsPath, time.i32Year, time.u8Month, time.u8MonthDay,
                                      time.u8Hour, time.u8Minute, time.u8Second, time.u32Nanosecond,
                                      uScreen+1, pszExt);
                else
                    rc = RTStrAPrintf(&pszName, "%s-%04d-%02u-%02uT%02u-%02u-%02u-%09uZ%s",
                                      pszAbsPath, time.i32Year, time.u8Month, time.u8MonthDay,
                                      time.u8Hour, time.u8Minute, time.u8Second, time.u32Nanosecond,
                                      pszExt);
                if (RT_SUCCESS(rc))
                    rc = VideoRecStrmInit(mpVideoRecCtx, uScreen,
                                          pszName, ulWidth, ulHeight, ulRate, ulFPS);
            }
        }

        if (RT_SUCCESS(rc))
            LogRel(("WebM/VP8 video recording screen #%u with %ux%u @ %u kbps, %u fps to '%s' enabled.\n",
                   uScreen, ulWidth, ulHeight, ulRate, ulFPS, pszName));
        else
            LogRel(("Failed to initialize video recording context #%u (%Rrc)!\n", uScreen, rc));
        RTStrFree(pszName);
        RTStrFree(pszExt);
        RTStrFree(pszAbsPath);
    }
    return rc;
#else
    return VERR_NOT_IMPLEMENTED;
#endif
}

/**
 * Stop video capturing. Does nothing if video capturing is not active.
 */
void Display::VideoCaptureStop()
{
#ifdef VBOX_WITH_VPX
    if (VideoRecIsEnabled(mpVideoRecCtx))
        LogRel(("WebM/VP8 video recording stopped.\n"));
    VideoRecContextClose(mpVideoRecCtx);
    mpVideoRecCtx = NULL;
#endif
}

int Display::drawToScreenEMT(Display *pDisplay, ULONG aScreenId, BYTE *address,
                             ULONG x, ULONG y, ULONG width, ULONG height)
{
    int rc = VINF_SUCCESS;
    pDisplay->vbvaLock();

    DISPLAYFBINFO *pFBInfo = &pDisplay->maFramebuffers[aScreenId];

    if (aScreenId == VBOX_VIDEO_PRIMARY_SCREEN)
    {
        if (pFBInfo->u32ResizeStatus == ResizeStatus_Void)
        {
            rc = pDisplay->mpDrv->pUpPort->pfnDisplayBlt(pDisplay->mpDrv->pUpPort, address, x, y, width, height);
        }
    }
    else if (aScreenId < pDisplay->mcMonitors)
    {
        /* Copy the bitmap to the guest VRAM. */
        const uint8_t *pu8Src       = address;
        int32_t xSrc                = 0;
        int32_t ySrc                = 0;
        uint32_t u32SrcWidth        = width;
        uint32_t u32SrcHeight       = height;
        uint32_t u32SrcLineSize     = width * 4;
        uint32_t u32SrcBitsPerPixel = 32;

        uint8_t *pu8Dst             = pFBInfo->pu8FramebufferVRAM;
        int32_t xDst                = x;
        int32_t yDst                = y;
        uint32_t u32DstWidth        = pFBInfo->w;
        uint32_t u32DstHeight       = pFBInfo->h;
        uint32_t u32DstLineSize     = pFBInfo->u32LineSize;
        uint32_t u32DstBitsPerPixel = pFBInfo->u16BitsPerPixel;

        rc = pDisplay->mpDrv->pUpPort->pfnCopyRect(pDisplay->mpDrv->pUpPort,
                                                   width, height,
                                                   pu8Src,
                                                   xSrc, ySrc,
                                                   u32SrcWidth, u32SrcHeight,
                                                   u32SrcLineSize, u32SrcBitsPerPixel,
                                                   pu8Dst,
                                                   xDst, yDst,
                                                   u32DstWidth, u32DstHeight,
                                                   u32DstLineSize, u32DstBitsPerPixel);
        if (RT_SUCCESS(rc))
        {
            if (!pFBInfo->pFramebuffer.isNull())
            {
                /* Update the changed screen area. When framebuffer uses VRAM directly, just notify
                 * it to update. And for default format, render the guest VRAM to framebuffer.
                 */
                if (   pFBInfo->fDefaultFormat
                    && !pFBInfo->fDisabled)
                {
                    address = NULL;
                    HRESULT hrc = pFBInfo->pFramebuffer->COMGETTER(Address) (&address);
                    if (SUCCEEDED(hrc) && address != NULL)
                    {
                        pu8Src       = pFBInfo->pu8FramebufferVRAM;
                        xSrc                = x;
                        ySrc                = y;
                        u32SrcWidth        = pFBInfo->w;
                        u32SrcHeight       = pFBInfo->h;
                        u32SrcLineSize     = pFBInfo->u32LineSize;
                        u32SrcBitsPerPixel = pFBInfo->u16BitsPerPixel;

                        /* Default format is 32 bpp. */
                        pu8Dst             = address;
                        xDst                = xSrc;
                        yDst                = ySrc;
                        u32DstWidth        = u32SrcWidth;
                        u32DstHeight       = u32SrcHeight;
                        u32DstLineSize     = u32DstWidth * 4;
                        u32DstBitsPerPixel = 32;

                        pDisplay->mpDrv->pUpPort->pfnCopyRect(pDisplay->mpDrv->pUpPort,
                                                              width, height,
                                                              pu8Src,
                                                              xSrc, ySrc,
                                                              u32SrcWidth, u32SrcHeight,
                                                              u32SrcLineSize, u32SrcBitsPerPixel,
                                                              pu8Dst,
                                                              xDst, yDst,
                                                              u32DstWidth, u32DstHeight,
                                                              u32DstLineSize, u32DstBitsPerPixel);
                    }
                }

                pDisplay->handleDisplayUpdate(aScreenId, x, y, width, height);
            }
        }
    }
    else
    {
        rc = VERR_INVALID_PARAMETER;
    }

    if (   RT_SUCCESS(rc)
        && pDisplay->maFramebuffers[aScreenId].u32ResizeStatus == ResizeStatus_Void)
        pDisplay->mParent->consoleVRDPServer()->SendUpdateBitmap(aScreenId, x, y, width, height);

    pDisplay->vbvaUnlock();
    return rc;
}

STDMETHODIMP Display::DrawToScreen(ULONG aScreenId, BYTE *address,
                                   ULONG x, ULONG y, ULONG width, ULONG height)
{
    /// @todo (r=dmik) this function may take too long to complete if the VM
    //  is doing something like saving state right now. Which, in case if it
    //  is called on the GUI thread, will make it unresponsive. We should
    //  check the machine state here (by enclosing the check and VMRequCall
    //  within the Console lock to make it atomic).

    LogRelFlowFunc(("address=%p, x=%d, y=%d, width=%d, height=%d\n",
                  (void *)address, x, y, width, height));

    CheckComArgNotNull(address);
    CheckComArgExpr(width, width != 0);
    CheckComArgExpr(height, height != 0);

    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    CHECK_CONSOLE_DRV(mpDrv);

    Console::SafeVMPtr ptrVM(mParent);
    if (!ptrVM.isOk())
        return ptrVM.rc();

    /* Release lock because the call scheduled on EMT may also try to take it. */
    alock.release();

    /*
     * Again we're lazy and make the graphics device do all the
     * dirty conversion work.
     */
    int rcVBox = VMR3ReqCallWaitU(ptrVM.rawUVM(), VMCPUID_ANY, (PFNRT)Display::drawToScreenEMT, 7,
                                  this, aScreenId, address, x, y, width, height);

    /*
     * If the function returns not supported, we'll have to do all the
     * work ourselves using the framebuffer.
     */
    HRESULT rc = S_OK;
    if (rcVBox == VERR_NOT_SUPPORTED || rcVBox == VERR_NOT_IMPLEMENTED)
    {
        /** @todo implement generic fallback for screen blitting. */
        rc = E_NOTIMPL;
    }
    else if (RT_FAILURE(rcVBox))
        rc = setError(VBOX_E_IPRT_ERROR,
                      tr("Could not draw to the screen (%Rrc)"), rcVBox);
//@todo
//    else
//    {
//        /* All ok. Redraw the screen. */
//        handleDisplayUpdate (x, y, width, height);
//    }

    LogRelFlowFunc(("rc=%Rhrc\n", rc));
    return rc;
}

void Display::InvalidateAndUpdateEMT(Display *pDisplay, unsigned uId, bool fUpdateAll)
{
    pDisplay->vbvaLock();
    unsigned uScreenId;
    for (uScreenId = (fUpdateAll ? 0 : uId); uScreenId < pDisplay->mcMonitors; uScreenId++)
    {
        DISPLAYFBINFO *pFBInfo = &pDisplay->maFramebuffers[uScreenId];

        if (uScreenId == VBOX_VIDEO_PRIMARY_SCREEN && !pFBInfo->pFramebuffer.isNull())
        {
            pDisplay->mpDrv->pUpPort->pfnUpdateDisplayAll(pDisplay->mpDrv->pUpPort);
        }
        else
        {
            if (   !pFBInfo->pFramebuffer.isNull()
                && !pFBInfo->fDisabled
                && pFBInfo->u32ResizeStatus == ResizeStatus_Void)
            {
                /* Render complete VRAM screen to the framebuffer.
                 * When framebuffer uses VRAM directly, just notify it to update.
                 */
                if (pFBInfo->fDefaultFormat)
                {
                    BYTE *address = NULL;
                    ULONG uWidth = 0;
                    ULONG uHeight = 0;
                    pFBInfo->pFramebuffer->COMGETTER(Width) (&uWidth);
                    pFBInfo->pFramebuffer->COMGETTER(Height) (&uHeight);
                    HRESULT hrc = pFBInfo->pFramebuffer->COMGETTER(Address) (&address);
                    if (SUCCEEDED(hrc) && address != NULL)
                    {
                        uint32_t width              = pFBInfo->w;
                        uint32_t height             = pFBInfo->h;

                        const uint8_t *pu8Src       = pFBInfo->pu8FramebufferVRAM;
                        int32_t xSrc                = 0;
                        int32_t ySrc                = 0;
                        uint32_t u32SrcWidth        = pFBInfo->w;
                        uint32_t u32SrcHeight       = pFBInfo->h;
                        uint32_t u32SrcLineSize     = pFBInfo->u32LineSize;
                        uint32_t u32SrcBitsPerPixel = pFBInfo->u16BitsPerPixel;

                        /* Default format is 32 bpp. */
                        uint8_t *pu8Dst             = address;
                        int32_t xDst                = xSrc;
                        int32_t yDst                = ySrc;
                        uint32_t u32DstWidth        = u32SrcWidth;
                        uint32_t u32DstHeight       = u32SrcHeight;
                        uint32_t u32DstLineSize     = u32DstWidth * 4;
                        uint32_t u32DstBitsPerPixel = 32;

                        /* if uWidth != pFBInfo->w and uHeight != pFBInfo->h
                         * implies resize of Framebuffer is in progress and
                         * copyrect should not be called.
                         */
                        if (uWidth == pFBInfo->w && uHeight == pFBInfo->h)
                        {

                            pDisplay->mpDrv->pUpPort->pfnCopyRect(pDisplay->mpDrv->pUpPort,
                                                                  width, height,
                                                                  pu8Src,
                                                                  xSrc, ySrc,
                                                                  u32SrcWidth, u32SrcHeight,
                                                                  u32SrcLineSize, u32SrcBitsPerPixel,
                                                                  pu8Dst,
                                                                  xDst, yDst,
                                                                  u32DstWidth, u32DstHeight,
                                                                  u32DstLineSize, u32DstBitsPerPixel);
                        }
                    }
                }

                pDisplay->handleDisplayUpdate (uScreenId, 0, 0, pFBInfo->w, pFBInfo->h);
            }
        }
        if (!fUpdateAll)
            break;
    }
    pDisplay->vbvaUnlock();
}

/**
 * Does a full invalidation of the VM display and instructs the VM
 * to update it immediately.
 *
 * @returns COM status code
 */
STDMETHODIMP Display::InvalidateAndUpdate()
{
    LogRelFlowFunc(("\n"));

    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    CHECK_CONSOLE_DRV(mpDrv);

    Console::SafeVMPtr ptrVM(mParent);
    if (!ptrVM.isOk())
        return ptrVM.rc();

    HRESULT rc = S_OK;

    LogRelFlowFunc(("Sending DPYUPDATE request\n"));

    /* Have to release the lock when calling EMT.  */
    alock.release();

    /* pdm.h says that this has to be called from the EMT thread */
    int rcVBox = VMR3ReqCallVoidWaitU(ptrVM.rawUVM(), VMCPUID_ANY, (PFNRT)Display::InvalidateAndUpdateEMT,
                                      3, this, 0, true);
    alock.acquire();

    if (RT_FAILURE(rcVBox))
        rc = setError(VBOX_E_IPRT_ERROR,
                      tr("Could not invalidate and update the screen (%Rrc)"), rcVBox);

    LogRelFlowFunc(("rc=%Rhrc\n", rc));
    return rc;
}

/**
 * Notification that the framebuffer has completed the
 * asynchronous resize processing
 *
 * @returns COM status code
 */
STDMETHODIMP Display::ResizeCompleted(ULONG aScreenId)
{
    LogRelFlowFunc(("\n"));

    /// @todo (dmik) can we AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS); here?
    //  This will require general code review and may add some details.
    //  In particular, we may want to check whether EMT is really waiting for
    //  this notification, etc. It might be also good to obey the caller to make
    //  sure this method is not called from more than one thread at a time
    //  (and therefore don't use Display lock at all here to save some
    //  milliseconds).
    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    /* this is only valid for external framebuffers */
    if (maFramebuffers[aScreenId].pFramebuffer == NULL)
        return setError(VBOX_E_NOT_SUPPORTED,
                        tr("Resize completed notification is valid only for external framebuffers"));

    /* Set the flag indicating that the resize has completed and display
     * data need to be updated. */
    bool f = ASMAtomicCmpXchgU32 (&maFramebuffers[aScreenId].u32ResizeStatus,
        ResizeStatus_UpdateDisplayData, ResizeStatus_InProgress);
    AssertRelease(f);NOREF(f);

    return S_OK;
}

STDMETHODIMP Display::CompleteVHWACommand(BYTE *pCommand)
{
#ifdef VBOX_WITH_VIDEOHWACCEL
    mpDrv->pVBVACallbacks->pfnVHWACommandCompleteAsync(mpDrv->pVBVACallbacks, (PVBOXVHWACMD)pCommand);
    return S_OK;
#else
    return E_NOTIMPL;
#endif
}

STDMETHODIMP Display::ViewportChanged(ULONG aScreenId, ULONG x, ULONG y, ULONG width, ULONG height)
{
    AssertMsgReturn(aScreenId < mcMonitors, ("aScreendId=%d mcMonitors=%d\n", aScreenId, mcMonitors), E_INVALIDARG);

#if defined(VBOX_WITH_HGCM) && defined(VBOX_WITH_CROGL)
    if (mfIsCr3DEnabled)
    {
        int rc = crViewportNotify(aScreenId, x, y, width, height);
        if (RT_FAILURE(rc))
        {
            DISPLAYFBINFO *pFb = &maFramebuffers[aScreenId];
            pFb->pendingViewportInfo.fPending = true;
            pFb->pendingViewportInfo.x = x;
            pFb->pendingViewportInfo.y = y;
            pFb->pendingViewportInfo.width = width;
            pFb->pendingViewportInfo.height = height;
        }
    }
#endif /* VBOX_WITH_CROGL && VBOX_WITH_HGCM */

#ifdef VBOX_WITH_VMSVGA
    /* The driver might not have been constructed yet */
    if (mpDrv)
        mpDrv->pUpPort->pfnSetViewPort(mpDrv->pUpPort, aScreenId, x, y, width, height);
#endif

    return S_OK;
}

// private methods
/////////////////////////////////////////////////////////////////////////////

/**
 *  Helper to update the display information from the framebuffer.
 *
 *  @thread EMT
 */
int Display::updateDisplayData(void)
{
    LogRelFlowFunc(("\n"));

    /* the driver might not have been constructed yet */
    if (!mpDrv)
        return VINF_SUCCESS;

#ifdef VBOX_STRICT
    /*
     *  Sanity check. Note that this method may be called on EMT after Console
     *  has started the power down procedure (but before our #drvDestruct() is
     *  called, in which case pVM will already be NULL but mpDrv will not). Since
     *  we don't really need pVM to proceed, we avoid this check in the release
     *  build to save some ms (necessary to construct SafeVMPtrQuiet) in this
     *  time-critical method.
     */
    Console::SafeVMPtrQuiet ptrVM(mParent);
    if (ptrVM.isOk())
    {
        PVM pVM = VMR3GetVM(ptrVM.rawUVM());
        Assert(VM_IS_EMT(pVM));
    }
#endif

    /* The method is only relevant to the primary framebuffer. */
    IFramebuffer *pFramebuffer = maFramebuffers[VBOX_VIDEO_PRIMARY_SCREEN].pFramebuffer;

    if (pFramebuffer)
    {
        HRESULT rc;
        BYTE *address = 0;
        rc = pFramebuffer->COMGETTER(Address) (&address);
        AssertComRC (rc);
        ULONG bytesPerLine = 0;
        rc = pFramebuffer->COMGETTER(BytesPerLine) (&bytesPerLine);
        AssertComRC (rc);
        ULONG bitsPerPixel = 0;
        rc = pFramebuffer->COMGETTER(BitsPerPixel) (&bitsPerPixel);
        AssertComRC (rc);
        ULONG width = 0;
        rc = pFramebuffer->COMGETTER(Width) (&width);
        AssertComRC (rc);
        ULONG height = 0;
        rc = pFramebuffer->COMGETTER(Height) (&height);
        AssertComRC (rc);

        if (   (width  != mLastWidth  && mLastWidth != 0)
            || (height != mLastHeight && mLastHeight != 0))
        {
            LogRel(("updateDisplayData: size mismatch w %d(%d) h %d(%d)\n",
                    width, mLastWidth, height, mLastHeight));
            return VERR_INVALID_STATE;
        }

        mpDrv->IConnector.pu8Data = (uint8_t *) address;
        mpDrv->IConnector.cbScanline = bytesPerLine;
        mpDrv->IConnector.cBits = bitsPerPixel;
        mpDrv->IConnector.cx = width;
        mpDrv->IConnector.cy = height;
    }
    else
    {
        /* black hole */
        mpDrv->IConnector.pu8Data = NULL;
        mpDrv->IConnector.cbScanline = 0;
        mpDrv->IConnector.cBits = 0;
        mpDrv->IConnector.cx = 0;
        mpDrv->IConnector.cy = 0;
    }
    LogRelFlowFunc(("leave\n"));
    return VINF_SUCCESS;
}

#if defined(VBOX_WITH_HGCM) && defined(VBOX_WITH_CROGL)
int Display::crViewportNotify(ULONG aScreenId, ULONG x, ULONG y, ULONG width, ULONG height)
{
    VMMDev *pVMMDev = mParent->getVMMDev();
    if (!pVMMDev)
        return VERR_INVALID_STATE;

    size_t cbData = RT_UOFFSETOF(VBOXCRCMDCTL_HGCM, aParms[5]);
    VBOXCRCMDCTL_HGCM *pData = (VBOXCRCMDCTL_HGCM*)alloca(cbData);

    pData->Hdr.enmType = VBOXCRCMDCTL_TYPE_HGCM;
    pData->Hdr.u32Function = SHCRGL_HOST_FN_VIEWPORT_CHANGED;

    pData->aParms[0].type = VBOX_HGCM_SVC_PARM_32BIT;
    pData->aParms[0].u.uint32 = aScreenId;

    pData->aParms[1].type = VBOX_HGCM_SVC_PARM_32BIT;
    pData->aParms[1].u.uint32 = x;

    pData->aParms[2].type = VBOX_HGCM_SVC_PARM_32BIT;
    pData->aParms[2].u.uint32 = y;

    pData->aParms[3].type = VBOX_HGCM_SVC_PARM_32BIT;
    pData->aParms[3].u.uint32 = width;

    pData->aParms[4].type = VBOX_HGCM_SVC_PARM_32BIT;
    pData->aParms[4].u.uint32 = height;

    return crCtlSubmitSyncIfHasDataForScreen(aScreenId, &pData->Hdr, cbData);
}
#endif

#ifdef VBOX_WITH_CRHGSMI
void Display::setupCrHgsmiData(void)
{
    VMMDev *pVMMDev = mParent->getVMMDev();
    Assert(pVMMDev);
    int rc = RTCritSectRwEnterExcl(&mCrOglLock);
    AssertRC(rc);

    if (pVMMDev)
        rc = pVMMDev->hgcmHostSvcHandleCreate("VBoxSharedCrOpenGL", &mhCrOglSvc);
    else
        rc = VERR_GENERAL_FAILURE;

    if (RT_SUCCESS(rc))
    {
        Assert(mhCrOglSvc);
        /* setup command completion callback */
        VBOXVDMACMD_CHROMIUM_CTL_CRHGSMI_SETUP_MAINCB Completion;
        Completion.Hdr.enmType = VBOXVDMACMD_CHROMIUM_CTL_TYPE_CRHGSMI_SETUP_MAINCB;
        Completion.Hdr.cbCmd = sizeof (Completion);
        Completion.hCompletion = mpDrv->pVBVACallbacks;
        Completion.pfnCompletion = mpDrv->pVBVACallbacks->pfnCrHgsmiCommandCompleteAsync;

        VBOXHGCMSVCPARM parm;
        parm.type = VBOX_HGCM_SVC_PARM_PTR;
        parm.u.pointer.addr = &Completion;
        parm.u.pointer.size = 0;

        rc = pVMMDev->hgcmHostCall("VBoxSharedCrOpenGL", SHCRGL_HOST_FN_CRHGSMI_CTL, 1, &parm);
        if (RT_SUCCESS(rc))
            mCrOglCallbacks = Completion.MainInterface;
        else
            AssertMsgFailed(("VBOXVDMACMD_CHROMIUM_CTL_TYPE_CRHGSMI_SETUP_COMPLETION failed rc %d", rc));
    }

    if (RT_FAILURE(rc))
        mhCrOglSvc = NULL;

    RTCritSectRwLeaveExcl(&mCrOglLock);
}

void Display::destructCrHgsmiData(void)
{
    int rc = RTCritSectRwEnterExcl(&mCrOglLock);
    AssertRC(rc);
    mhCrOglSvc = NULL;
    RTCritSectRwLeaveExcl(&mCrOglLock);
}
#endif /* VBOX_WITH_CRHGSMI */

/**
 *  Changes the current frame buffer. Called on EMT to avoid both
 *  race conditions and excessive locking.
 *
 *  @note locks this object for writing
 *  @thread EMT
 */
/* static */
DECLCALLBACK(int) Display::changeFramebuffer (Display *that, IFramebuffer *aFB,
                                              unsigned uScreenId)
{
    LogRelFlowFunc(("uScreenId = %d\n", uScreenId));

    AssertReturn(that, VERR_INVALID_PARAMETER);
    AssertReturn(uScreenId < that->mcMonitors, VERR_INVALID_PARAMETER);

    AutoCaller autoCaller(that);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    AutoWriteLock alock(that COMMA_LOCKVAL_SRC_POS);

    DISPLAYFBINFO *pDisplayFBInfo = &that->maFramebuffers[uScreenId];
    pDisplayFBInfo->pFramebuffer = aFB;

    that->mParent->consoleVRDPServer()->SendResize ();

    /* The driver might not have been constructed yet */
    if (that->mpDrv)
    {
        /* Setup the new framebuffer, the resize will lead to an updateDisplayData call. */
        DISPLAYFBINFO *pFBInfo = &that->maFramebuffers[uScreenId];

#if defined(VBOX_WITH_CROGL)
        /* Release the lock, because SHCRGL_HOST_FN_SCREEN_CHANGED will read current framebuffer */
        {
            BOOL is3denabled;
            that->mParent->machine()->COMGETTER(Accelerate3DEnabled)(&is3denabled);

            if (is3denabled)
            {
                alock.release();
            }
        }
#endif

        if (pFBInfo->fVBVAEnabled && pFBInfo->pu8FramebufferVRAM)
        {
            /* This display in VBVA mode. Resize it to the last guest resolution,
             * if it has been reported.
             */
            that->handleDisplayResize(uScreenId, pFBInfo->u16BitsPerPixel,
                                      pFBInfo->pu8FramebufferVRAM,
                                      pFBInfo->u32LineSize,
                                      pFBInfo->w,
                                      pFBInfo->h,
                                      pFBInfo->flags);
        }
        else if (uScreenId == VBOX_VIDEO_PRIMARY_SCREEN)
        {
            /* VGA device mode, only for the primary screen. */
            that->handleDisplayResize(VBOX_VIDEO_PRIMARY_SCREEN, that->mLastBitsPerPixel,
                                      that->mLastAddress,
                                      that->mLastBytesPerLine,
                                      that->mLastWidth,
                                      that->mLastHeight,
                                      that->mLastFlags);
        }
    }

    LogRelFlowFunc(("leave\n"));
    return VINF_SUCCESS;
}

/**
 * Handle display resize event issued by the VGA device for the primary screen.
 *
 * @see PDMIDISPLAYCONNECTOR::pfnResize
 */
DECLCALLBACK(int) Display::displayResizeCallback(PPDMIDISPLAYCONNECTOR pInterface,
                                                 uint32_t bpp, void *pvVRAM, uint32_t cbLine, uint32_t cx, uint32_t cy)
{
    PDRVMAINDISPLAY pDrv = PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface);

    LogRelFlowFunc(("bpp %d, pvVRAM %p, cbLine %d, cx %d, cy %d\n",
                  bpp, pvVRAM, cbLine, cx, cy));

    return pDrv->pDisplay->handleDisplayResize(VBOX_VIDEO_PRIMARY_SCREEN, bpp, pvVRAM, cbLine, cx, cy, VBVA_SCREEN_F_ACTIVE);
}

/**
 * Handle display update.
 *
 * @see PDMIDISPLAYCONNECTOR::pfnUpdateRect
 */
DECLCALLBACK(void) Display::displayUpdateCallback(PPDMIDISPLAYCONNECTOR pInterface,
                                                  uint32_t x, uint32_t y, uint32_t cx, uint32_t cy)
{
    PDRVMAINDISPLAY pDrv = PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface);

#ifdef DEBUG_sunlover
    LogFlowFunc(("mfVideoAccelEnabled = %d, %d,%d %dx%d\n",
                 pDrv->pDisplay->mfVideoAccelEnabled, x, y, cx, cy));
#endif /* DEBUG_sunlover */

    /* This call does update regardless of VBVA status.
     * But in VBVA mode this is called only as result of
     * pfnUpdateDisplayAll in the VGA device.
     */

    pDrv->pDisplay->handleDisplayUpdate(VBOX_VIDEO_PRIMARY_SCREEN, x, y, cx, cy);
}

/**
 * Periodic display refresh callback.
 *
 * @see PDMIDISPLAYCONNECTOR::pfnRefresh
 * @thread EMT
 */
/*static */DECLCALLBACK(void) Display::displayRefreshCallback(PPDMIDISPLAYCONNECTOR pInterface)
{
    PDRVMAINDISPLAY pDrv = PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface);

#ifdef DEBUG_sunlover
    STAM_PROFILE_START(&g_StatDisplayRefresh, a);
#endif /* DEBUG_sunlover */

#ifdef DEBUG_sunlover_2
    LogFlowFunc(("pDrv->pDisplay->mfVideoAccelEnabled = %d\n",
                 pDrv->pDisplay->mfVideoAccelEnabled));
#endif /* DEBUG_sunlover_2 */

    Display *pDisplay = pDrv->pDisplay;
    bool fNoUpdate = false; /* Do not update the display if any of the framebuffers is being resized. */
    unsigned uScreenId;

    Log2(("DisplayRefreshCallback\n"));
    for (uScreenId = 0; uScreenId < pDisplay->mcMonitors; uScreenId++)
    {
        DISPLAYFBINFO *pFBInfo = &pDisplay->maFramebuffers[uScreenId];

        /* Check the resize status. The status can be checked normally because
         * the status affects only the EMT.
         */
        uint32_t u32ResizeStatus = pFBInfo->u32ResizeStatus;

        if (u32ResizeStatus == ResizeStatus_UpdateDisplayData)
        {
            LogRelFlowFunc(("ResizeStatus_UpdateDisplayData %d\n", uScreenId));
            fNoUpdate = true; /* Always set it here, because pfnUpdateDisplayAll can cause a new resize. */
            /* The framebuffer was resized and display data need to be updated. */
            pDisplay->handleResizeCompletedEMT(FALSE);
            if (pFBInfo->u32ResizeStatus != ResizeStatus_Void)
            {
                /* The resize status could be not Void here because a pending resize is issued. */
                continue;
            }
            /* Continue with normal processing because the status here is ResizeStatus_Void.
             * Repaint all displays because VM continued to run during the framebuffer resize.
             */
            pDisplay->InvalidateAndUpdateEMT(pDisplay, uScreenId, false);
        }
        else if (u32ResizeStatus == ResizeStatus_InProgress)
        {
            /* The framebuffer is being resized. Do not call the VGA device back. Immediately return. */
            LogRelFlowFunc(("ResizeStatus_InProcess\n"));
            fNoUpdate = true;
            continue;
        }
    }

    if (!fNoUpdate)
    {
        int rc = pDisplay->videoAccelRefreshProcess();
        if (rc != VINF_TRY_AGAIN) /* Means 'do nothing' here. */
        {
            if (rc == VWRN_INVALID_STATE)
            {
                /* No VBVA do a display update. */
                DISPLAYFBINFO *pFBInfo = &pDisplay->maFramebuffers[VBOX_VIDEO_PRIMARY_SCREEN];
                if (!pFBInfo->pFramebuffer.isNull() && pFBInfo->u32ResizeStatus == ResizeStatus_Void)
                {
                    Assert(pDrv->IConnector.pu8Data);
                    pDisplay->vbvaLock();
                    pDrv->pUpPort->pfnUpdateDisplay(pDrv->pUpPort);
                    pDisplay->vbvaUnlock();
                }
            }

            /* Inform the VRDP server that the current display update sequence is
             * completed. At this moment the framebuffer memory contains a definite
             * image, that is synchronized with the orders already sent to VRDP client.
             * The server can now process redraw requests from clients or initial
             * fullscreen updates for new clients.
             */
            for (uScreenId = 0; uScreenId < pDisplay->mcMonitors; uScreenId++)
            {
                DISPLAYFBINFO *pFBInfo = &pDisplay->maFramebuffers[uScreenId];

                if (!pFBInfo->pFramebuffer.isNull() && pFBInfo->u32ResizeStatus == ResizeStatus_Void)
                {
                    Assert (pDisplay->mParent && pDisplay->mParent->consoleVRDPServer());
                    pDisplay->mParent->consoleVRDPServer()->SendUpdate (uScreenId, NULL, 0);
                }
            }
        }
    }

#ifdef VBOX_WITH_VPX
    if (VideoRecIsEnabled(pDisplay->mpVideoRecCtx))
    {
        do {
# if defined(VBOX_WITH_HGCM) && defined(VBOX_WITH_CROGL)
            if (pDisplay->mfIsCr3DEnabled)
            {
                if (ASMAtomicCmpXchgU32(&pDisplay->mfCrOglVideoRecState, CRVREC_STATE_SUBMITTED, CRVREC_STATE_IDLE))
                {
                    if (   pDisplay->mCrOglCallbacks.pfnHasData
                        && pDisplay->mCrOglCallbacks.pfnHasData())
                    {
                        /* submit */
                        VBOXCRCMDCTL_HGCM *pData = &pDisplay->mCrOglScreenshotCtl;

                        pData->Hdr.enmType = VBOXCRCMDCTL_TYPE_HGCM;
                        pData->Hdr.u32Function = SHCRGL_HOST_FN_TAKE_SCREENSHOT;

                        pData->aParms[0].type = VBOX_HGCM_SVC_PARM_PTR;
                        pData->aParms[0].u.pointer.addr = &pDisplay->mCrOglScreenshotData;
                        pData->aParms[0].u.pointer.size = sizeof (pDisplay->mCrOglScreenshotData);
                        int rc = pDisplay->crCtlSubmit(&pData->Hdr, sizeof (*pData), Display::displayVRecCompletion, pDisplay);
                        if (RT_SUCCESS(rc))
                            break;
                        AssertMsgFailed(("crCtlSubmit failed rc %d\n", rc));
                    }

                    /* no 3D data available, or error has occured,
                     * go the straight way */
                    ASMAtomicWriteU32(&pDisplay->mfCrOglVideoRecState, CRVREC_STATE_IDLE);
                }
                else
                {
                    /* record request is still in progress, don't do anything */
                    break;
                }
            }
# endif /* VBOX_WITH_HGCM && VBOX_WITH_CROGL */

            uint64_t u64Now = RTTimeProgramMilliTS();
            for (uScreenId = 0; uScreenId < pDisplay->mcMonitors; uScreenId++)
            {
                if (!pDisplay->maVideoRecEnabled[uScreenId])
                    continue;

                DISPLAYFBINFO *pFBInfo = &pDisplay->maFramebuffers[uScreenId];

                if (   !pFBInfo->pFramebuffer.isNull()
                    && !pFBInfo->fDisabled
                    && pFBInfo->u32ResizeStatus == ResizeStatus_Void)
                {
                    int rc;
                    if (   pFBInfo->fVBVAEnabled
                        && pFBInfo->pu8FramebufferVRAM)
                    {
                        rc = VideoRecCopyToIntBuf(pDisplay->mpVideoRecCtx, uScreenId, 0, 0,
                                                  FramebufferPixelFormat_FOURCC_RGB,
                                                  pFBInfo->u16BitsPerPixel,
                                                  pFBInfo->u32LineSize, pFBInfo->w, pFBInfo->h,
                                                  pFBInfo->pu8FramebufferVRAM, u64Now);
                    }
                    else
                    {
                        rc = VideoRecCopyToIntBuf(pDisplay->mpVideoRecCtx, uScreenId, 0, 0,
                                                  FramebufferPixelFormat_FOURCC_RGB,
                                                  pDrv->IConnector.cBits,
                                                  pDrv->IConnector.cbScanline, pDrv->IConnector.cx,
                                                  pDrv->IConnector.cy, pDrv->IConnector.pu8Data, u64Now);
                    }
                    if (rc == VINF_TRY_AGAIN)
                        break;
                }
            }
        } while (0);
    }
#endif /* VBOX_WITH_VPX */

#ifdef DEBUG_sunlover
    STAM_PROFILE_STOP(&g_StatDisplayRefresh, a);
#endif /* DEBUG_sunlover */
#ifdef DEBUG_sunlover_2
    LogFlowFunc(("leave\n"));
#endif /* DEBUG_sunlover_2 */
}

/**
 * Reset notification
 *
 * @see PDMIDISPLAYCONNECTOR::pfnReset
 */
DECLCALLBACK(void) Display::displayResetCallback(PPDMIDISPLAYCONNECTOR pInterface)
{
    PDRVMAINDISPLAY pDrv = PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface);

    LogRelFlowFunc(("\n"));

   /* Disable VBVA mode. */
    pDrv->pDisplay->VideoAccelEnable (false, NULL);
}

/**
 * LFBModeChange notification
 *
 * @see PDMIDISPLAYCONNECTOR::pfnLFBModeChange
 */
DECLCALLBACK(void) Display::displayLFBModeChangeCallback(PPDMIDISPLAYCONNECTOR pInterface, bool fEnabled)
{
    PDRVMAINDISPLAY pDrv = PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface);

    LogRelFlowFunc(("fEnabled=%d\n", fEnabled));

    NOREF(fEnabled);

    /* Disable VBVA mode in any case. The guest driver reenables VBVA mode if necessary. */
    /* The LFBModeChange function is called under DevVGA lock. Postpone disabling VBVA, do it in the refresh timer. */
    ASMAtomicWriteU32(&pDrv->pDisplay->mfu32PendingVideoAccelDisable, true);
}

/**
 * Adapter information change notification.
 *
 * @see PDMIDISPLAYCONNECTOR::pfnProcessAdapterData
 */
DECLCALLBACK(void) Display::displayProcessAdapterDataCallback(PPDMIDISPLAYCONNECTOR pInterface, void *pvVRAM, uint32_t u32VRAMSize)
{
    PDRVMAINDISPLAY pDrv = PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface);

    if (pvVRAM == NULL)
    {
        unsigned i;
        for (i = 0; i < pDrv->pDisplay->mcMonitors; i++)
        {
            DISPLAYFBINFO *pFBInfo = &pDrv->pDisplay->maFramebuffers[i];

            pFBInfo->u32Offset = 0;
            pFBInfo->u32MaxFramebufferSize = 0;
            pFBInfo->u32InformationSize = 0;
        }
    }
#ifndef VBOX_WITH_HGSMI
    else
    {
         uint8_t *pu8 = (uint8_t *)pvVRAM;
         pu8 += u32VRAMSize - VBOX_VIDEO_ADAPTER_INFORMATION_SIZE;

         // @todo
         uint8_t *pu8End = pu8 + VBOX_VIDEO_ADAPTER_INFORMATION_SIZE;

         VBOXVIDEOINFOHDR *pHdr;

         for (;;)
         {
             pHdr = (VBOXVIDEOINFOHDR *)pu8;
             pu8 += sizeof (VBOXVIDEOINFOHDR);

             if (pu8 >= pu8End)
             {
                 LogRel(("VBoxVideo: Guest adapter information overflow!!!\n"));
                 break;
             }

             if (pHdr->u8Type == VBOX_VIDEO_INFO_TYPE_DISPLAY)
             {
                 if (pHdr->u16Length != sizeof (VBOXVIDEOINFODISPLAY))
                 {
                     LogRel(("VBoxVideo: Guest adapter information %s invalid length %d!!!\n", "DISPLAY", pHdr->u16Length));
                     break;
                 }

                 VBOXVIDEOINFODISPLAY *pDisplay = (VBOXVIDEOINFODISPLAY *)pu8;

                 if (pDisplay->u32Index >= pDrv->pDisplay->mcMonitors)
                 {
                     LogRel(("VBoxVideo: Guest adapter information invalid display index %d!!!\n", pDisplay->u32Index));
                     break;
                 }

                 DISPLAYFBINFO *pFBInfo = &pDrv->pDisplay->maFramebuffers[pDisplay->u32Index];

                 pFBInfo->u32Offset = pDisplay->u32Offset;
                 pFBInfo->u32MaxFramebufferSize = pDisplay->u32FramebufferSize;
                 pFBInfo->u32InformationSize = pDisplay->u32InformationSize;

                 LogRelFlow(("VBOX_VIDEO_INFO_TYPE_DISPLAY: %d: at 0x%08X, size 0x%08X, info 0x%08X\n", pDisplay->u32Index, pDisplay->u32Offset, pDisplay->u32FramebufferSize, pDisplay->u32InformationSize));
             }
             else if (pHdr->u8Type == VBOX_VIDEO_INFO_TYPE_QUERY_CONF32)
             {
                 if (pHdr->u16Length != sizeof (VBOXVIDEOINFOQUERYCONF32))
                 {
                     LogRel(("VBoxVideo: Guest adapter information %s invalid length %d!!!\n", "CONF32", pHdr->u16Length));
                     break;
                 }

                 VBOXVIDEOINFOQUERYCONF32 *pConf32 = (VBOXVIDEOINFOQUERYCONF32 *)pu8;

                 switch (pConf32->u32Index)
                 {
                     case VBOX_VIDEO_QCI32_MONITOR_COUNT:
                     {
                         pConf32->u32Value = pDrv->pDisplay->mcMonitors;
                     } break;

                     case VBOX_VIDEO_QCI32_OFFSCREEN_HEAP_SIZE:
                     {
                         /* @todo make configurable. */
                         pConf32->u32Value = _1M;
                     } break;

                     default:
                         LogRel(("VBoxVideo: CONF32 %d not supported!!! Skipping.\n", pConf32->u32Index));
                 }
             }
             else if (pHdr->u8Type == VBOX_VIDEO_INFO_TYPE_END)
             {
                 if (pHdr->u16Length != 0)
                 {
                     LogRel(("VBoxVideo: Guest adapter information %s invalid length %d!!!\n", "END", pHdr->u16Length));
                     break;
                 }

                 break;
             }
             else if (pHdr->u8Type != VBOX_VIDEO_INFO_TYPE_NV_HEAP) /** @todo why is Additions/WINNT/Graphics/Miniport/VBoxVideo.cpp pushing this to us? */
             {
                 LogRel(("Guest adapter information contains unsupported type %d. The block has been skipped.\n", pHdr->u8Type));
             }

             pu8 += pHdr->u16Length;
         }
    }
#endif /* !VBOX_WITH_HGSMI */
}

/**
 * Display information change notification.
 *
 * @see PDMIDISPLAYCONNECTOR::pfnProcessDisplayData
 */
DECLCALLBACK(void) Display::displayProcessDisplayDataCallback(PPDMIDISPLAYCONNECTOR pInterface, void *pvVRAM, unsigned uScreenId)
{
    PDRVMAINDISPLAY pDrv = PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface);

    if (uScreenId >= pDrv->pDisplay->mcMonitors)
    {
        LogRel(("VBoxVideo: Guest display information invalid display index %d!!!\n", uScreenId));
        return;
    }

    /* Get the display information structure. */
    DISPLAYFBINFO *pFBInfo = &pDrv->pDisplay->maFramebuffers[uScreenId];

    uint8_t *pu8 = (uint8_t *)pvVRAM;
    pu8 += pFBInfo->u32Offset + pFBInfo->u32MaxFramebufferSize;

    // @todo
    uint8_t *pu8End = pu8 + pFBInfo->u32InformationSize;

    VBOXVIDEOINFOHDR *pHdr;

    for (;;)
    {
        pHdr = (VBOXVIDEOINFOHDR *)pu8;
        pu8 += sizeof (VBOXVIDEOINFOHDR);

        if (pu8 >= pu8End)
        {
            LogRel(("VBoxVideo: Guest display information overflow!!!\n"));
            break;
        }

        if (pHdr->u8Type == VBOX_VIDEO_INFO_TYPE_SCREEN)
        {
            if (pHdr->u16Length != sizeof (VBOXVIDEOINFOSCREEN))
            {
                LogRel(("VBoxVideo: Guest display information %s invalid length %d!!!\n", "SCREEN", pHdr->u16Length));
                break;
            }

            VBOXVIDEOINFOSCREEN *pScreen = (VBOXVIDEOINFOSCREEN *)pu8;

            pFBInfo->xOrigin = pScreen->xOrigin;
            pFBInfo->yOrigin = pScreen->yOrigin;

            pFBInfo->w = pScreen->u16Width;
            pFBInfo->h = pScreen->u16Height;

            LogRelFlow(("VBOX_VIDEO_INFO_TYPE_SCREEN: (%p) %d: at %d,%d, linesize 0x%X, size %dx%d, bpp %d, flags 0x%02X\n",
                     pHdr, uScreenId, pScreen->xOrigin, pScreen->yOrigin, pScreen->u32LineSize, pScreen->u16Width, pScreen->u16Height, pScreen->bitsPerPixel, pScreen->u8Flags));

            if (uScreenId != VBOX_VIDEO_PRIMARY_SCREEN)
            {
                /* Primary screen resize is eeeeeeeee by the VGA device. */
                if (pFBInfo->fDisabled)
                {
                    pFBInfo->fDisabled = false;
                    fireGuestMonitorChangedEvent(pDrv->pDisplay->mParent->getEventSource(),
                                                 GuestMonitorChangedEventType_Enabled,
                                                 uScreenId,
                                                 pFBInfo->xOrigin, pFBInfo->yOrigin,
                                                 pFBInfo->w, pFBInfo->h);
                }

                pDrv->pDisplay->handleDisplayResize(uScreenId, pScreen->bitsPerPixel, (uint8_t *)pvVRAM + pFBInfo->u32Offset, pScreen->u32LineSize, pScreen->u16Width, pScreen->u16Height, VBVA_SCREEN_F_ACTIVE);
            }
        }
        else if (pHdr->u8Type == VBOX_VIDEO_INFO_TYPE_END)
        {
            if (pHdr->u16Length != 0)
            {
                LogRel(("VBoxVideo: Guest adapter information %s invalid length %d!!!\n", "END", pHdr->u16Length));
                break;
            }

            break;
        }
        else if (pHdr->u8Type == VBOX_VIDEO_INFO_TYPE_HOST_EVENTS)
        {
            if (pHdr->u16Length != sizeof (VBOXVIDEOINFOHOSTEVENTS))
            {
                LogRel(("VBoxVideo: Guest display information %s invalid length %d!!!\n", "HOST_EVENTS", pHdr->u16Length));
                break;
            }

            VBOXVIDEOINFOHOSTEVENTS *pHostEvents = (VBOXVIDEOINFOHOSTEVENTS *)pu8;

            pFBInfo->pHostEvents = pHostEvents;

            LogFlow(("VBOX_VIDEO_INFO_TYPE_HOSTEVENTS: (%p)\n",
                     pHostEvents));
        }
        else if (pHdr->u8Type == VBOX_VIDEO_INFO_TYPE_LINK)
        {
            if (pHdr->u16Length != sizeof (VBOXVIDEOINFOLINK))
            {
                LogRel(("VBoxVideo: Guest adapter information %s invalid length %d!!!\n", "LINK", pHdr->u16Length));
                break;
            }

            VBOXVIDEOINFOLINK *pLink = (VBOXVIDEOINFOLINK *)pu8;
            pu8 += pLink->i32Offset;
        }
        else
        {
            LogRel(("Guest display information contains unsupported type %d\n", pHdr->u8Type));
        }

        pu8 += pHdr->u16Length;
    }
}

#ifdef VBOX_WITH_VIDEOHWACCEL

#ifndef S_FALSE
# define S_FALSE ((HRESULT)1L)
#endif

int Display::handleVHWACommandProcess(PVBOXVHWACMD pCommand)
{
    unsigned id = (unsigned)pCommand->iDisplay;
    int rc = VINF_SUCCESS;
    if (id >= mcMonitors)
        return VERR_INVALID_PARAMETER;

    ComPtr<IFramebuffer> pFramebuffer;
    AutoReadLock arlock(this COMMA_LOCKVAL_SRC_POS);
    pFramebuffer = maFramebuffers[id].pFramebuffer;
    arlock.release();

    if (pFramebuffer == NULL)
        return VERR_INVALID_STATE; /* notify we can not handle request atm */

    HRESULT hr = pFramebuffer->ProcessVHWACommand((BYTE*)pCommand);
    if (hr == S_FALSE)
        return VINF_SUCCESS;
    else if (SUCCEEDED(hr))
        return VINF_CALLBACK_RETURN;
    else if (hr == E_ACCESSDENIED)
        return VERR_INVALID_STATE; /* notify we can not handle request atm */
    else if (hr == E_NOTIMPL)
        return VERR_NOT_IMPLEMENTED;
    return VERR_GENERAL_FAILURE;
}

DECLCALLBACK(int) Display::displayVHWACommandProcess(PPDMIDISPLAYCONNECTOR pInterface, PVBOXVHWACMD pCommand)
{
    PDRVMAINDISPLAY pDrv = PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface);

    return pDrv->pDisplay->handleVHWACommandProcess(pCommand);
}
#endif

#ifdef VBOX_WITH_CRHGSMI
void Display::handleCrHgsmiCommandCompletion(int32_t result, uint32_t u32Function, PVBOXHGCMSVCPARM pParam)
{
    mpDrv->pVBVACallbacks->pfnCrHgsmiCommandCompleteAsync(mpDrv->pVBVACallbacks,
                                                          (PVBOXVDMACMD_CHROMIUM_CMD)pParam->u.pointer.addr, result);
}

void Display::handleCrHgsmiControlCompletion(int32_t result, uint32_t u32Function, PVBOXHGCMSVCPARM pParam)
{
    PVBOXVDMACMD_CHROMIUM_CTL pCtl = (PVBOXVDMACMD_CHROMIUM_CTL)pParam->u.pointer.addr;
    mpDrv->pVBVACallbacks->pfnCrHgsmiControlCompleteAsync(mpDrv->pVBVACallbacks, pCtl, result);
}

void Display::handleCrHgsmiCommandProcess(PVBOXVDMACMD_CHROMIUM_CMD pCmd, uint32_t cbCmd)
{
    int rc = VERR_NOT_SUPPORTED;
    VBOXHGCMSVCPARM parm;
    parm.type = VBOX_HGCM_SVC_PARM_PTR;
    parm.u.pointer.addr = pCmd;
    parm.u.pointer.size = cbCmd;

    if (mhCrOglSvc)
    {
        VMMDev *pVMMDev = mParent->getVMMDev();
        if (pVMMDev)
        {
            /* no completion callback is specified with this call,
             * the CrOgl code will complete the CrHgsmi command once it processes it */
            rc = pVMMDev->hgcmHostFastCallAsync(mhCrOglSvc, SHCRGL_HOST_FN_CRHGSMI_CMD, &parm, NULL, NULL);
            AssertRC(rc);
            if (RT_SUCCESS(rc))
                return;
        }
        else
            rc = VERR_INVALID_STATE;
    }

    /* we are here because something went wrong with command processing, complete it */
    handleCrHgsmiCommandCompletion(rc, SHCRGL_HOST_FN_CRHGSMI_CMD, &parm);
}

void Display::handleCrHgsmiControlProcess(PVBOXVDMACMD_CHROMIUM_CTL pCtl, uint32_t cbCtl)
{
    int rc = VERR_NOT_SUPPORTED;
    VBOXHGCMSVCPARM parm;
    parm.type = VBOX_HGCM_SVC_PARM_PTR;
    parm.u.pointer.addr = pCtl;
    parm.u.pointer.size = cbCtl;

    if (mhCrOglSvc)
    {
        VMMDev *pVMMDev = mParent->getVMMDev();
        if (pVMMDev)
        {
            bool fCheckPendingViewport = (pCtl->enmType == VBOXVDMACMD_CHROMIUM_CTL_TYPE_CRHGSMI_SETUP);
            rc = pVMMDev->hgcmHostFastCallAsync(mhCrOglSvc, SHCRGL_HOST_FN_CRHGSMI_CTL, &parm,
                                                Display::displayCrHgsmiControlCompletion, this);
            AssertRC(rc);
            if (RT_SUCCESS(rc))
            {
                if (fCheckPendingViewport)
                {
                    ULONG ul;
                    for (ul = 0; ul < mcMonitors; ul++)
                    {
                        DISPLAYFBINFO *pFb = &maFramebuffers[ul];
                        if (!pFb->pendingViewportInfo.fPending)
                            continue;

                        rc = crViewportNotify(ul, pFb->pendingViewportInfo.x, pFb->pendingViewportInfo.y,
                                              pFb->pendingViewportInfo.width, pFb->pendingViewportInfo.height);
                        if (RT_SUCCESS(rc))
                            pFb->pendingViewportInfo.fPending = false;
                        else
                        {
                            AssertMsgFailed(("crViewportNotify failed %d\n", rc));
                            rc = VINF_SUCCESS;
                        }
                    }
                }
                return;
            }
        }
        else
            rc = VERR_INVALID_STATE;
    }

    /* we are here because something went wrong with command processing, complete it */
    handleCrHgsmiControlCompletion(rc, SHCRGL_HOST_FN_CRHGSMI_CTL, &parm);
}

DECLCALLBACK(void) Display::displayCrHgsmiCommandProcess(PPDMIDISPLAYCONNECTOR pInterface, PVBOXVDMACMD_CHROMIUM_CMD pCmd,
                                                         uint32_t cbCmd)
{
    PDRVMAINDISPLAY pDrv = PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface);

    pDrv->pDisplay->handleCrHgsmiCommandProcess(pCmd, cbCmd);
}

DECLCALLBACK(void) Display::displayCrHgsmiControlProcess(PPDMIDISPLAYCONNECTOR pInterface, PVBOXVDMACMD_CHROMIUM_CTL pCmd,
                                                         uint32_t cbCmd)
{
    PDRVMAINDISPLAY pDrv = PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface);

    pDrv->pDisplay->handleCrHgsmiControlProcess(pCmd, cbCmd);
}

DECLCALLBACK(void) Display::displayCrHgsmiCommandCompletion(int32_t result, uint32_t u32Function, PVBOXHGCMSVCPARM pParam,
                                                            void *pvContext)
{
    AssertMsgFailed(("not expected!"));
    Display *pDisplay = (Display *)pvContext;
    pDisplay->handleCrHgsmiCommandCompletion(result, u32Function, pParam);
}

DECLCALLBACK(void) Display::displayCrHgsmiControlCompletion(int32_t result, uint32_t u32Function, PVBOXHGCMSVCPARM pParam,
                                                            void *pvContext)
{
    Display *pDisplay = (Display *)pvContext;
    pDisplay->handleCrHgsmiControlCompletion(result, u32Function, pParam);

}
#endif

#if defined(VBOX_WITH_HGCM) && defined(VBOX_WITH_CROGL)
DECLCALLBACK(void)  Display::displayCrHgcmCtlSubmitCompletion(int32_t result, uint32_t u32Function, PVBOXHGCMSVCPARM pParam,
                                                              void *pvContext)
{
    VBOXCRCMDCTL *pCmd = (VBOXCRCMDCTL*)pParam->u.pointer.addr;
    if (pCmd->u.pfnInternal)
        ((PFNCRCTLCOMPLETION)pCmd->u.pfnInternal)(pCmd, pParam->u.pointer.size, result, pvContext);
}

int  Display::handleCrHgcmCtlSubmit(struct VBOXCRCMDCTL* pCmd, uint32_t cbCmd,
                                    PFNCRCTLCOMPLETION pfnCompletion,
                                    void *pvCompletion)
{
    VMMDev *pVMMDev = mParent ? mParent->getVMMDev() : NULL;
    if (!pVMMDev)
    {
        AssertMsgFailed(("no vmmdev\n"));
        return VERR_INVALID_STATE;
    }

    Assert(mhCrOglSvc);
    VBOXHGCMSVCPARM parm;
    parm.type = VBOX_HGCM_SVC_PARM_PTR;
    parm.u.pointer.addr = pCmd;
    parm.u.pointer.size = cbCmd;

    pCmd->u.pfnInternal = (void(*)())pfnCompletion;
    int rc = pVMMDev->hgcmHostFastCallAsync(mhCrOglSvc, SHCRGL_HOST_FN_CTL, &parm, displayCrHgcmCtlSubmitCompletion,
                                            pvCompletion);
    if (!RT_SUCCESS(rc))
        AssertMsgFailed(("hgcmHostFastCallAsync failed rc %d\n", rc));

    return rc;
}

DECLCALLBACK(int)  Display::displayCrHgcmCtlSubmit(PPDMIDISPLAYCONNECTOR pInterface,
                                    struct VBOXCRCMDCTL* pCmd, uint32_t cbCmd,
                                    PFNCRCTLCOMPLETION pfnCompletion,
                                    void *pvCompletion)
{
    PDRVMAINDISPLAY pDrv = PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface);
    Display *pThis = pDrv->pDisplay;
    return pThis->handleCrHgcmCtlSubmit(pCmd, cbCmd, pfnCompletion, pvCompletion);
}

int Display::crCtlSubmit(struct VBOXCRCMDCTL* pCmd, uint32_t cbCmd, PFNCRCTLCOMPLETION pfnCompletion, void *pvCompletion)
{
    int rc = RTCritSectRwEnterShared(&mCrOglLock);
    if (RT_SUCCESS(rc))
    {
        if (mhCrOglSvc)
            rc = mpDrv->pVBVACallbacks->pfnCrCtlSubmit(mpDrv->pVBVACallbacks, pCmd, cbCmd, pfnCompletion, pvCompletion);
        else
            rc = VERR_NOT_SUPPORTED;

        RTCritSectRwLeaveShared(&mCrOglLock);
    }
    return rc;
}

int Display::crCtlSubmitSync(struct VBOXCRCMDCTL* pCmd, uint32_t cbCmd)
{
    int rc = RTCritSectRwEnterShared(&mCrOglLock);
    if (RT_SUCCESS(rc))
    {
        if (mhCrOglSvc)
            rc = mpDrv->pVBVACallbacks->pfnCrCtlSubmitSync(mpDrv->pVBVACallbacks, pCmd, cbCmd);
        else
            rc = VERR_NOT_SUPPORTED;

        RTCritSectRwLeaveShared(&mCrOglLock);
    }
    return rc;
}

int Display::crCtlSubmitAsyncCmdCopy(struct VBOXCRCMDCTL* pCmd, uint32_t cbCmd)
{
    VBOXCRCMDCTL* pCmdCopy = (VBOXCRCMDCTL*)RTMemAlloc(cbCmd);
    if (!pCmdCopy)
    {
        LogRel(("RTMemAlloc failed\n"));
        return VERR_NO_MEMORY;
    }

    memcpy(pCmdCopy, pCmd, cbCmd);

    int rc = crCtlSubmit(pCmdCopy, cbCmd, displayCrCmdFree, pCmdCopy);
    if (RT_FAILURE(rc))
    {
        LogRel(("crCtlSubmit failed %d\n", rc));
        RTMemFree(pCmdCopy);
        return rc;
    }

    return VINF_SUCCESS;
}

int Display::crCtlSubmitSyncIfHasDataForScreen(uint32_t u32ScreenID, struct VBOXCRCMDCTL* pCmd, uint32_t cbCmd)
{
    int rc = RTCritSectRwEnterShared(&mCrOglLock);
    AssertRCReturn(rc, rc);

    if (   mCrOglCallbacks.pfnHasDataForScreen
        && mCrOglCallbacks.pfnHasDataForScreen(u32ScreenID))
        rc = crCtlSubmitSync(pCmd, cbCmd);
    else
        rc = crCtlSubmitAsyncCmdCopy(pCmd, cbCmd);

    RTCritSectRwLeaveShared(&mCrOglLock);

    return rc;
}

bool  Display::handleCrVRecScreenshotBegin(uint32_t uScreen, uint64_t u64TimeStamp)
{
# if VBOX_WITH_VPX
    return VideoRecIsReady(mpVideoRecCtx, uScreen, u64TimeStamp);
# else
    return false;
# endif
}

void  Display::handleCrVRecScreenshotEnd(uint32_t uScreen, uint64_t u64TimeStamp)
{
}

void  Display::handleCrVRecScreenshotPerform(uint32_t uScreen,
                                             uint32_t x, uint32_t y, uint32_t uPixelFormat,
                                             uint32_t uBitsPerPixel, uint32_t uBytesPerLine,
                                             uint32_t uGuestWidth, uint32_t uGuestHeight,
                                             uint8_t *pu8BufferAddress, uint64_t u64TimeStamp)
{
    Assert(mfCrOglVideoRecState == CRVREC_STATE_SUBMITTED);
# if VBOX_WITH_VPX
    int rc = VideoRecCopyToIntBuf(mpVideoRecCtx, uScreen, x, y,
                                  uPixelFormat,
                                  uBitsPerPixel, uBytesPerLine,
                                  uGuestWidth, uGuestHeight,
                                  pu8BufferAddress, u64TimeStamp);
    Assert(rc == VINF_SUCCESS /* || rc == VERR_TRY_AGAIN || rc == VINF_TRY_AGAIN*/);
# endif
}

void  Display::handleVRecCompletion()
{
    Assert(mfCrOglVideoRecState == CRVREC_STATE_SUBMITTED);
    ASMAtomicWriteU32(&mfCrOglVideoRecState, CRVREC_STATE_IDLE);
}

DECLCALLBACK(void) Display::displayCrVRecScreenshotPerform(void *pvCtx, uint32_t uScreen,
                                                           uint32_t x, uint32_t y,
                                                           uint32_t uBitsPerPixel, uint32_t uBytesPerLine,
                                                           uint32_t uGuestWidth, uint32_t uGuestHeight,
                                                           uint8_t *pu8BufferAddress, uint64_t u64TimeStamp)
{
    Display *pDisplay = (Display *)pvCtx;
    pDisplay->handleCrVRecScreenshotPerform(uScreen,
                                            x, y, FramebufferPixelFormat_FOURCC_RGB, uBitsPerPixel,
                                            uBytesPerLine, uGuestWidth, uGuestHeight,
                                            pu8BufferAddress, u64TimeStamp);
}

DECLCALLBACK(bool) Display::displayCrVRecScreenshotBegin(void *pvCtx, uint32_t uScreen, uint64_t u64TimeStamp)
{
    Display *pDisplay = (Display *)pvCtx;
    return pDisplay->handleCrVRecScreenshotBegin(uScreen, u64TimeStamp);
}

DECLCALLBACK(void) Display::displayCrVRecScreenshotEnd(void *pvCtx, uint32_t uScreen, uint64_t u64TimeStamp)
{
    Display *pDisplay = (Display *)pvCtx;
    pDisplay->handleCrVRecScreenshotEnd(uScreen, u64TimeStamp);
}

DECLCALLBACK(void)  Display::displayVRecCompletion(struct VBOXCRCMDCTL* pCmd, uint32_t cbCmd, int rc, void *pvCompletion)
{
    Display *pDisplay = (Display *)pvCompletion;
    pDisplay->handleVRecCompletion();
}

#endif

#ifdef VBOX_WITH_HGSMI
DECLCALLBACK(int) Display::displayVBVAEnable(PPDMIDISPLAYCONNECTOR pInterface, unsigned uScreenId, PVBVAHOSTFLAGS pHostFlags, bool fRenderThreadMode)
{
    LogRelFlowFunc(("uScreenId %d\n", uScreenId));

    PDRVMAINDISPLAY pDrv = PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface);
    Display *pThis = pDrv->pDisplay;

    if (pThis->maFramebuffers[uScreenId].fVBVAEnabled && pThis->maFramebuffers[uScreenId].fRenderThreadMode != fRenderThreadMode)
    {
        LogRel(("enabling different vbva mode"));
#ifdef DEBUG_misha
        AssertMsgFailed(("enabling different vbva mode"));
#endif
        return VERR_INVALID_STATE;
    }

    pThis->maFramebuffers[uScreenId].fVBVAEnabled = true;
    pThis->maFramebuffers[uScreenId].pVBVAHostFlags = pHostFlags;
    pThis->maFramebuffers[uScreenId].fRenderThreadMode = fRenderThreadMode;

    vbvaSetMemoryFlagsHGSMI(uScreenId, pThis->mfu32SupportedOrders, pThis->mfVideoAccelVRDP, &pThis->maFramebuffers[uScreenId]);

    return VINF_SUCCESS;
}

DECLCALLBACK(void) Display::displayVBVADisable(PPDMIDISPLAYCONNECTOR pInterface, unsigned uScreenId)
{
    LogRelFlowFunc(("uScreenId %d\n", uScreenId));

    PDRVMAINDISPLAY pDrv = PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface);
    Display *pThis = pDrv->pDisplay;

    DISPLAYFBINFO *pFBInfo = &pThis->maFramebuffers[uScreenId];

    if (uScreenId == VBOX_VIDEO_PRIMARY_SCREEN)
    {
        /* Make sure that the primary screen is visible now.
         * The guest can't use VBVA anymore, so only only the VGA device output works.
         */
        if (pFBInfo->fDisabled)
        {
            pFBInfo->fDisabled = false;
            fireGuestMonitorChangedEvent(pThis->mParent->getEventSource(),
                                         GuestMonitorChangedEventType_Enabled,
                                         uScreenId,
                                         pFBInfo->xOrigin, pFBInfo->yOrigin,
                                         pFBInfo->w, pFBInfo->h);
        }
    }

    pFBInfo->fVBVAEnabled = false;
    pFBInfo->fRenderThreadMode = false;

    vbvaSetMemoryFlagsHGSMI(uScreenId, 0, false, pFBInfo);

    pFBInfo->pVBVAHostFlags = NULL;

    pFBInfo->u32Offset = 0; /* Not used in HGSMI. */
    pFBInfo->u32MaxFramebufferSize = 0; /* Not used in HGSMI. */
    pFBInfo->u32InformationSize = 0; /* Not used in HGSMI. */

    pFBInfo->xOrigin = 0;
    pFBInfo->yOrigin = 0;

    pFBInfo->w = 0;
    pFBInfo->h = 0;

    pFBInfo->u16BitsPerPixel = 0;
    pFBInfo->pu8FramebufferVRAM = NULL;
    pFBInfo->u32LineSize = 0;
}

DECLCALLBACK(void) Display::displayVBVAUpdateBegin(PPDMIDISPLAYCONNECTOR pInterface, unsigned uScreenId)
{
    LogFlowFunc(("uScreenId %d\n", uScreenId));

    PDRVMAINDISPLAY pDrv = PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface);
    Display *pThis = pDrv->pDisplay;
    DISPLAYFBINFO *pFBInfo = &pThis->maFramebuffers[uScreenId];

    if (ASMAtomicReadU32(&pThis->mu32UpdateVBVAFlags) > 0)
    {
        vbvaSetMemoryFlagsAllHGSMI(pThis->mfu32SupportedOrders, pThis->mfVideoAccelVRDP, pThis->maFramebuffers, pThis->mcMonitors);
        ASMAtomicDecU32(&pThis->mu32UpdateVBVAFlags);
    }

    if (RT_LIKELY(pFBInfo->u32ResizeStatus == ResizeStatus_Void))
    {
        if (RT_UNLIKELY(pFBInfo->cVBVASkipUpdate != 0))
        {
            /* Some updates were skipped. Note: displayVBVAUpdate* callbacks are called
             * under display device lock, so thread safe.
             */
            pFBInfo->cVBVASkipUpdate = 0;
            pThis->handleDisplayUpdate(uScreenId, pFBInfo->vbvaSkippedRect.xLeft - pFBInfo->xOrigin,
                                       pFBInfo->vbvaSkippedRect.yTop - pFBInfo->yOrigin,
                                       pFBInfo->vbvaSkippedRect.xRight - pFBInfo->vbvaSkippedRect.xLeft,
                                       pFBInfo->vbvaSkippedRect.yBottom - pFBInfo->vbvaSkippedRect.yTop);
        }
    }
    else
    {
        /* The framebuffer is being resized. */
        pFBInfo->cVBVASkipUpdate++;
    }
}

DECLCALLBACK(void) Display::displayVBVAUpdateProcess(PPDMIDISPLAYCONNECTOR pInterface, unsigned uScreenId, const PVBVACMDHDR pCmd, size_t cbCmd)
{
    LogFlowFunc(("uScreenId %d pCmd %p cbCmd %d, @%d,%d %dx%d\n", uScreenId, pCmd, cbCmd, pCmd->x, pCmd->y, pCmd->w, pCmd->h));

    PDRVMAINDISPLAY pDrv = PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface);
    Display *pThis = pDrv->pDisplay;
    DISPLAYFBINFO *pFBInfo = &pThis->maFramebuffers[uScreenId];

    if (RT_LIKELY(pFBInfo->cVBVASkipUpdate == 0))
    {
        if (pFBInfo->fDefaultFormat)
        {
            /* Make sure that framebuffer contains the same image as the guest VRAM. */
            if (   uScreenId == VBOX_VIDEO_PRIMARY_SCREEN
                && !pFBInfo->pFramebuffer.isNull()
                && !pFBInfo->fDisabled)
            {
                pDrv->pUpPort->pfnUpdateDisplayRect (pDrv->pUpPort, pCmd->x, pCmd->y, pCmd->w, pCmd->h);
            }
            else if (   !pFBInfo->pFramebuffer.isNull()
                     && !pFBInfo->fDisabled)
            {
                /* Render VRAM content to the framebuffer. */
                BYTE *address = NULL;
                HRESULT hrc = pFBInfo->pFramebuffer->COMGETTER(Address) (&address);
                if (SUCCEEDED(hrc) && address != NULL)
                {
                    uint32_t width              = pCmd->w;
                    uint32_t height             = pCmd->h;

                    const uint8_t *pu8Src       = pFBInfo->pu8FramebufferVRAM;
                    int32_t xSrc                = pCmd->x - pFBInfo->xOrigin;
                    int32_t ySrc                = pCmd->y - pFBInfo->yOrigin;
                    uint32_t u32SrcWidth        = pFBInfo->w;
                    uint32_t u32SrcHeight       = pFBInfo->h;
                    uint32_t u32SrcLineSize     = pFBInfo->u32LineSize;
                    uint32_t u32SrcBitsPerPixel = pFBInfo->u16BitsPerPixel;

                    uint8_t *pu8Dst             = address;
                    int32_t xDst                = xSrc;
                    int32_t yDst                = ySrc;
                    uint32_t u32DstWidth        = u32SrcWidth;
                    uint32_t u32DstHeight       = u32SrcHeight;
                    uint32_t u32DstLineSize     = u32DstWidth * 4;
                    uint32_t u32DstBitsPerPixel = 32;

                    pDrv->pUpPort->pfnCopyRect(pDrv->pUpPort,
                                               width, height,
                                               pu8Src,
                                               xSrc, ySrc,
                                               u32SrcWidth, u32SrcHeight,
                                               u32SrcLineSize, u32SrcBitsPerPixel,
                                               pu8Dst,
                                               xDst, yDst,
                                               u32DstWidth, u32DstHeight,
                                               u32DstLineSize, u32DstBitsPerPixel);
                }
            }
        }

        VBVACMDHDR hdrSaved = *pCmd;

        VBVACMDHDR *pHdrUnconst = (VBVACMDHDR *)pCmd;

        pHdrUnconst->x -= (int16_t)pFBInfo->xOrigin;
        pHdrUnconst->y -= (int16_t)pFBInfo->yOrigin;

        /* @todo new SendUpdate entry which can get a separate cmd header or coords. */
        pThis->mParent->consoleVRDPServer()->SendUpdate (uScreenId, pCmd, (uint32_t)cbCmd);

        *pHdrUnconst = hdrSaved;
    }
}

DECLCALLBACK(void) Display::displayVBVAUpdateEnd(PPDMIDISPLAYCONNECTOR pInterface, unsigned uScreenId, int32_t x, int32_t y, uint32_t cx, uint32_t cy)
{
    LogFlowFunc(("uScreenId %d %d,%d %dx%d\n", uScreenId, x, y, cx, cy));

    PDRVMAINDISPLAY pDrv = PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface);
    Display *pThis = pDrv->pDisplay;
    DISPLAYFBINFO *pFBInfo = &pThis->maFramebuffers[uScreenId];

    /* @todo handleFramebufferUpdate (uScreenId,
     *                                x - pThis->maFramebuffers[uScreenId].xOrigin,
     *                                y - pThis->maFramebuffers[uScreenId].yOrigin,
     *                                cx, cy);
     */
    if (RT_LIKELY(pFBInfo->cVBVASkipUpdate == 0))
    {
        pThis->handleDisplayUpdate(uScreenId, x - pFBInfo->xOrigin, y - pFBInfo->yOrigin, cx, cy);
    }
    else
    {
        /* Save the updated rectangle. */
        int32_t xRight = x + cx;
        int32_t yBottom = y + cy;

        if (pFBInfo->cVBVASkipUpdate == 1)
        {
            pFBInfo->vbvaSkippedRect.xLeft = x;
            pFBInfo->vbvaSkippedRect.yTop = y;
            pFBInfo->vbvaSkippedRect.xRight = xRight;
            pFBInfo->vbvaSkippedRect.yBottom = yBottom;
        }
        else
        {
            if (pFBInfo->vbvaSkippedRect.xLeft > x)
            {
                pFBInfo->vbvaSkippedRect.xLeft = x;
            }
            if (pFBInfo->vbvaSkippedRect.yTop > y)
            {
                pFBInfo->vbvaSkippedRect.yTop = y;
            }
            if (pFBInfo->vbvaSkippedRect.xRight < xRight)
            {
                pFBInfo->vbvaSkippedRect.xRight = xRight;
            }
            if (pFBInfo->vbvaSkippedRect.yBottom < yBottom)
            {
                pFBInfo->vbvaSkippedRect.yBottom = yBottom;
            }
        }
    }
}

#ifdef DEBUG_sunlover
static void logVBVAResize(const PVBVAINFOVIEW pView, const PVBVAINFOSCREEN pScreen, const DISPLAYFBINFO *pFBInfo)
{
    LogRel(("displayVBVAResize: [%d] %s\n"
            "    pView->u32ViewIndex     %d\n"
            "    pView->u32ViewOffset    0x%08X\n"
            "    pView->u32ViewSize      0x%08X\n"
            "    pView->u32MaxScreenSize 0x%08X\n"
            "    pScreen->i32OriginX      %d\n"
            "    pScreen->i32OriginY      %d\n"
            "    pScreen->u32StartOffset  0x%08X\n"
            "    pScreen->u32LineSize     0x%08X\n"
            "    pScreen->u32Width        %d\n"
            "    pScreen->u32Height       %d\n"
            "    pScreen->u16BitsPerPixel %d\n"
            "    pScreen->u16Flags        0x%04X\n"
            "    pFBInfo->u32Offset             0x%08X\n"
            "    pFBInfo->u32MaxFramebufferSize 0x%08X\n"
            "    pFBInfo->u32InformationSize    0x%08X\n"
            "    pFBInfo->fDisabled             %d\n"
            "    xOrigin, yOrigin, w, h:        %d,%d %dx%d\n"
            "    pFBInfo->u16BitsPerPixel       %d\n"
            "    pFBInfo->pu8FramebufferVRAM    %p\n"
            "    pFBInfo->u32LineSize           0x%08X\n"
            "    pFBInfo->flags                 0x%04X\n"
            "    pFBInfo->pHostEvents           %p\n"
            "    pFBInfo->u32ResizeStatus       %d\n"
            "    pFBInfo->fDefaultFormat        %d\n"
            "    dirtyRect                      %d-%d %d-%d\n"
            "    pFBInfo->pendingResize.fPending    %d\n"
            "    pFBInfo->pendingResize.pixelFormat %d\n"
            "    pFBInfo->pendingResize.pvVRAM      %p\n"
            "    pFBInfo->pendingResize.bpp         %d\n"
            "    pFBInfo->pendingResize.cbLine      0x%08X\n"
            "    pFBInfo->pendingResize.w,h         %dx%d\n"
            "    pFBInfo->pendingResize.flags       0x%04X\n"
            "    pFBInfo->fVBVAEnabled    %d\n"
            "    pFBInfo->cVBVASkipUpdate %d\n"
            "    pFBInfo->vbvaSkippedRect %d-%d %d-%d\n"
            "    pFBInfo->pVBVAHostFlags  %p\n"
            "",
            pScreen->u32ViewIndex,
            (pScreen->u16Flags & VBVA_SCREEN_F_DISABLED)? "DISABLED": "ENABLED",
            pView->u32ViewIndex,
            pView->u32ViewOffset,
            pView->u32ViewSize,
            pView->u32MaxScreenSize,
            pScreen->i32OriginX,
            pScreen->i32OriginY,
            pScreen->u32StartOffset,
            pScreen->u32LineSize,
            pScreen->u32Width,
            pScreen->u32Height,
            pScreen->u16BitsPerPixel,
            pScreen->u16Flags,
            pFBInfo->u32Offset,
            pFBInfo->u32MaxFramebufferSize,
            pFBInfo->u32InformationSize,
            pFBInfo->fDisabled,
            pFBInfo->xOrigin,
            pFBInfo->yOrigin,
            pFBInfo->w,
            pFBInfo->h,
            pFBInfo->u16BitsPerPixel,
            pFBInfo->pu8FramebufferVRAM,
            pFBInfo->u32LineSize,
            pFBInfo->flags,
            pFBInfo->pHostEvents,
            pFBInfo->u32ResizeStatus,
            pFBInfo->fDefaultFormat,
            pFBInfo->dirtyRect.xLeft,
            pFBInfo->dirtyRect.xRight,
            pFBInfo->dirtyRect.yTop,
            pFBInfo->dirtyRect.yBottom,
            pFBInfo->pendingResize.fPending,
            pFBInfo->pendingResize.pixelFormat,
            pFBInfo->pendingResize.pvVRAM,
            pFBInfo->pendingResize.bpp,
            pFBInfo->pendingResize.cbLine,
            pFBInfo->pendingResize.w,
            pFBInfo->pendingResize.h,
            pFBInfo->pendingResize.flags,
            pFBInfo->fVBVAEnabled,
            pFBInfo->cVBVASkipUpdate,
            pFBInfo->vbvaSkippedRect.xLeft,
            pFBInfo->vbvaSkippedRect.yTop,
            pFBInfo->vbvaSkippedRect.xRight,
            pFBInfo->vbvaSkippedRect.yBottom,
            pFBInfo->pVBVAHostFlags
          ));
}
#endif /* DEBUG_sunlover */

DECLCALLBACK(int) Display::displayVBVAResize(PPDMIDISPLAYCONNECTOR pInterface, const PVBVAINFOVIEW pView, const PVBVAINFOSCREEN pScreen, void *pvVRAM)
{
    LogRelFlowFunc(("pScreen %p, pvVRAM %p\n", pScreen, pvVRAM));

    PDRVMAINDISPLAY pDrv = PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface);
    Display *pThis = pDrv->pDisplay;

    DISPLAYFBINFO *pFBInfo = &pThis->maFramebuffers[pScreen->u32ViewIndex];

    if (pScreen->u16Flags & VBVA_SCREEN_F_DISABLED)
    {
        pThis->notifyCroglResize(pView, pScreen, pvVRAM);

        pFBInfo->fDisabled = true;
        pFBInfo->flags = pScreen->u16Flags;

        /* Ask the framebuffer to resize using a default format. The framebuffer will be black.
         * So if the frontend does not support GuestMonitorChangedEventType_Disabled event,
         * the VM window will be black. */
        uint32_t u32Width = pFBInfo->w ? pFBInfo->w : 640;
        uint32_t u32Height = pFBInfo->h ? pFBInfo->h : 480;
        pThis->handleDisplayResize(pScreen->u32ViewIndex, 0, (uint8_t *)NULL, 0,
                                   u32Width, u32Height, pScreen->u16Flags);

        fireGuestMonitorChangedEvent(pThis->mParent->getEventSource(),
                                     GuestMonitorChangedEventType_Disabled,
                                     pScreen->u32ViewIndex,
                                     0, 0, 0, 0);
        return VINF_SUCCESS;
    }

    /* If display was disabled or there is no framebuffer, a resize will be required,
     * because the framebuffer was/will be changed.
     */
    bool fResize = pFBInfo->fDisabled || pFBInfo->pFramebuffer.isNull();

    /* Check if this is a real resize or a notification about the screen origin.
     * The guest uses this VBVAResize call for both.
     */
    fResize =    fResize
              || pFBInfo->u16BitsPerPixel != pScreen->u16BitsPerPixel
              || pFBInfo->pu8FramebufferVRAM != (uint8_t *)pvVRAM + pScreen->u32StartOffset
              || pFBInfo->u32LineSize != pScreen->u32LineSize
              || pFBInfo->w != pScreen->u32Width
              || pFBInfo->h != pScreen->u32Height;

    bool fNewOrigin =    pFBInfo->xOrigin != pScreen->i32OriginX
                      || pFBInfo->yOrigin != pScreen->i32OriginY;

    if (fNewOrigin || fResize)
        pThis->notifyCroglResize(pView, pScreen, pvVRAM);

    if (pFBInfo->fDisabled)
    {
        pFBInfo->fDisabled = false;
        fireGuestMonitorChangedEvent(pThis->mParent->getEventSource(),
                                     GuestMonitorChangedEventType_Enabled,
                                     pScreen->u32ViewIndex,
                                     pScreen->i32OriginX, pScreen->i32OriginY,
                                     pScreen->u32Width, pScreen->u32Height);
        /* Continue to update pFBInfo. */
    }

    pFBInfo->u32Offset = pView->u32ViewOffset; /* Not used in HGSMI. */
    pFBInfo->u32MaxFramebufferSize = pView->u32MaxScreenSize; /* Not used in HGSMI. */
    pFBInfo->u32InformationSize = 0; /* Not used in HGSMI. */

    pFBInfo->xOrigin = pScreen->i32OriginX;
    pFBInfo->yOrigin = pScreen->i32OriginY;

    pFBInfo->w = pScreen->u32Width;
    pFBInfo->h = pScreen->u32Height;

    pFBInfo->u16BitsPerPixel = pScreen->u16BitsPerPixel;
    pFBInfo->pu8FramebufferVRAM = (uint8_t *)pvVRAM + pScreen->u32StartOffset;
    pFBInfo->u32LineSize = pScreen->u32LineSize;

    pFBInfo->flags = pScreen->u16Flags;

    pThis->xInputMappingOrigin = 0;
    pThis->yInputMappingOrigin = 0;
    pThis->cxInputMapping = 0;
    pThis->cyInputMapping = 0;

    if (fNewOrigin)
    {
        fireGuestMonitorChangedEvent(pThis->mParent->getEventSource(),
                                     GuestMonitorChangedEventType_NewOrigin,
                                     pScreen->u32ViewIndex,
                                     pScreen->i32OriginX, pScreen->i32OriginY,
                                     0, 0);
    }

    if (!fResize)
    {
        /* No parameters of the framebuffer have actually changed. */
        if (fNewOrigin)
        {
            /* VRDP server still need this notification. */
            LogRelFlowFunc(("Calling VRDP\n"));
            pThis->mParent->consoleVRDPServer()->SendResize();
        }
        return VINF_SUCCESS;
    }

    if (pFBInfo->pFramebuffer.isNull())
    {
        /* If no framebuffer, the resize will be done later when a new framebuffer will be set in changeFramebuffer. */
        return VINF_SUCCESS;
    }

    /* If the framebuffer already set for the screen, do a regular resize. */
    return pThis->handleDisplayResize(pScreen->u32ViewIndex, pScreen->u16BitsPerPixel,
                                      (uint8_t *)pvVRAM + pScreen->u32StartOffset,
                                      pScreen->u32LineSize, pScreen->u32Width, pScreen->u32Height, pScreen->u16Flags);
}

DECLCALLBACK(int) Display::displayVBVAMousePointerShape(PPDMIDISPLAYCONNECTOR pInterface, bool fVisible, bool fAlpha,
                                                        uint32_t xHot, uint32_t yHot,
                                                        uint32_t cx, uint32_t cy,
                                                        const void *pvShape)
{
    LogFlowFunc(("\n"));

    PDRVMAINDISPLAY pDrv = PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface);
    Display *pThis = pDrv->pDisplay;

    size_t cbShapeSize = 0;

    if (pvShape)
    {
        cbShapeSize = (cx + 7) / 8 * cy; /* size of the AND mask */
        cbShapeSize = ((cbShapeSize + 3) & ~3) + cx * 4 * cy; /* + gap + size of the XOR mask */
    }
    com::SafeArray<BYTE> shapeData(cbShapeSize);

    if (pvShape)
        ::memcpy(shapeData.raw(), pvShape, cbShapeSize);

    /* Tell the console about it */
    pDrv->pDisplay->mParent->onMousePointerShapeChange(fVisible, fAlpha,
                                                       xHot, yHot, cx, cy, ComSafeArrayAsInParam(shapeData));

    return VINF_SUCCESS;
}

DECLCALLBACK(void) Display::i_displayVBVAGuestCapabilityUpdate(PPDMIDISPLAYCONNECTOR pInterface, uint32_t fCapabilities)
{
    LogFlowFunc(("\n"));

    PDRVMAINDISPLAY pDrv = PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface);
    Display *pThis = pDrv->pDisplay;

    pThis->i_handleUpdateGuestVBVACapabilities(fCapabilities);
}

DECLCALLBACK(void) Display::i_displayVBVAInputMappingUpdate(PPDMIDISPLAYCONNECTOR pInterface, int32_t xOrigin, int32_t yOrigin,
                                                            uint32_t cx, uint32_t cy)
{
    LogFlowFunc(("\n"));

    PDRVMAINDISPLAY pDrv = PDMIDISPLAYCONNECTOR_2_MAINDISPLAY(pInterface);
    Display *pThis = pDrv->pDisplay;

    pThis->i_handleUpdateVBVAInputMapping(xOrigin, yOrigin, cx, cy);
}

#endif /* VBOX_WITH_HGSMI */

/**
 * @interface_method_impl{PDMIBASE,pfnQueryInterface}
 */
DECLCALLBACK(void *)  Display::drvQueryInterface(PPDMIBASE pInterface, const char *pszIID)
{
    PPDMDRVINS pDrvIns = PDMIBASE_2_PDMDRV(pInterface);
    PDRVMAINDISPLAY pDrv = PDMINS_2_DATA(pDrvIns, PDRVMAINDISPLAY);
    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIBASE, &pDrvIns->IBase);
    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIDISPLAYCONNECTOR, &pDrv->IConnector);
    return NULL;
}


/**
 * Destruct a display driver instance.
 *
 * @returns VBox status.
 * @param   pDrvIns     The driver instance data.
 */
DECLCALLBACK(void) Display::drvDestruct(PPDMDRVINS pDrvIns)
{
    PDMDRV_CHECK_VERSIONS_RETURN_VOID(pDrvIns);
    PDRVMAINDISPLAY pThis = PDMINS_2_DATA(pDrvIns, PDRVMAINDISPLAY);
    LogRelFlowFunc(("iInstance=%d\n", pDrvIns->iInstance));

    if (pThis->pDisplay)
    {
        AutoWriteLock displayLock(pThis->pDisplay COMMA_LOCKVAL_SRC_POS);
#ifdef VBOX_WITH_VPX
        pThis->pDisplay->VideoCaptureStop();
#endif
#ifdef VBOX_WITH_CRHGSMI
        pThis->pDisplay->destructCrHgsmiData();
#endif
        pThis->pDisplay->mpDrv = NULL;
        pThis->pDisplay->mpVMMDev = NULL;
        pThis->pDisplay->mLastAddress = NULL;
        pThis->pDisplay->mLastBytesPerLine = 0;
        pThis->pDisplay->mLastBitsPerPixel = 0,
        pThis->pDisplay->mLastWidth = 0;
        pThis->pDisplay->mLastHeight = 0;
    }
}


/**
 * Construct a display driver instance.
 *
 * @copydoc FNPDMDRVCONSTRUCT
 */
DECLCALLBACK(int) Display::drvConstruct(PPDMDRVINS pDrvIns, PCFGMNODE pCfg, uint32_t fFlags)
{
    PDMDRV_CHECK_VERSIONS_RETURN(pDrvIns);
    PDRVMAINDISPLAY pThis = PDMINS_2_DATA(pDrvIns, PDRVMAINDISPLAY);
    LogRelFlowFunc(("iInstance=%d\n", pDrvIns->iInstance));

    /*
     * Validate configuration.
     */
    if (!CFGMR3AreValuesValid(pCfg, "Object\0"))
        return VERR_PDM_DRVINS_UNKNOWN_CFG_VALUES;
    AssertMsgReturn(PDMDrvHlpNoAttach(pDrvIns) == VERR_PDM_NO_ATTACHED_DRIVER,
                    ("Configuration error: Not possible to attach anything to this driver!\n"),
                    VERR_PDM_DRVINS_NO_ATTACH);

    /*
     * Init Interfaces.
     */
    pDrvIns->IBase.pfnQueryInterface           = Display::drvQueryInterface;

    pThis->IConnector.pfnResize                = Display::displayResizeCallback;
    pThis->IConnector.pfnUpdateRect            = Display::displayUpdateCallback;
    pThis->IConnector.pfnRefresh               = Display::displayRefreshCallback;
    pThis->IConnector.pfnReset                 = Display::displayResetCallback;
    pThis->IConnector.pfnLFBModeChange         = Display::displayLFBModeChangeCallback;
    pThis->IConnector.pfnProcessAdapterData    = Display::displayProcessAdapterDataCallback;
    pThis->IConnector.pfnProcessDisplayData    = Display::displayProcessDisplayDataCallback;
#ifdef VBOX_WITH_VIDEOHWACCEL
    pThis->IConnector.pfnVHWACommandProcess    = Display::displayVHWACommandProcess;
#endif
#ifdef VBOX_WITH_CRHGSMI
    pThis->IConnector.pfnCrHgsmiCommandProcess = Display::displayCrHgsmiCommandProcess;
    pThis->IConnector.pfnCrHgsmiControlProcess = Display::displayCrHgsmiControlProcess;
#endif
#if defined(VBOX_WITH_HGCM) && defined(VBOX_WITH_CROGL)
    pThis->IConnector.pfnCrHgcmCtlSubmit       = Display::displayCrHgcmCtlSubmit;
#endif
#ifdef VBOX_WITH_HGSMI
    pThis->IConnector.pfnVBVAEnable            = Display::displayVBVAEnable;
    pThis->IConnector.pfnVBVADisable           = Display::displayVBVADisable;
    pThis->IConnector.pfnVBVAUpdateBegin       = Display::displayVBVAUpdateBegin;
    pThis->IConnector.pfnVBVAUpdateProcess     = Display::displayVBVAUpdateProcess;
    pThis->IConnector.pfnVBVAUpdateEnd         = Display::displayVBVAUpdateEnd;
    pThis->IConnector.pfnVBVAResize            = Display::displayVBVAResize;
    pThis->IConnector.pfnVBVAMousePointerShape = Display::displayVBVAMousePointerShape;
    pThis->IConnector.pfnVBVAGuestCapabilityUpdate = Display::i_displayVBVAGuestCapabilityUpdate;
    pThis->IConnector.pfnVBVAInputMappingUpdate = Display::i_displayVBVAInputMappingUpdate;
#endif

    /*
     * Get the IDisplayPort interface of the above driver/device.
     */
    pThis->pUpPort = PDMIBASE_QUERY_INTERFACE(pDrvIns->pUpBase, PDMIDISPLAYPORT);
    if (!pThis->pUpPort)
    {
        AssertMsgFailed(("Configuration error: No display port interface above!\n"));
        return VERR_PDM_MISSING_INTERFACE_ABOVE;
    }
#if defined(VBOX_WITH_VIDEOHWACCEL) || defined(VBOX_WITH_CRHGSMI)
    pThis->pVBVACallbacks = PDMIBASE_QUERY_INTERFACE(pDrvIns->pUpBase, PDMIDISPLAYVBVACALLBACKS);
    if (!pThis->pVBVACallbacks)
    {
        AssertMsgFailed(("Configuration error: No VBVA callback interface above!\n"));
        return VERR_PDM_MISSING_INTERFACE_ABOVE;
    }
#endif
    /*
     * Get the Display object pointer and update the mpDrv member.
     */
    void *pv;
    int rc = CFGMR3QueryPtr(pCfg, "Object", &pv);
    if (RT_FAILURE(rc))
    {
        AssertMsgFailed(("Configuration error: No/bad \"Object\" value! rc=%Rrc\n", rc));
        return rc;
    }
    Display *pDisplay = (Display *)pv;      /** @todo Check this cast! */
    pThis->pDisplay = pDisplay;
    pThis->pDisplay->mpDrv = pThis;
    /*
     * Update our display information according to the framebuffer
     */
    pDisplay->updateDisplayData();

    /*
     * Start periodic screen refreshes
     */
    pThis->pUpPort->pfnSetRefreshRate(pThis->pUpPort, 20);

#ifdef VBOX_WITH_CRHGSMI
    pDisplay->setupCrHgsmiData();
#endif

#ifdef VBOX_WITH_VPX
    ComPtr<IMachine> pMachine = pDisplay->mParent->machine();
    BOOL fEnabled = false;
    HRESULT hrc = pMachine->COMGETTER(VideoCaptureEnabled)(&fEnabled);
    AssertComRCReturn(hrc, VERR_COM_UNEXPECTED);
    if (fEnabled)
    {
        rc = pDisplay->VideoCaptureStart();
        fireVideoCaptureChangedEvent(pDisplay->mParent->getEventSource());
    }
#endif

    return rc;
}


/**
 * Display driver registration record.
 */
const PDMDRVREG Display::DrvReg =
{
    /* u32Version */
    PDM_DRVREG_VERSION,
    /* szName */
    "MainDisplay",
    /* szRCMod */
    "",
    /* szR0Mod */
    "",
    /* pszDescription */
    "Main display driver (Main as in the API).",
    /* fFlags */
    PDM_DRVREG_FLAGS_HOST_BITS_DEFAULT,
    /* fClass. */
    PDM_DRVREG_CLASS_DISPLAY,
    /* cMaxInstances */
    ~0U,
    /* cbInstance */
    sizeof(DRVMAINDISPLAY),
    /* pfnConstruct */
    Display::drvConstruct,
    /* pfnDestruct */
    Display::drvDestruct,
    /* pfnRelocate */
    NULL,
    /* pfnIOCtl */
    NULL,
    /* pfnPowerOn */
    NULL,
    /* pfnReset */
    NULL,
    /* pfnSuspend */
    NULL,
    /* pfnResume */
    NULL,
    /* pfnAttach */
    NULL,
    /* pfnDetach */
    NULL,
    /* pfnPowerOff */
    NULL,
    /* pfnSoftReset */
    NULL,
    /* u32EndVersion */
    PDM_DRVREG_VERSION
};
/* vi: set tabstop=4 shiftwidth=4 expandtab: */
