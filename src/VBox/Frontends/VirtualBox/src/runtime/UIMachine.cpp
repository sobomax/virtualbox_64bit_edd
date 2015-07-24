/* $Id: UIMachine.cpp $ */
/** @file
 * VBox Qt GUI - UIMachine class implementation.
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

/* Local includes: */
#include "VBoxGlobal.h"
#include "UIMachine.h"
#include "UISession.h"
#include "UIActionPoolRuntime.h"
#include "UIMachineLogic.h"
#include "UIMachineWindow.h"

/* Visual state interface: */
class UIVisualState : public QObject
{
    Q_OBJECT;

public:

    /* Constructor: */
    UIVisualState(QObject *pParent, UISession *pSession, UIVisualStateType type)
        : QObject(pParent)
        , m_type(type)
        , m_pSession(pSession)
        , m_pMachineLogic(0)
    {
    }

    /* Destructor: */
    ~UIVisualState()
    {
        /* Cleanup/delete machine logic if exists: */
        if (m_pMachineLogic)
        {
            /* Cleanup the logic object: */
            m_pMachineLogic->cleanup();
            /* Destroy the logic object: */
            UIMachineLogic::destroy(m_pMachineLogic);
        }
    }

    /* Visual state type getter: */
    UIVisualStateType visualStateType() const { return m_type; }

    /* Machine logic getter: */
    UIMachineLogic* machineLogic() const { return m_pMachineLogic; }

    /* Method to prepare change one visual state to another: */
    bool prepareChange()
    {
        m_pMachineLogic = UIMachineLogic::create(this, m_pSession, visualStateType());
        return m_pMachineLogic->checkAvailability();
    }

    /* Method to change one visual state to another: */
    void change()
    {
        /* Prepare the logic object: */
        m_pMachineLogic->prepare();
    }

protected:

    /* Variables: */
    UIVisualStateType m_type;
    UISession *m_pSession;
    UIMachineLogic *m_pMachineLogic;
};

UIMachine::UIMachine(UIMachine **ppSelf, const CSession &session)
    : QObject(0)
    , m_ppThis(ppSelf)
    , initialStateType(UIVisualStateType_Normal)
    , m_session(session)
    , m_pSession(0)
    , m_pVisualState(0)
    , m_allowedVisualStateTypes(UIVisualStateType_Invalid)
{
    /* Store self pointer: */
    if (m_ppThis)
        *m_ppThis = this;

    /* Create UI session: */
    m_pSession = new UISession(this, m_session);

    /* Preventing application from closing in case of window(s) closed: */
    qApp->setQuitOnLastWindowClosed(false);

    /* Cache medium data only if really necessary: */
    vboxGlobal().startMediumEnumeration(false /* force start */);

    /* Load machine settings: */
    loadMachineSettings();

    /* Prepare async visual-state change handler: */
    qRegisterMetaType<UIVisualStateType>();
    connect(this, SIGNAL(sigRequestAsyncVisualStateChange(UIVisualStateType)),
            this, SLOT(sltChangeVisualState(UIVisualStateType)),
            Qt::QueuedConnection);

    /* Enter default (normal) state */
    enterInitialVisualState();
}

UIMachine::~UIMachine()
{
    /* Save machine settings: */
    saveMachineSettings();

    /* Delete visual state: */
    delete m_pVisualState;
    m_pVisualState = 0;

    /* Delete UI session: */
    delete m_pSession;
    m_pSession = 0;

    /* Free session finally: */
    m_session.UnlockMachine();
    m_session.detach();

    /* Clear self pointer: */
    *m_ppThis = 0;

    /* Quit application: */
    QApplication::quit();
}

QWidget* UIMachine::activeWindow() const
{
    /* Null if machine-logic not yet created: */
    if (!machineLogic())
        return 0;
    /* Active machine-window otherwise: */
    return machineLogic()->activeMachineWindow();
}

void UIMachine::asyncChangeVisualState(UIVisualStateType visualStateType)
{
    emit sigRequestAsyncVisualStateChange(visualStateType);
}

