/** @file
 * VBox Qt GUI - UIMachineLogicSeamless class declaration.
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

#ifndef ___UIMachineLogicSeamless_h___
#define ___UIMachineLogicSeamless_h___

/* Local includes: */
#include "UIMachineLogic.h"

/* Forward declarations: */
class UIMultiScreenLayout;

/* Seamless machine logic implementation: */
class UIMachineLogicSeamless : public UIMachineLogic
{
    Q_OBJECT;

protected:

    /* Constructor/destructor: */
    UIMachineLogicSeamless(QObject *pParent, UISession *pSession);
    ~UIMachineLogicSeamless();

    /* Check if this logic is available: */
    bool checkAvailability();

    /** Returns machine-window flags for 'Seamless' machine-logic and passed @a uScreenId. */
    virtual Qt::WindowFlags windowFlags(ulong uScreenId) const { Q_UNUSED(uScreenId); return Qt::FramelessWindowHint; }

    /** Adjusts machine-window geometry if necessary for 'Seamless'. */
    virtual void adjustMachineWindowsGeometry();

    /* Helpers: Multi-screen stuff: */
    int hostScreenForGuestScreen(int iScreenId) const;
    bool hasHostScreenForGuestScreen(int iScreenId) const;

    /* API: 3D overlay visibility stuff: */
    void notifyAbout3DOverlayVisibilityChange(bool fVisible);

private slots:

    /** Checks if some visual-state type was requested. */
    void sltCheckForRequestedVisualStateType();

    /* Handler: Console callback stuff: */
    void sltMachineStateChanged();

    /** Updates machine-window(s) location/size on screen-layout changes. */
    void sltScreenLayoutChanged();

    /** Handles guest-screen count change. */
    virtual void sltGuestMonitorChange(KGuestMonitorChangedEventType changeType, ulong uScreenId, QRect screenGeo);
    /** Handles host-screen count change. */
    virtual void sltHostScreenCountChange();

private:

    /* Prepare helpers: */
    void prepareActionGroups();
    void prepareActionConnections();
    void prepareMachineWindows();
    void prepareMenu();

    /* Cleanup helpers: */
    //void cleanupMenu() {}
    void cleanupMachineWindows();
    void cleanupActionConnections();
    void cleanupActionGroups();

    /* Variables: */
    UIMultiScreenLayout *m_pScreenLayout;

    /* Friend classes: */
    friend class UIMachineLogic;
    friend class UIMachineWindowSeamless;
    friend class UIMachineViewSeamless;
};

#endif /* !___UIMachineLogicSeamless_h___ */

