/* $Id: UIFrameBufferQuartz2D.cpp $ */
/** @file
 *
 * VBox frontends: Qt GUI ("VirtualBox"):
 * UIFrameBufferQuartz2D class implementation
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

#ifdef VBOX_GUI_USE_QUARTZ2D

#ifdef VBOX_WITH_PRECOMPILED_HEADERS
# include "precomp.h"
#else  /* !VBOX_WITH_PRECOMPILED_HEADERS */
/* Qt includes: */
# include <QApplication>

/* GUI includes: */
# include "UIFrameBufferQuartz2D.h"
# include "VBoxUtils.h"
# include "UIPopupCenter.h"
# include "UISession.h"
# include "UIMachineLogic.h"
# include "UIMachineWindow.h"
# include "UIMachineView.h"

/* COM includes: */
# include "COMEnums.h"

/* Other VBox includes: */
# include <iprt/asm.h>
#endif /* !VBOX_WITH_PRECOMPILED_HEADERS */

/** @class UIFrameBufferQuartz2D
 *
 *  The UIFrameBufferQuartz2D class is a class that implements the IFrameBuffer
 *  interface and uses Apples Quartz2D to store and render VM display data.
 */

UIFrameBufferQuartz2D::UIFrameBufferQuartz2D(UIMachineView *pMachineView)
    : UIFrameBuffer(pMachineView)
    , m_fUsesGuestVRAM(false)
    , m_pDataAddress(NULL)
    , m_pBitmapData(NULL)
    , m_uPixelFormat(FramebufferPixelFormat_FOURCC_RGB)
    , m_image(NULL)
    , mRegion(NULL)
    , mRegionUnused(NULL)
{
    Log (("Quartz2D: Creating\n"));

    UIResizeEvent event(FramebufferPixelFormat_Opaque,
                        NULL, 0, 0, 640, 480);
    resizeEvent (&event);
}

UIFrameBufferQuartz2D::~UIFrameBufferQuartz2D()
{
    Log (("Quartz2D: Deleting\n"));
    clean(false);
}

STDMETHODIMP UIFrameBufferQuartz2D::SetVisibleRegion(BYTE *pRectangles, ULONG aCount)
{
    LogRel2(("UIFrameBufferQuartz2D::SetVisibleRegion: Rectangle count=%lu\n",
             (unsigned long)aCount));

    /* Make sure rectangles were passed: */
    if (!pRectangles)
    {
        LogRel2(("UIFrameBufferQuartz2D::SetVisibleRegion: Invalid pRectangles pointer!\n"));

        return E_POINTER;
    }

    /* Lock access to frame-buffer: */
    lock();

    /* Make sure frame-buffer is used: */
    if (m_fIsMarkedAsUnused)
    {
        LogRel2(("UIFrameBufferQuartz2D::SetVisibleRegion: Ignored!\n"));

        /* Unlock access to frame-buffer: */
        unlock();

        /* Ignore SetVisibleRegion: */
        return E_FAIL;
    }

    /** @todo r=bird: Is this thread safe? If I remember the code flow correctly, the
     * GUI thread could be happily jogging along paintEvent now on another cpu core.
     * This function is called on the EMT (emulation thread). Which means, blocking
     * execution waiting for a lock is out of the question. A quick solution using
     * ASMAtomic(Cmp)XchgPtr and a struct { cAllocated; cRects; aRects[1]; }
     * *mRegion, *mUnusedRegion; should suffice (and permit you to reuse allocations). */
    RegionRects *rgnRcts = ASMAtomicXchgPtrT(&mRegionUnused, NULL, RegionRects *);
    if (rgnRcts && rgnRcts->allocated < aCount)
    {
        RTMemFree (rgnRcts);
        rgnRcts = NULL;
    }
    if (!rgnRcts)
    {
        ULONG allocated = RT_ALIGN_32(aCount + 1, 32);
        allocated = RT_MAX (128, allocated);
        rgnRcts = (RegionRects *)RTMemAlloc(RT_OFFSETOF(RegionRects, rcts[allocated]));
        if (!rgnRcts)
        {
            /* Unlock access to frame-buffer: */
            unlock();

            return E_OUTOFMEMORY;
        }
        rgnRcts->allocated = allocated;
    }
    rgnRcts->used = 0;

    /* Compose region: */
    QRegion reg;
    PRTRECT rects = (PRTRECT)pRectangles;
    QRect vmScreenRect(0, 0, width(), height());
    for (ULONG ind = 0; ind < aCount; ++ ind)
    {
        /* Get current rectangle: */
        QRect rect;
        rect.setLeft(rects->xLeft);
        rect.setTop(rects->yTop);
        /* Which is inclusive: */
        rect.setRight(rects->xRight - 1);
        rect.setBottom(rects->yBottom - 1);

        /* The rect should intersect with the vm screen. */
        rect = vmScreenRect.intersect(rect);
        ++rects;
        /* Make sure only valid rects are distributed: */
        if (rect.isValid() &&
           rect.width() > 0 && rect.height() > 0)
            reg += rect;
        else
            continue;

        /* That is some *magic* added by Knut in r27807: */
        CGRect *cgRct = &rgnRcts->rcts[rgnRcts->used];
        cgRct->origin.x = rect.x();
        cgRct->origin.y = height() - rect.y() - rect.height();
        cgRct->size.width = rect.width();
        cgRct->size.height = rect.height();
        rgnRcts->used++;
    }

    RegionRects *pOld = ASMAtomicXchgPtrT(&mRegion, rgnRcts, RegionRects *);
    if (    pOld
        &&  !ASMAtomicCmpXchgPtr(&mRegionUnused, pOld, NULL))
        RTMemFree(pOld);

    /* Send async signal to update asynchronous visible-region: */
    LogRel2(("UIFrameBufferQuartz2D::SetVisibleRegion: Sending to async-handler...\n"));
    emit sigSetVisibleRegion(reg);

    /* Unlock access to frame-buffer: */
    unlock();

    /* Confirm SetVisibleRegion: */
    return S_OK;
}

