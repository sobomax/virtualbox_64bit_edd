/* $Id: UIMachineViewNormal.cpp $ */
/** @file
 *
 * VBox frontends: Qt GUI ("VirtualBox"):
 * UIMachineViewNormal class implementation
 */

/*
 * Copyright (C) 2010-2012 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

/* Global includes */
#include <QApplication>
#include <QDesktopWidget>
#include <QMainWindow>
#include <QMenuBar>
#include <QScrollBar>
#include <QTimer>

/* Local includes */
#include "VBoxGlobal.h"
#include "UISession.h"
#include "UIActionPoolRuntime.h"
#include "UIMachineLogic.h"
#include "UIMachineWindow.h"
#include "UIMachineViewNormal.h"
#include "UIFrameBuffer.h"

UIMachineViewNormal::UIMachineViewNormal(  UIMachineWindow *pMachineWindow
                                         , ulong uScreenId
#ifdef VBOX_WITH_VIDEOHWACCEL
                                         , bool bAccelerate2DVideo
#endif
                                         )
    : UIMachineView(  pMachineWindow
                    , uScreenId
#ifdef VBOX_WITH_VIDEOHWACCEL
                    , bAccelerate2DVideo
#endif
                    )
    , m_bIsGuestAutoresizeEnabled(gActionPool->action(UIActionIndexRuntime_Toggle_GuestAutoresize)->isChecked())
{
    /* Resend the last resize hint if necessary: */
    maybeResendSizeHint();
}

UIMachineViewNormal::~UIMachineViewNormal()
{
    /* Save machine view settings: */
    saveMachineViewSettings();

    /* Cleanup frame buffer: */
    cleanupFrameBuffer();
}

void UIMachineViewNormal::sltAdditionsStateChanged()
{
    adjustGuestScreenSize();
}

bool UIMachineViewNormal::eventFilter(QObject *pWatched, QEvent *pEvent)
{
    if (pWatched != 0 && pWatched == machineWindow())
    {
        switch (pEvent->type())
        {
            case QEvent::Resize:
            {
                /* Recalculate max guest size: */
                setMaxGuestSize();
                /* And resize guest to current window size: */
                if (pEvent->spontaneous() && m_bIsGuestAutoresizeEnabled && uisession()->isGuestSupportsGraphics())
                    QTimer::singleShot(300, this, SLOT(sltPerformGuestResize()));
                break;
            }
            default:
                break;
        }
    }

#ifdef Q_WS_WIN
    else if (pWatched != 0 && pWatched == machineWindow()->menuBar())
    {
        /* Due to windows host uses separate 'focus set' to let menubar to
         * operate while popped up (see UIMachineViewNormal::event() for details),
         * it also requires backward processing: */
        switch (pEvent->type())
        {
            /* If menubar gets the focus while not popped up => give it back: */
            case QEvent::FocusIn:
            {
                if (!QApplication::activePopupWidget())
                    setFocus();
            }
            default:
                break;
        }
    }
#endif /* Q_WS_WIN */

    return UIMachineView::eventFilter(pWatched, pEvent);
}

void UIMachineViewNormal::prepareCommon()
{
    /* Base class common settings: */
    UIMachineView::prepareCommon();

    /* Setup size-policy: */
    setSizePolicy(QSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum));
    /* Maximum size to sizehint: */
    setMaximumSize(sizeHint());
}

void UIMachineViewNormal::prepareFilters()
{
    /* Base class filters: */
    UIMachineView::prepareFilters();

#ifdef Q_WS_WIN
    /* Install menu-bar event-filter: */
    machineWindow()->menuBar()->installEventFilter(this);
#endif /* Q_WS_WIN */
}

void UIMachineViewNormal::prepareConsoleConnections()
{
    /* Base class connections: */
    UIMachineView::prepareConsoleConnections();

    /* Guest additions state-change updater: */
    connect(uisession(), SIGNAL(sigAdditionsStateActualChange()), this, SLOT(sltAdditionsStateChanged()));
}

