/** @file
 *
 * VBox frontends: Qt GUI ("VirtualBox"):
 * UIMachineViewNormal class declaration
 */

/*
 * Copyright (C) 2010-2011 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#ifndef ___UIMachineViewNormal_h___
#define ___UIMachineViewNormal_h___

/* Local includes */
#include "UIMachineView.h"

class UIMachineViewNormal : public UIMachineView
{
    Q_OBJECT;

protected:

    /* Normal machine-view constructor: */
    UIMachineViewNormal(  UIMachineWindow *pMachineWindow
                        , ulong uScreenId
#ifdef VBOX_WITH_VIDEOHWACCEL
                        , bool bAccelerate2DVideo
#endif
    );
    /* Normal machine-view destructor: */
    virtual ~UIMachineViewNormal();

private slots:

    /* Console callback handlers: */
    void sltAdditionsStateChanged();

private:

    /* Event handlers: */
    bool eventFilter(QObject *pWatched, QEvent *pEvent);

    /* Prepare helpers: */
    void prepareCommon();
    void prepareFilters();
    void prepareConsoleConnections();
    //void loadMachineViewSettings();

    /* Cleanup helpers: */
    void saveMachineViewSettings();
    //void cleanupConsoleConnections() {}
    //void cleanupFilters() {}
    //void cleanupCommon() {}

    /* Hidden setters: */
    void setGuestAutoresizeEnabled(bool bEnabled);

    /** Resends guest size-hint if necessary. */
    void maybeResendSizeHint();

    /** Adjusts guest-screen size to correspond current <i>machine-window</i> size. */
    void adjustGuestScreenSize();

    /* Private helpers: */
    QRect workingArea() const;
    QSize calculateMaxGuestSize() const;

    /* Private members: */
    bool m_bIsGuestAutoresizeEnabled : 1;

    /* Friend classes: */
    friend class UIMachineView;
};

#endif // !___UIMachineViewNormal_h___