void UIFrameBufferQuartz2D::resizeEvent(UIResizeEvent *aEvent)
{
#if 0
    printf ("fmt=%lu, vram=%X, bpp=%lu, bpl=%lu, width=%lu, height=%lu\n",
           aEvent->pixelFormat(), (unsigned int)aEvent->VRAM(),
           aEvent->bitsPerPixel(), aEvent->bytesPerLine(),
           aEvent->width(), aEvent->height());
#endif
    /* Clean out old stuff */
    clean(m_width == aEvent->width()
            && m_height == aEvent->height());

    m_width = aEvent->width();
    m_height = aEvent->height();

    bool remind = false;

    /* We need a color space in any case */
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    /* Check if we support the pixel format/colordepth and can use the guest VRAM directly.
     * Mac OS X supports 16 bit also but not in the 565 mode. So we could use
     * 32 bit only. */
    if (   aEvent->pixelFormat() == FramebufferPixelFormat_FOURCC_RGB
        && aEvent->bitsPerPixel() == 32)
    {
        m_fUsesGuestVRAM = true;
//        printf ("VRAM\n");
        /* Create the image copy of the framebuffer */
        CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, aEvent->VRAM(), aEvent->bytesPerLine() * m_height, NULL);
        m_image = CGImageCreate(m_width, m_height, 8, aEvent->bitsPerPixel(), aEvent->bytesPerLine(), cs,
                                kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, dp, 0, false,
                                kCGRenderingIntentDefault);
        m_pDataAddress = aEvent->VRAM();
        CGDataProviderRelease (dp);
    }
    else
    {
        m_fUsesGuestVRAM = false;
        remind = true;
//        printf ("No VRAM\n");
        /* Create the memory we need for our image copy
         * Read somewhere that an alignment of 16 is
         * best for optimal performance. So why not. */
//        int bitmapBytesPerRow = RT_ALIGN (m_width * 4, 16);
        int bitmapBytesPerRow = m_width * 4;
        int bitmapByteCount = (bitmapBytesPerRow * m_height);
        m_pBitmapData = RTMemAllocZ(bitmapByteCount);
        CGDataProviderRef dp = CGDataProviderCreateWithData(NULL, m_pBitmapData, bitmapByteCount, NULL);
        m_image = CGImageCreate(m_width, m_height, 8, 32, bitmapBytesPerRow, cs,
                                kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little, dp, 0, false,
                                kCGRenderingIntentDefault);
        m_pDataAddress = static_cast<uchar*>(m_pBitmapData);
        CGDataProviderRelease(dp);
    }
    CGColorSpaceRelease(cs);