void UIMachine::sltChangeVisualState(UIVisualStateType newVisualStateType)
{
    /* Create new state: */
    UIVisualState *pNewVisualState = new UIVisualState(this, m_pSession, newVisualStateType);

    /* First we have to check if the selected mode is available at all.
     * Only then we delete the old mode and switch to the new mode. */
    if (pNewVisualState->prepareChange())
    {
        /* Delete previous state: */
        delete m_pVisualState;

        /* Set the new mode as current mode: */
        m_pVisualState = pNewVisualState;
        m_pVisualState->change();
    }
    else
    {
        /* Discard the temporary created new state: */
        delete pNewVisualState;

        /* If there is no state currently created => we have to exit: */
        if (!m_pVisualState)
            deleteLater();
    }
}

void UIMachine::enterInitialVisualState()
{
    sltChangeVisualState(initialStateType);
}

UIMachineLogic* UIMachine::machineLogic() const
{
    if (m_pVisualState && m_pVisualState->machineLogic())
        return m_pVisualState->machineLogic();
    return 0;
}

void UIMachine::loadMachineSettings()
{
    /* Load machine settings: */
    CMachine machine = uisession()->session().GetMachine();
    UIVisualStateType restrictedVisualStateTypes = VBoxGlobal::restrictedVisualStateTypes(machine);
    m_allowedVisualStateTypes = static_cast<UIVisualStateType>(UIVisualStateType_All ^ restrictedVisualStateTypes);

    /* Load extra-data settings: */
    {
        /* Machine while saving own settings will save "on" only for current
         * visual representation mode if its differs from normal mode of course.
         * But user can alter extra data manually in machine xml file and set there
         * more than one visual representation mode flags. Shame on such user!
         * There is no reason to enter in more than one visual representation mode
         * at machine start, so we are choosing first of requested modes: */
        bool fIsSomeExtendedModeChosen = false;

        if (!fIsSomeExtendedModeChosen)
        {
            /* Test 'scale' flag: */
            QString strScaleSettings = machine.GetExtraData(GUI_Scale);
            if (strScaleSettings == "on" && isVisualStateAllowedScale())
            {
                fIsSomeExtendedModeChosen = true;
                /* We can enter scale mode initially: */
                initialStateType = UIVisualStateType_Scale;
            }
        }

        if (!fIsSomeExtendedModeChosen)
        {
            /* Test 'seamless' flag: */
            QString strSeamlessSettings = machine.GetExtraData(GUI_Seamless);
            if (strSeamlessSettings == "on" && isVisualStateAllowedSeamless())
            {
                fIsSomeExtendedModeChosen = true;
                /* We can't enter seamless mode initially,
                 * so we should ask ui-session for that: */
                uisession()->setRequestedVisualState(UIVisualStateType_Seamless);
            }
        }

        if (!fIsSomeExtendedModeChosen)
        {
            /* Test 'fullscreen' flag: */
            QString strFullscreenSettings = machine.GetExtraData(GUI_Fullscreen);
            if (strFullscreenSettings == "on" && isVisualStateAllowedFullscreen())
            {
                fIsSomeExtendedModeChosen = true;
                /* We can enter fullscreen mode initially: */
                initialStateType = UIVisualStateType_Fullscreen;
            }
        }
    }
}

void UIMachine::saveMachineSettings()
{
    /* Save machine settings: */
    CMachine machine = uisession()->session().GetMachine();

    /* Save extra-data settings: */
    {
        /* Prepare extra-data values: */
        QString strFullscreenRequested;
        QString strSeamlessRequested;
        QString strScaleRequested;
        /* Check if some state was requested: */
        if (uisession()->requestedVisualState() != UIVisualStateType_Invalid)
        {
            switch (uisession()->requestedVisualState())
            {
                case UIVisualStateType_Fullscreen: strFullscreenRequested = "on"; break;
                case UIVisualStateType_Seamless: strSeamlessRequested = "on"; break;
                case UIVisualStateType_Scale: strScaleRequested = "on"; break;
                default: break;
            }
        }
        /* Check if some state still exists: */
        else if (m_pVisualState)
        {
            switch (m_pVisualState->visualStateType())
            {
                case UIVisualStateType_Fullscreen: strFullscreenRequested = "on"; break;
                case UIVisualStateType_Seamless: strSeamlessRequested = "on"; break;
                case UIVisualStateType_Scale: strScaleRequested = "on"; break;
                default: break;
            }
        }
        /* Rewrite extra-data values: */
        machine.SetExtraData(GUI_Fullscreen, strFullscreenRequested);
        machine.SetExtraData(GUI_Seamless, strSeamlessRequested);
        machine.SetExtraData(GUI_Scale, strScaleRequested);
    }
}

#include "UIMachine.moc"