void UIMachineViewNormal::saveMachineViewSettings()
{
    /* If guest screen-still visible => store it's size-hint: */
    if (uisession()->isScreenVisible(screenId()))
        storeGuestSizeHint(QSize(frameBuffer()->width(), frameBuffer()->height()));
}

void UIMachineViewNormal::setGuestAutoresizeEnabled(bool fEnabled)
{
    if (m_bIsGuestAutoresizeEnabled != fEnabled)
    {
        m_bIsGuestAutoresizeEnabled = fEnabled;

        if (m_bIsGuestAutoresizeEnabled && uisession()->isGuestSupportsGraphics())
            sltPerformGuestResize();
    }
}

void UIMachineViewNormal::maybeResendSizeHint()
{
    if (m_bIsGuestAutoresizeEnabled && uisession()->isGuestSupportsGraphics())
    {
        /* Get the current machine: */
        CMachine machine = session().GetMachine();

        /* We send a guest size hint if needed to reverse a transition
         * to fullscreen or seamless. */
        QString strKey = makeExtraDataKeyPerMonitor(GUI_LastGuestSizeHintWasFullscreen);
        QString strHintSent = machine.GetExtraData(strKey);
        if (!strHintSent.isEmpty())
        {
            const QSize sizeHint = guestSizeHint();
            LogRel(("UIMachineViewNormal::maybeResendSizeHint: "
                    "Restoring guest size-hint for screen %d to %dx%d\n",
                    (int)screenId(), sizeHint.width(), sizeHint.height()));
            /* Temporarily restrict the size to prevent a brief resize to the
             * framebuffer dimensions (see @a UIMachineView::sizeHint()) before
             * the following resize() is acted upon. */
            setMaximumSize(sizeHint);
            m_sizeHintOverride = sizeHint;
            sltPerformGuestResize(sizeHint);
        }
    }
}

void UIMachineViewNormal::adjustGuestScreenSize()
{
    /* Check if we should adjust guest-screen to new size: */
    const QSize centralWidgetSize = machineWindow()->centralWidget()->size();
    if ((int)frameBuffer()->width() != centralWidgetSize.width() ||
        (int)frameBuffer()->height() != centralWidgetSize.height())
        if (m_bIsGuestAutoresizeEnabled && uisession()->isGuestSupportsGraphics())
            sltPerformGuestResize(centralWidgetSize);
}

QRect UIMachineViewNormal::workingArea() const
{
    return QApplication::desktop()->availableGeometry(this);
}

QSize UIMachineViewNormal::calculateMaxGuestSize() const
{
    /* 1) The calculation below is not reliable on some (X11) platforms until we
     *    have been visible for a fraction of a second, so do the best we can
     *    otherwise.
     * 2) We also get called early before "machineWindow" has been fully
     *    initialised, at which time we can't perform the calculation. */
    if (!isVisible())
        return workingArea().size() * 0.95;
    /* The area taken up by the machine window on the desktop, including window
     * frame, title, menu bar and status bar. */
    QSize windowSize = machineWindow()->frameGeometry().size();
    /* The window shouldn't be allowed to expand beyond the working area
     * unless it already does.  In that case the guest shouldn't expand it
     * any further though. */
    QSize maximumSize = workingArea().size().expandedTo(windowSize);
    /* The current size of the machine display. */
    QSize centralWidgetSize = machineWindow()->centralWidget()->size();
    /* To work out how big the guest display can get without the window going
     * over the maximum size we calculated above, we work out how much space
     * the other parts of the window (frame, menu bar, status bar and so on)
     * take up and subtract that space from the maximum window size. The
     * central widget shouldn't be bigger than the window, but we bound it for
     * sanity (or insanity) reasons. */
    return maximumSize - (windowSize - centralWidgetSize.boundedTo(windowSize));
}

