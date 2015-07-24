/* $Id: UIMachineWindowSeamless.cpp $ */
/** @file
 *
 * VBox frontends: Qt GUI ("VirtualBox"):
 * UIMachineWindowSeamless class implementation
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
#include <QDesktopWidget>
#include <QMenu>
#include <QTimer>

/* GUI includes: */
#include "VBoxGlobal.h"
#include "UISession.h"
#include "UIActionPoolRuntime.h"
#include "UIMachineLogicSeamless.h"
#include "UIMachineWindowSeamless.h"
#include "UIMachineView.h"
#ifndef Q_WS_MAC
# include "UIMachineDefs.h"
# include "UIMiniToolBar.h"
#endif /* !Q_WS_MAC */
#ifdef Q_WS_MAC
# include "VBoxUtils.h"
#endif /* Q_WS_MAC */

/* COM includes: */
#include "CSnapshot.h"

UIMachineWindowSeamless::UIMachineWindowSeamless(UIMachineLogic *pMachineLogic, ulong uScreenId)
    : UIMachineWindow(pMachineLogic, uScreenId)
    , m_pMainMenu(0)
#ifndef Q_WS_MAC
    , m_pMiniToolBar(0)
#endif /* !Q_WS_MAC */
{
}

#ifndef Q_WS_MAC
void UIMachineWindowSeamless::sltMachineStateChanged()
{
    /* Call to base-class: */
    UIMachineWindow::sltMachineStateChanged();

    /* Update mini-toolbar: */
    updateAppearanceOf(UIVisualElement_MiniToolBar);
}
#endif /* !Q_WS_MAC */

void UIMachineWindowSeamless::sltPopupMainMenu()
{
    /* Popup main-menu if present: */
    if (m_pMainMenu && !m_pMainMenu->isEmpty())
    {
        m_pMainMenu->popup(geometry().center());
        QTimer::singleShot(0, m_pMainMenu, SLOT(sltHighlightFirstAction()));
    }
}

void UIMachineWindowSeamless::sltRevokeFocus()
{
    /* Make sure window is visible: */
    if (!isVisible())
        return;

    /* Revoke stolen focus: */
    m_pMachineView->setFocus();
}

#ifndef VBOX_WITH_TRANSLUCENT_SEAMLESS
# ifdef Q_WS_X11
void UIMachineWindowSeamless::sltUpdateMiniToolbarMask(const QRect &geo)
{
    /* Make sure mini-toolbar exists: */
    AssertPtrReturnVoid(m_pMiniToolBar);

    /* Remember mini-toolbar mask: */
    m_maskMiniToolbar = geo;

    /* Re-assign guest mask. */
    setMask(m_maskGuest);
}
# endif /* Q_WS_X11 */
#endif /* !VBOX_WITH_TRANSLUCENT_SEAMLESS */

void UIMachineWindowSeamless::prepareMenu()
{
    /* Call to base-class: */
    UIMachineWindow::prepareMenu();

    /* Prepare menu: */
    CMachine machine = session().GetMachine();
    RuntimeMenuType restrictedMenus = VBoxGlobal::restrictedRuntimeMenuTypes(machine);
    RuntimeMenuType allowedMenus = static_cast<RuntimeMenuType>(RuntimeMenuType_All ^ restrictedMenus);
    m_pMainMenu = uisession()->newMenu(allowedMenus);
}

void UIMachineWindowSeamless::prepareVisualState()
{
    /* Call to base-class: */
    UIMachineWindow::prepareVisualState();

    /* Make sure we have no background
     * until the first one paint-event: */
    setAttribute(Qt::WA_NoSystemBackground);

#ifdef VBOX_WITH_TRANSLUCENT_SEAMLESS
# ifdef Q_WS_MAC
    /* Using native API to enable translucent background for the Mac host.
     * - We also want to disable window-shadows which is possible
     *   using Qt::WA_MacNoShadow only since Qt 4.8,
     *   while minimum supported version is 4.7.1 for now: */
    ::darwinSetShowsWindowTransparent(this, true);
# else /* Q_WS_MAC */
    /* Using Qt API to enable translucent background:
     * - Under Win host Qt conflicts with 3D stuff (black seamless regions).
     * - Under Mac host Qt doesn't allows to disable window-shadows
     *   until version 4.8, but minimum supported version is 4.7.1 for now.
     * - Under x11 host Qt has it broken with KDE 4.9 (black background): */
    setAttribute(Qt::WA_TranslucentBackground);
# endif /* !Q_WS_MAC */
#else /* !VBOX_WITH_TRANSLUCENT_SEAMLESS */
    /* Make sure we have no background
     * until the first one set-region-event: */
    setMask(m_maskGuest);
#endif /* !VBOX_WITH_TRANSLUCENT_SEAMLESS */

#ifndef Q_WS_MAC
    /* Prepare mini-toolbar: */
    prepareMiniToolbar();
#endif /* !Q_WS_MAC */
}

