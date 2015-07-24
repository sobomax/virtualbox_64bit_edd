/** @file
 * VBox Qt GUI - UIMachineLogicFullscreen class declaration.
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

#ifndef ___UIMachineLogicFullscreen_h___
#define ___UIMachineLogicFullscreen_h___

/* GUI includes: */
#include "UIMachineLogic.h"

/* Other includes: */
#ifdef Q_WS_MAC
# include <ApplicationServices/ApplicationServices.h>
#endif /* Q_WS_MAC */

/* Forward declarations: */
class UIMultiScreenLayout;

/* Fullscreen machine logic implementation: */
class UIMachineLogicFullscreen : public UIMachineLogic
{
    Q_OBJECT;

#ifdef RT_OS_DARWIN
signals:

    /** Mac OS X: Notifies listeners about native fullscreen mode should be entered on @a pMachineWindow. */
    void sigNotifyAboutNativeFullscreenShouldBeEntered(UIMachineWindow *pMachineWindow = 0);
    /** Mac OS X: Notifies listeners about native fullscreen mode should be exited on @a pMachineWindow. */
    void sigNotifyAboutNativeFullscreenShouldBeExited(UIMachineWindow *pMachineWindow = 0);
#endif /* RT_OS_DARWIN */

#ifdef Q_WS_MAC
public:

    /** Returns whether screens have separate spaces. */
    bool screensHaveSeparateSpaces() const { return m_fScreensHaveSeparateSpaces; }
#endif /* Q_WS_MAC */

protected:

    /* Constructor/destructor: */
    UIMachineLogicFullscreen(QObject *pParent, UISession *pSession);
    ~UIMachineLogicFullscreen();

    /* Check if this logic is available: */
    bool checkAvailability();

    /** Returns machine-window flags for 'Fullscreen' machine-logic and passed @a uScreenId. */
    virtual Qt::WindowFlags windowFlags(ulong uScreenId) const { Q_UNUSED(uScreenId); return Qt::FramelessWindowHint; }

    /** Adjusts machine-window geometry if necessary for 'Fullscreen'. */
    virtual void adjustMachineWindowsGeometry();

    /* Helpers: Multi-screen stuff: */
    int hostScreenForGuestScreen(int iScreenId) const;
    bool hasHostScreenForGuestScreen(int iScreenId) const;

private slots:

#ifdef RT_OS_DARWIN
    /** Mac OS X: Handles native notification about 'fullscreen' will be entered. */
    void sltHandleNativeFullscreenWillEnter();
    /** Mac OS X: Handles native notification about 'fullscreen' entered. */
    void sltHandleNativeFullscreenDidEnter();
    /** Mac OS X: Handles native notification about 'fullscreen' will be exited. */
    void sltHandleNativeFullscreenWillExit();
    /** Mac OS X: Handles native notification about 'fullscreen' exited. */
    void sltHandleNativeFullscreenDidExit();
    /** Mac OS X: Handles native notification about 'fullscreen' fail to enter. */
    void sltHandleNativeFullscreenFailToEnter();

    /** Mac OS X: Requests visual-state change from 'fullscreen' to 'normal' (window). */
    void sltChangeVisualStateToNormal();
    /** Mac OS X: Requests visual-state change from 'fullscreen' to 'seamless'. */
    void sltChangeVisualStateToSeamless();
    /** Mac OS X: Requests visual-state change from 'fullscreen' to 'scale'. */
    void sltChangeVisualStateToScale();

    /** Mac OS X: Checks if some visual-state type was requested. */
    void sltCheckForRequestedVisualStateType();
#endif /* RT_OS_DARWIN */

    /* Handler: Console callback stuff: */
    void sltMachineStateChanged();

#ifdef RT_OS_DARWIN
    void sltChangePresentationMode(bool fEnabled);
#endif /* RT_OS_DARWIN */

    /** Updates machine-window(s) location/size on screen-layout changes. */
    void sltScreenLayoutChanged();

    /** Handles guest-screen count change. */
    virtual void sltGuestMonitorChange(KGuestMonitorChangedEventType changeType, ulong uScreenId, QRect screenGeo);
    /** Handles host-screen count change. */
    virtual void sltHostScreenCountChange();
    /** Handles host-screen available-area change. */
    virtual void sltHostScreenAvailableAreaChange();

private:

    /* Prepare helpers: */
    void prepareActionGroups();
    void prepareActionConnections();
#ifdef Q_WS_MAC
    void prepareOtherConnections();
#endif /* Q_WS_MAC */
    void prepareMachineWindows();
    void prepareMenu();

    /* Cleanup helpers: */
    //void cleanupMenu() {}
    void cleanupMachineWindows();
#ifdef Q_WS_MAC
    //void cleanupOtherConnections() {}
#endif /* Q_WS_MAC */
    void cleanupActionConnections();
    void cleanupActionGroups();

#ifdef Q_WS_MAC
    void setPresentationModeEnabled(bool fEnabled);

    /** Mac OS X: Performs fade to black if possible. */
    void fadeToBlack();
    /** Mac OS X: Performs fade to normal if possible. */
    void fadeToNormal();

    /** Mac OS X: Revalidates 'fullscreen' mode for @a pMachineWindow. */
    void revalidateNativeFullScreen(UIMachineWindow *pMachineWindow);
    /** Mac OS X: Revalidates 'fullscreen' mode for all windows. */
    void revalidateNativeFullScreen();

    /** Mac OS X: Proxies native notification about application activation. */
    static void nativeHandlerForApplicationActivation(QObject *pObject, const QMap<QString, QString> &userInfo);
    /** Mac OS X: Handles native notification about application activation. */
    void nativeHandlerForApplicationActivation(const QMap<QString, QString> &userInfo);

    /** Mac OS X: Proxies native notification about active space change. */
    static void nativeHandlerForActiveSpaceChange(QObject *pObject, const QMap<QString, QString> &userInfo);
    /** Mac OS X: Handles native notification about active space change. */
    void nativeHandlerForActiveSpaceChange(const QMap<QString, QString> &userInfo);
#endif /* Q_WS_MAC */

    /* Variables: */
    UIMultiScreenLayout *m_pScreenLayout;

#ifdef Q_WS_MAC
    /** Mac OS X: Holds whether screens have separate spaces. */
    const bool m_fScreensHaveSeparateSpaces;

    /** Mac OS X: Fade token. */
    CGDisplayFadeReservationToken m_fadeToken;

    /** Mac OS X: Contains machine-window(s) marked as 'fullscreen'. */
    QSet<UIMachineWindow*> m_fullscreenMachineWindows;
    /** Mac OS X: Contains machine-window(s) marked as 'invalid fullscreen'. */
    QSet<UIMachineWindow*> m_invalidFullscreenMachineWindows;
#endif /* Q_WS_MAC */

    /* Friend classes: */
    friend class UIMachineLogic;
    friend class UIMachineWindowFullscreen;
    friend class UIMachineViewFullscreen;
};

#endif /* !___UIMachineLogicFullscreen_h___ */

