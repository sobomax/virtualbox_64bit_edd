/** @file
 *
 * VBox frontends: Qt GUI ("VirtualBox"):
 * UIMachineWindowSeamless class declaration
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

#ifndef __UIMachineWindowSeamless_h__
#define __UIMachineWindowSeamless_h__

/* Local includes: */
#include "UIMachineWindow.h"

/* Forward declarations: */
class UIRuntimeMiniToolBar;

/* Seamless machine-window implementation: */
class UIMachineWindowSeamless : public UIMachineWindow
{
    Q_OBJECT;

protected:

    /* Constructor: */
    UIMachineWindowSeamless(UIMachineLogic *pMachineLogic, ulong uScreenId);

private slots:

#ifndef Q_WS_MAC
    /* Session event-handlers: */
    void sltMachineStateChanged();
#endif /* !Q_WS_MAC */

    /* Popup main menu: */
    void sltPopupMainMenu();

    /** Revokes keyboard-focus. */
    void sltRevokeFocus();

#ifndef VBOX_WITH_TRANSLUCENT_SEAMLESS
# ifdef Q_WS_X11
    /** Assigns mini-toolbar seamless mask. */
    void sltUpdateMiniToolbarMask(const QRect &geo);
# endif /* Q_WS_X11 */
#endif /* !VBOX_WITH_TRANSLUCENT_SEAMLESS */

private:

    /* Prepare helpers: */
    void prepareMenu();
    void prepareVisualState();
#ifndef Q_WS_MAC
    void prepareMiniToolbar();
#endif /* !Q_WS_MAC */

    /* Cleanup helpers: */
#ifndef Q_WS_MAC
    void cleanupMiniToolbar();
#endif /* !Q_WS_MAC */
    void cleanupVisualState();
    void cleanupMenu();

    /* Show stuff: */
    void placeOnScreen();
    void showInNecessaryMode();

    /** Adjusts machine-view size to correspond current machine-window size. */
    virtual void adjustMachineViewSize();

#ifndef Q_WS_MAC
    /* Update routines: */
    void updateAppearanceOf(int iElement);
#endif /* !Q_WS_MAC */

#if defined(VBOX_WITH_TRANSLUCENT_SEAMLESS) && defined(Q_WS_WIN)
    /* Handler: Translucency stuff: */
    void showEvent(QShowEvent *pEvent);
#endif /* VBOX_WITH_TRANSLUCENT_SEAMLESS && Q_WS_WIN */

#ifndef VBOX_WITH_TRANSLUCENT_SEAMLESS
    /** Assigns guest seamless mask. */
    void setMask(const QRegion &maskGuest);
#endif /* !VBOX_WITH_TRANSLUCENT_SEAMLESS */

    /* Widgets: */
    QMenu *m_pMainMenu;
#ifndef Q_WS_MAC
    UIRuntimeMiniToolBar *m_pMiniToolBar;
#endif /* !Q_WS_MAC */

#ifndef VBOX_WITH_TRANSLUCENT_SEAMLESS
    /** Holds the full seamless mask. */
    QRegion m_maskFull;
    /** Holds the guest seamless mask. */
    QRegion m_maskGuest;
# ifdef Q_WS_X11
    /** Holds the mini-toolbar seamless mask. */
    QRect m_maskMiniToolbar;
# endif /* Q_WS_X11 */
#endif /* !VBOX_WITH_TRANSLUCENT_SEAMLESS */

    /* Factory support: */
    friend class UIMachineWindow;
};

#endif // __UIMachineWindowSeamless_h__