#ifndef Q_WS_MAC
void UIMachineWindowSeamless::prepareMiniToolbar()
{
    /* Get machine: */
    CMachine m = machine();

    /* Make sure mini-toolbar is necessary: */
    bool fIsActive = m.GetExtraData(GUI_ShowMiniToolBar) != "no";
    if (!fIsActive)
        return;

    /* Get the mini-toolbar alignment: */
    bool fIsAtTop = m.GetExtraData(GUI_MiniToolBarAlignment) == "top";
    /* Get the mini-toolbar auto-hide feature availability: */
    bool fIsAutoHide = m.GetExtraData(GUI_MiniToolBarAutoHide) != "off";
    /* Create mini-toolbar: */
    m_pMiniToolBar = new UIRuntimeMiniToolBar(this,
                                              GeometryType_Available,
                                              fIsAtTop ? Qt::AlignTop : Qt::AlignBottom,
                                              fIsAutoHide);
    m_pMiniToolBar->show();
    QList<QMenu*> menus;
    RuntimeMenuType restrictedMenus = VBoxGlobal::restrictedRuntimeMenuTypes(m);
    RuntimeMenuType allowedMenus = static_cast<RuntimeMenuType>(RuntimeMenuType_All ^ restrictedMenus);
    QList<QAction*> actions = uisession()->newMenu(allowedMenus)->actions();
    for (int i=0; i < actions.size(); ++i)
        menus << actions.at(i)->menu();
    m_pMiniToolBar->addMenus(menus);
    connect(m_pMiniToolBar, SIGNAL(sigMinimizeAction()), this, SLOT(showMinimized()));
    connect(m_pMiniToolBar, SIGNAL(sigExitAction()),
            gActionPool->action(UIActionIndexRuntime_Toggle_Seamless), SLOT(trigger()));
    connect(m_pMiniToolBar, SIGNAL(sigCloseAction()),
            gActionPool->action(UIActionIndexRuntime_Simple_Close), SLOT(trigger()));
    connect(m_pMiniToolBar, SIGNAL(sigNotifyAboutFocusStolen()), this, SLOT(sltRevokeFocus()));
#ifndef VBOX_WITH_TRANSLUCENT_SEAMLESS
# ifdef Q_WS_X11
    connect(m_pMiniToolBar, SIGNAL(sigNotifyAboutGeometryChange(const QRect&)),
            this, SLOT(sltUpdateMiniToolbarMask(const QRect&)));
# endif /* Q_WS_X11 */
#endif /* !VBOX_WITH_TRANSLUCENT_SEAMLESS */
}
#endif /* !Q_WS_MAC */

#ifndef Q_WS_MAC
void UIMachineWindowSeamless::cleanupMiniToolbar()
{
    /* Make sure mini-toolbar was created: */
    if (!m_pMiniToolBar)
        return;

    /* Save mini-toolbar settings: */
    machine().SetExtraData(GUI_MiniToolBarAutoHide, m_pMiniToolBar->autoHide() ? QString() : "off");
    /* Delete mini-toolbar: */
    delete m_pMiniToolBar;
    m_pMiniToolBar = 0;
}
#endif /* !Q_WS_MAC */

void UIMachineWindowSeamless::cleanupVisualState()
{
#ifndef Q_WS_MAC
    /* Cleanup mini-toolbar: */
    cleanupMiniToolbar();
#endif /* !Q_WS_MAC */

    /* Call to base-class: */
    UIMachineWindow::cleanupVisualState();
}

void UIMachineWindowSeamless::cleanupMenu()
{
    /* Cleanup menu: */
    delete m_pMainMenu;
    m_pMainMenu = 0;

    /* Call to base-class: */
    UIMachineWindow::cleanupMenu();
}

void UIMachineWindowSeamless::placeOnScreen()
{
    /* Get corresponding screen: */
    int iScreen = qobject_cast<UIMachineLogicSeamless*>(machineLogic())->hostScreenForGuestScreen(m_uScreenId);
    /* And corresponding working area: */
    QRect workingArea = QApplication::desktop()->availableGeometry(iScreen);

    /* Move to the appropriate position: */
    move(workingArea.topLeft());

    /* Resize to the appropriate size: */
    resize(workingArea.size());
}

