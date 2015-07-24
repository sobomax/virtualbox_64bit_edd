/* $Id: UIMachineLogicNormal.cpp $ */
/** @file
 * VBox Qt GUI - UIMachineLogicNormal class implementation.
 */

/*
 * Copyright (C) 2010-2013 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

/* Qt includes: */
#include <QMenu>

/* GUI includes: */
#include "VBoxGlobal.h"
#include "UIMessageCenter.h"
#include "UISession.h"
#include "UIActionPoolRuntime.h"
#include "UIMachineLogicNormal.h"
#include "UIMachineWindow.h"
#ifdef Q_WS_MAC
#include "VBoxUtils.h"
#endif /* Q_WS_MAC */

UIMachineLogicNormal::UIMachineLogicNormal(QObject *pParent, UISession *pSession)
    : UIMachineLogic(pParent, pSession, UIVisualStateType_Normal)
{
}

bool UIMachineLogicNormal::checkAvailability()
{
    /* Normal mode is always available: */
    return true;
}

void UIMachineLogicNormal::sltCheckForRequestedVisualStateType()
{
    LogRel(("UIMachineLogicNormal::sltCheckForRequestedVisualStateType: Requested-state=%d, Machine-state=%d\n",
            uisession()->requestedVisualState(), uisession()->machineState()));

    /* Do not try to change visual-state type if machine was not started yet: */
    if (!uisession()->isRunning() && !uisession()->isPaused())
        return;

    /* Do not try to change visual-state type in 'manual override' mode: */
    if (isManualOverrideMode())
        return;

    /* Check requested visual-state types: */
    switch (uisession()->requestedVisualState())
    {
        /* If 'seamless' visual-state type is requested: */
        case UIVisualStateType_Seamless:
        {
            /* And supported: */
            if (uisession()->isGuestSupportsSeamless())
            {
                LogRel(("UIMachineLogicNormal::sltCheckForRequestedVisualStateType: "
                        "Going 'seamless' as requested...\n"));
                uisession()->setRequestedVisualState(UIVisualStateType_Invalid);
                uisession()->changeVisualState(UIVisualStateType_Seamless);
            }
            else
                LogRel(("UIMachineLogicNormal::sltCheckForRequestedVisualStateType: "
                        "Rejecting 'seamless' as is it not yet supported...\n"));
            break;
        }
        default:
            break;
    }
}

void UIMachineLogicNormal::sltPrepareSharedFoldersMenu()
{
    QMenu *menu = qobject_cast<QMenu*>(sender());
    AssertMsg(menu, ("This slot should be called only on Shared Folders menu show!\n"));
    menu->clear();
    menu->addAction(gActionPool->action(UIActionIndexRuntime_Simple_SharedFoldersSettings));
}

void UIMachineLogicNormal::sltPrepareVideoCaptureMenu()
{
    QMenu *pMenu = qobject_cast<QMenu*>(sender());
    AssertMsg(pMenu, ("This slot should be called only on Video Capture menu show!\n"));
    pMenu->clear();
    pMenu->addAction(gActionPool->action(UIActionIndexRuntime_Simple_VideoCaptureSettings));
    pMenu->addAction(gActionPool->action(UIActionIndexRuntime_Toggle_VideoCapture));
}

void UIMachineLogicNormal::sltPrepareMouseIntegrationMenu()
{
    QMenu *menu = qobject_cast<QMenu*>(sender());
    AssertMsg(menu, ("This slot should be called only on Mouse Integration Menu show!\n"));
    menu->clear();
    menu->addAction(gActionPool->action(UIActionIndexRuntime_Toggle_MouseIntegration));
}

void UIMachineLogicNormal::prepareActionConnections()
{
    /* Call to base-class: */
    UIMachineLogic::prepareActionConnections();

    /* "View" actions connections: */
    connect(gActionPool->action(UIActionIndexRuntime_Toggle_Fullscreen), SIGNAL(triggered(bool)),
            this, SLOT(sltChangeVisualStateToFullscreen()));
    connect(gActionPool->action(UIActionIndexRuntime_Toggle_Seamless), SIGNAL(triggered(bool)),
            this, SLOT(sltChangeVisualStateToSeamless()));
    connect(gActionPool->action(UIActionIndexRuntime_Toggle_Scale), SIGNAL(triggered(bool)),
            this, SLOT(sltChangeVisualStateToScale()));

    /* "Device" actions connections: */
    connect(gActionPool->action(UIActionIndexRuntime_Menu_SharedFolders)->menu(), SIGNAL(aboutToShow()),
            this, SLOT(sltPrepareSharedFoldersMenu()));
    connect(gActionPool->action(UIActionIndexRuntime_Menu_VideoCapture)->menu(), SIGNAL(aboutToShow()),
            this, SLOT(sltPrepareVideoCaptureMenu()));
    connect(gActionPool->action(UIActionIndexRuntime_Menu_MouseIntegration)->menu(), SIGNAL(aboutToShow()),
            this, SLOT(sltPrepareMouseIntegrationMenu()));
}

void UIMachineLogicNormal::prepareMachineWindows()
{
    /* Do not create machine-window(s) if they created already: */
    if (isMachineWindowsCreated())
        return;

#ifdef Q_WS_MAC // TODO: Is that really need here?
    /* We have to make sure that we are getting the front most process.
     * This is necessary for Qt versions > 4.3.3: */
    ::darwinSetFrontMostProcess();
#endif /* Q_WS_MAC */

    /* Get monitors count: */
    ulong uMonitorCount = session().GetMachine().GetMonitorCount();
    /* Create machine window(s): */
    for (ulong uScreenId = 0; uScreenId < uMonitorCount; ++ uScreenId)
        addMachineWindow(UIMachineWindow::create(this, uScreenId));
    /* Order machine window(s): */
    for (ulong uScreenId = uMonitorCount; uScreenId > 0; -- uScreenId)
        machineWindows()[uScreenId - 1]->raise();

    /* Mark machine-window(s) created: */
    setMachineWindowsCreated(true);
}

void UIMachineLogicNormal::cleanupMachineWindows()
{
    /* Do not destroy machine-window(s) if they destroyed already: */
    if (!isMachineWindowsCreated())
        return;

    /* Mark machine-window(s) destroyed: */
    setMachineWindowsCreated(false);

    /* Cleanup machine-window(s): */
    foreach (UIMachineWindow *pMachineWindow, machineWindows())
        UIMachineWindow::destroy(pMachineWindow);
}

void UIMachineLogicNormal::cleanupActionConnections()
{
    /* "View" actions disconnections: */
    disconnect(gActionPool->action(UIActionIndexRuntime_Toggle_Fullscreen), SIGNAL(triggered(bool)),
               this, SLOT(sltChangeVisualStateToFullscreen()));
    disconnect(gActionPool->action(UIActionIndexRuntime_Toggle_Seamless), SIGNAL(triggered(bool)),
               this, SLOT(sltChangeVisualStateToSeamless()));
    disconnect(gActionPool->action(UIActionIndexRuntime_Toggle_Scale), SIGNAL(triggered(bool)),
               this, SLOT(sltChangeVisualStateToScale()));

    /* Call to base-class: */
    UIMachineLogic::cleanupActionConnections();
}