#ifdef VBOX_WITH_ICHAT_THEATER
    setImageRef(m_image);
#endif

    /* This check (supports graphics) is not quite right due to past mistakes
     * in the Guest Additions protocol, but in practice it should be fine. */
    if (remind && m_pMachineView->uisession()->isGuestSupportsGraphics())
        popupCenter().remindAboutWrongColorDepth(m_pMachineView->machineWindow(),
                                                      aEvent->bitsPerPixel(), 32);
    else
        popupCenter().forgetAboutWrongColorDepth(m_pMachineView->machineWindow());
}

void UIFrameBufferQuartz2D::paintEvent(QPaintEvent *aEvent)
{
    /* If the machine is NOT in 'running' state,
     * the link between framebuffer and video memory
     * is broken, we should go fallback now... */
    if (m_fUsesGuestVRAM &&
        !m_pMachineView->uisession()->isRunning() &&
        !m_pMachineView->uisession()->isPaused() &&
        /* Online snapshotting: */
        m_pMachineView->uisession()->machineState() != KMachineState_Saving)
    {
        /* Simulate fallback through fake resize-event: */
        UIResizeEvent event(FramebufferPixelFormat_Opaque, NULL, 0, 0, 640, 480);
        resizeEvent(&event);
    }

    /* For debugging /Developer/Applications/Performance Tools/Quartz
     * Debug.app is a nice tool to see which parts of the screen are
     * updated.*/
    Assert(m_image);

    QWidget* viewport = m_pMachineView->viewport();
    Assert(VALID_PTR(viewport));
    /* Get the dimensions of the viewport */
    CGRect viewRect = ::darwinToCGRect(viewport->geometry());
    /* Get the context of this window from Qt */
    CGContextRef ctx = ::darwinToCGContextRef(viewport);
    Assert(VALID_PTR(ctx));

    /* Flip the context */
    CGContextTranslateCTM(ctx, 0, viewRect.size.height);
    CGContextScaleCTM(ctx, 1.0, -1.0);

    /* We handle the seamless mode as a special case. */
    if (m_pMachineView->machineLogic()->visualStateType() == UIVisualStateType_Seamless)
    {
        /* Determine current visible region: */
        RegionRects *pRgnRcts = ASMAtomicXchgPtrT(&mRegion, NULL, RegionRects*);

        /* Prepare cleanup-region: */
        QRegion cleanupRegion(aEvent->rect());
        /* Filter out visible-rectangles;
         * That way we erase *only* invisible-rectangles of paint-region: */
        if (pRgnRcts && pRgnRcts->used > 0)
        {
            /* Slowly, step-by-step filter our visible-rectangles: */
            CGRect *pCgVisibleRect = pRgnRcts->rcts;
            for (ULONG uVisibleRectIndex = 0; uVisibleRectIndex < pRgnRcts->used; ++uVisibleRectIndex)
            {
                /* Prepare Qt visible-rectangle, flipping received CG visible-rectangle: */
                CGRect cgVisibleRectFlippedToQt = ::darwinFlipCGRect(*pCgVisibleRect, viewRect.size.height);
                QRect visibleRect(cgVisibleRectFlippedToQt.origin.x, cgVisibleRectFlippedToQt.origin.y,
                                  cgVisibleRectFlippedToQt.size.width, cgVisibleRectFlippedToQt.size.height);
                /* Filter that rectangle out: */
                cleanupRegion -= visibleRect;
                ++pCgVisibleRect;
            }
        }
        /* Erase cleanup-region: */
        if (!cleanupRegion.isEmpty())
        {
            /* Slowly, step-by-step erase cleanup-rectangles: */
            foreach (const QRect &cleanupRect, cleanupRegion.rects())
            {
                CGRect cgCleanupRect = ::darwinToCGRect(cleanupRect);
                CGContextClearRect(ctx, ::darwinFlipCGRect(cgCleanupRect, viewRect.size.height));
            }
        }

        /* Prepare painting path/clipping: */
        if (pRgnRcts)
        {
            /* If visible region is determined: */
            if (pRgnRcts->used > 0)
            {
                /* Add the clipping rects all at once (they are defined in SetVisibleRegion): */
                CGContextBeginPath(ctx);
                CGContextAddRects(ctx, pRgnRcts->rcts, pRgnRcts->used);
                /* Now convert the path to a clipping path: */
                CGContextClip(ctx);
            }

            /* Put back the visible region, free if we cannot (2+ SetVisibleRegion calls): */
            if (   !ASMAtomicCmpXchgPtr(&mRegion, pRgnRcts, NULL)
                && !ASMAtomicCmpXchgPtr(&mRegionUnused, pRgnRcts, NULL))
            {
                RTMemFree(pRgnRcts);
                pRgnRcts = NULL;
            }
        }

        /* If visible region is still determined: */
        if (pRgnRcts && pRgnRcts->used > 0)
        {
            /* Create a subimage of the current view in the size
             * of the bounding box of the current paint event: */
            CGRect cgPaintRect = ::darwinToCGRect(aEvent->rect());
            CGImageRef subImage;
            if (!m_pMachineView->pauseShot().isNull())
            {
                CGImageRef pauseImg = ::darwinToCGImageRef(&m_pMachineView->pauseShot());
                subImage = CGImageCreateWithImageInRect(pauseImg, cgPaintRect);
                CGImageRelease(pauseImg);
            }
            else
            {
#ifdef RT_ARCH_AMD64
                /* Not sure who to blame, but it seems on 64bit there goes
                 * something terrible wrong (on a second monitor) when directly
                 * using CGImageCreateWithImageInRect without making a copy. We saw
                 * something like this already with the scale mode. */
                CGImageRef tmpImage = CGImageCreateWithImageInRect(m_image, cgPaintRect);
                subImage = CGImageCreateCopy(tmpImage);
                CGImageRelease(tmpImage);
#else /* RT_ARCH_AMD64 */
                subImage = CGImageCreateWithImageInRect(m_image, CGRectMake(m_pMachineView->contentsX(), m_pMachineView->contentsY(), m_pMachineView->visibleWidth(), m_pMachineView->visibleHeight()));
#endif /* !RT_ARCH_AMD64 */
            }
            Assert(VALID_PTR(subImage));

            /* In any case clip the drawing to the view window: */
            CGContextClipToRect(ctx, viewRect);
            /* At this point draw the real vm image: */
            CGContextDrawImage(ctx, ::darwinFlipCGRect(cgPaintRect, viewRect.size.height), subImage);

            /* Release the subimage: */
            CGImageRelease(subImage);
        }
    }
    else if (   m_pMachineView->machineLogic()->visualStateType() == UIVisualStateType_Scale
             && m_scaledSize.isValid())
    {
        /* Here we paint if we didn't care about any masks */

        /* Create a subimage of the current view in the size
         * of the bounding box of the current paint event */
        CGRect ir = ::darwinToCGRect(aEvent->rect());
        CGRect is = CGRectMake(CGRectGetMinX(ir), CGRectGetMinY(ir), CGRectGetWidth(ir), CGRectGetHeight(ir));

        double iw = 1.0;
        double ih = 1.0;
        CGImageRef subImage = 0;
        if (!m_pMachineView->pauseShot().isNull())
        {
            /* The pause image is already scaled. Maybe we should change that,
             * to have a more uniform behavior and the different
             * implementations use there own scaling algorithm. */
            CGImageRef pauseImg = ::darwinToCGImageRef(&m_pMachineView->pauseShot());
            subImage = CGImageCreateWithImageInRect(pauseImg, is);
            CGImageRelease(pauseImg);
        }
        else
        {
            /* Scale all values needed for the image drawing. */
            iw = (double)CGImageGetWidth(m_image) / m_scaledSize.width();
            ih = (double)CGImageGetHeight(m_image) / m_scaledSize.height();
            /* We make sure the image is always bigger than requested to
             * compensate rounding errors. */
            /* Round down */
            is.origin.x = (int)(is.origin.x * iw);
            is.origin.y = (int)(is.origin.y * ih);
            /* Round up */
            is.size.width = (int)(is.size.width * iw) + 2;
            is.size.height = (int)(is.size.height * ih) + 2;
            /* Make sure the size is within the image boundaries */
            CGRect is1 = CGRectIntersection(is, CGRectMake(0, 0, CGImageGetWidth(m_image), CGImageGetHeight(m_image)));
            /* Cause we probably changed the rectangle to update in the origin
             * coordinate system, we have to recalculate the update rectangle
             * for the screen coordinates as well. Please note that this has to
             * be in double precision. */
            ir.origin.x = is1.origin.x / iw;
            ir.origin.y = is1.origin.y / ih;
            ir.size.width = is1.size.width / iw;
            ir.size.height = is1.size.height / ih;
            /* Create the sub image. Note: The copy step is necessary otherwise
             * strange things happens especially on Snow Leopard. No idea why. */
            CGImageRef tmpImage = CGImageCreateWithImageInRect(m_image, is1);
            subImage = CGImageCreateCopy(tmpImage);
            CGImageRelease(tmpImage);
        }
        if (subImage)
        {
            /* For more performance we set a clipping path of the regions given
             * by this paint event. */
            QVector<QRect> a = aEvent->region().rects();
            if (!a.isEmpty())
            {
                CGContextBeginPath(ctx);
                /* Add all region rects to the current context as path components */
                for (int i = 0; i < a.size(); ++i)
                    CGContextAddRect(ctx, ::darwinFlipCGRect(::darwinToCGRect(a.at(i)), CGRectGetHeight(viewRect)));
                /* Now convert the path to a clipping path. */
                CGContextClip(ctx);
            }
            /* In any case clip the drawing to the view window */
            CGContextClipToRect(ctx, viewRect);
            /* Turn the high interpolation quality on. */
            CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
            /* Draw the image. */
            CGContextDrawImage(ctx, ::darwinFlipCGRect(ir, CGRectGetHeight(viewRect)), subImage);
            CGImageRelease(subImage);
        }
    }
    else
    {
        /* Here we paint if we didn't care about any masks */

        /* Create a subimage of the current view in the size
         * of the bounding box of the current paint event */
        QRect ir = aEvent->rect();
        QRect is = QRect(ir.x() + m_pMachineView->contentsX(), ir.y() + m_pMachineView->contentsY(), ir.width(), ir.height());
        CGImageRef subImage = 0;
        if (!m_pMachineView->pauseShot().isNull())
        {
            CGImageRef pauseImg = ::darwinToCGImageRef(&m_pMachineView->pauseShot());
            subImage = CGImageCreateWithImageInRect(pauseImg, ::darwinToCGRect(is));
            CGImageRelease(pauseImg);
        }
        else
        {
#ifdef RT_ARCH_AMD64
            /* Not sure who to blame, but it seems on 64bit there goes
             * something terrible wrong (on a second monitor) when directly
             * using CGImageCreateWithImageInRect without making a copy. We saw
             * something like this already with the scale mode. */
            CGImageRef tmpImage = CGImageCreateWithImageInRect(m_image, ::darwinToCGRect(is));
            subImage = CGImageCreateCopy(tmpImage);
            CGImageRelease(tmpImage);
#else
            subImage = CGImageCreateWithImageInRect(m_image, ::darwinToCGRect(is));
#endif
        }
        if (subImage)
        {
            /* Ok, for more performance we set a clipping path of the
             * regions given by this paint event. */
            QVector<QRect> a = aEvent->region().rects();
            if (!a.isEmpty())
            {
                CGContextBeginPath (ctx);
                /* Add all region rects to the current context as path components */
                for (int i = 0; i < a.size(); ++i)
                    CGContextAddRect(ctx, ::darwinFlipCGRect(::darwinToCGRect(a[i]), viewRect.size.height));
                /* Now convert the path to a clipping path. */
                CGContextClip(ctx);
            }

            /* In any case clip the drawing to the view window */
            CGContextClipToRect(ctx, viewRect);

            /* At this point draw the real vm image */
            CGContextDrawImage(ctx, ::darwinFlipCGRect(::darwinToCGRect(ir), viewRect.size.height), subImage);
            CGImageRelease(subImage);
        }
    }
}

void UIFrameBufferQuartz2D::clean(bool fPreserveRegions)
{
    if (m_image)
    {
        CGImageRelease(m_image);
        m_image = NULL;
    }
    if (m_pBitmapData)
    {
        RTMemFree(m_pBitmapData);
        m_pBitmapData = NULL;
    }
    if (!fPreserveRegions && mRegion)
    {
        RTMemFree((void *)mRegion);
        mRegion = NULL;
    }
    if (!fPreserveRegions && mRegionUnused)
    {
        RTMemFree((void *)mRegionUnused);
        mRegionUnused = NULL;
    }
}

#endif /* VBOX_GUI_USE_QUARTZ2D */