void UIMachineWindowSeamless::showInNecessaryMode()
{
    /* Make sure this window has seamless logic: */
    UIMachineLogicSeamless *pSeamlessLogic = qobject_cast<UIMachineLogicSeamless*>(machineLogic());
    AssertPtrReturnVoid(pSeamlessLogic);

    /* Make sure this window should be shown and mapped to some host-screen: */
    if (!uisession()->isScreenVisible(m_uScreenId) ||
        !pSeamlessLogic->hasHostScreenForGuestScreen(m_uScreenId))
    {
#ifndef Q_WS_MAC
        /* Hide mini-toolbar: */
        if (m_pMiniToolBar)
            m_pMiniToolBar->hide();
#endif /* !Q_WS_MAC */
        /* Hide window: */
        hide();
        return;
    }

    /* Make sure this window is not minimized: */
    if (isMinimized())
        return;

    /* Make sure this window is maximized and placed on valid screen: */
    placeOnScreen();

    /* Show in normal mode: */
    show();

    /* Adjust machine-view size if necessary: */
    adjustMachineViewSize();

#ifndef Q_WS_MAC
    /* Show mini-toolbar: */
    if (m_pMiniToolBar)
        m_pMiniToolBar->show();
#endif /* !Q_WS_MAC */
}

void UIMachineWindowSeamless::adjustMachineViewSize()
{
    /* Call to base-class: */
    UIMachineWindow::adjustMachineViewSize();

#ifndef Q_WS_MAC
    /* If mini-toolbar present: */
    if (m_pMiniToolBar)
    {
        /* Make sure this window has seamless logic: */
        const UIMachineLogicSeamless *pSeamlessLogic = qobject_cast<UIMachineLogicSeamless*>(machineLogic());
        AssertPtrReturnVoid(pSeamlessLogic);

        /* Which host-screen should that machine-window located on? */
        const int iHostScreen = pSeamlessLogic->hostScreenForGuestScreen(m_uScreenId);

        /* Move mini-toolbar into appropriate place: */
        m_pMiniToolBar->adjustGeometry(iHostScreen);
    }
#endif /* !Q_WS_MAC */
}

#ifndef Q_WS_MAC
void UIMachineWindowSeamless::updateAppearanceOf(int iElement)
{
    /* Call to base-class: */
    UIMachineWindow::updateAppearanceOf(iElement);

    /* Update mini-toolbar: */
    if (iElement & UIVisualElement_MiniToolBar)
    {
        if (m_pMiniToolBar)
        {
            /* Get machine: */
            const CMachine &m = machine();
            /* Get snapshot(s): */
            QString strSnapshotName;
            if (m.GetSnapshotCount() > 0)
            {
                CSnapshot snapshot = m.GetCurrentSnapshot();
                strSnapshotName = " (" + snapshot.GetName() + ")";
            }
            /* Update mini-toolbar text: */
            m_pMiniToolBar->setText(m.GetName() + strSnapshotName);
        }
    }
}
#endif /* !Q_WS_MAC */

#if defined(VBOX_WITH_TRANSLUCENT_SEAMLESS) && defined(Q_WS_WIN)
void UIMachineWindowSeamless::showEvent(QShowEvent*)
{
    /* Following workaround allows to fix the next Qt BUG:
     * https://bugreports.qt-project.org/browse/QTBUG-17548
     * https://bugreports.qt-project.org/browse/QTBUG-30974
     * Widgets with Qt::WA_TranslucentBackground attribute
     * stops repainting after minimizing/restoring, we have to call for single update. */
    QApplication::postEvent(this, new QEvent(QEvent::UpdateRequest), Qt::LowEventPriority);
}
#endif /* VBOX_WITH_TRANSLUCENT_SEAMLESS && Q_WS_WIN */

#ifndef VBOX_WITH_TRANSLUCENT_SEAMLESS
void UIMachineWindowSeamless::setMask(const QRegion &maskGuest)
{
    /* Remember new guest mask: */
    m_maskGuest = maskGuest;

    /* Prepare full mask: */
    QRegion maskFull(m_maskGuest);

    /* Shift full mask if left or top spacer width is NOT zero: */
    if (m_pLeftSpacer->geometry().width() || m_pTopSpacer->geometry().height())
        maskFull.translate(m_pLeftSpacer->geometry().width(), m_pTopSpacer->geometry().height());

# ifdef Q_WS_X11
    /* Take into account mini-toolbar mask if necessary: */
    if (m_pMiniToolBar)
        maskFull += m_maskMiniToolbar;
# endif /* Q_WS_X11 */

    /* Seamless-window for empty full mask should be empty too,
     * but the QWidget::setMask() wrapper doesn't allow this.
     * Instead, we see a full guest-screen of available-geometry size.
     * So we have to make sure full mask have at least one pixel. */
    if (maskFull.isEmpty())
        maskFull += QRect(0, 0, 1, 1);

    /* Make sure full mask had changed: */
    if (m_maskFull != maskFull)
    {
        /* Compose viewport region to update: */
        QRegion toUpdate = m_maskFull + maskFull;
        /* Remember new full mask: */
        m_maskFull = maskFull;
        /* Assign new full mask: */
        UIMachineWindow::setMask(m_maskFull);
        /* Update viewport region finally: */
        m_pMachineView->viewport()->update(toUpdate);
    }
}
#endif /* !VBOX_WITH_TRANSLUCENT_SEAMLESS */

