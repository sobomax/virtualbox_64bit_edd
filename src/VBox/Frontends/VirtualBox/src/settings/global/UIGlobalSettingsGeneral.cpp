/* $Id: UIGlobalSettingsGeneral.cpp $ */
/** @file
 *
 * VBox frontends: Qt4 GUI ("VirtualBox"):
 * UIGlobalSettingsGeneral class implementation
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

/* Qt includes: */
#include <QDir>

/* GUI includes: */
#include "UIGlobalSettingsGeneral.h"
#include "VBoxGlobal.h"

UIGlobalSettingsGeneral::UIGlobalSettingsGeneral()
{
    /* Apply UI decorations: */
    Ui::UIGlobalSettingsGeneral::setupUi(this);

#ifndef Q_WS_MAC
    m_pLabelPresentationMode->hide();
    m_pCheckBoxPresentationMode->hide();
#endif /* !Q_WS_MAC */
    /* Hide checkbox for now: */
    m_pLabelHostScreenSaver->hide();
    m_pCheckBoxHostScreenSaver->hide();

    /* Setup widgets: */
    m_pSelectorMachineFolder->setHomeDir(vboxGlobal().homeFolder());
    m_pSelectorVRDPLibName->setHomeDir(vboxGlobal().homeFolder());
    m_pSelectorVRDPLibName->setMode(VBoxFilePathSelectorWidget::Mode_File_Open);

    /* Apply language settings: */
    retranslateUi();
}

/* Load data to cache from corresponding external object(s),
 * this task COULD be performed in other than GUI thread: */
void UIGlobalSettingsGeneral::loadToCacheFrom(QVariant &data)
{
    /* Fetch data to properties & settings: */
    UISettingsPageGlobal::fetchData(data);

    /* Load to cache: */
    m_cache.m_strDefaultMachineFolder = m_properties.GetDefaultMachineFolder();
    m_cache.m_strVRDEAuthLibrary = m_properties.GetVRDEAuthLibrary();
#ifdef Q_WS_MAC
    m_cache.m_fPresentationModeEnabled = m_settings.presentationModeEnabled();
#endif /* Q_WS_MAC */
    m_cache.m_fHostScreenSaverDisabled = m_settings.hostScreenSaverDisabled();

    /* Upload properties & settings to data: */
    UISettingsPageGlobal::uploadData(data);
}

/* Load data to corresponding widgets from cache,
 * this task SHOULD be performed in GUI thread only: */
void UIGlobalSettingsGeneral::getFromCache()
{
    /* Fetch from cache: */
    m_pSelectorMachineFolder->setPath(m_cache.m_strDefaultMachineFolder);
    m_pSelectorVRDPLibName->setPath(m_cache.m_strVRDEAuthLibrary);
#ifdef Q_WS_MAC
    m_pCheckBoxPresentationMode->setChecked(m_cache.m_fPresentationModeEnabled);
#endif /* Q_WS_MAC */
    m_pCheckBoxHostScreenSaver->setChecked(m_cache.m_fHostScreenSaverDisabled);
}

/* Save data from corresponding widgets to cache,
 * this task SHOULD be performed in GUI thread only: */
void UIGlobalSettingsGeneral::putToCache()
{
    /* Upload to cache: */
    m_cache.m_strDefaultMachineFolder = m_pSelectorMachineFolder->path();
    m_cache.m_strVRDEAuthLibrary = m_pSelectorVRDPLibName->path();
#ifdef Q_WS_MAC
    m_cache.m_fPresentationModeEnabled = m_pCheckBoxPresentationMode->isChecked();
#endif /* Q_WS_MAC */
    m_cache.m_fHostScreenSaverDisabled = m_pCheckBoxHostScreenSaver->isChecked();
}

/* Save data from cache to corresponding external object(s),
 * this task COULD be performed in other than GUI thread: */
void UIGlobalSettingsGeneral::saveFromCacheTo(QVariant &data)
{
    /* Fetch data to properties & settings: */
    UISettingsPageGlobal::fetchData(data);

    /* Save from cache: */
    if (m_properties.isOk() && m_pSelectorMachineFolder->isModified())
        m_properties.SetDefaultMachineFolder(m_cache.m_strDefaultMachineFolder);
    if (m_properties.isOk() && m_pSelectorVRDPLibName->isModified())
        m_properties.SetVRDEAuthLibrary(m_cache.m_strVRDEAuthLibrary);
#ifdef Q_WS_MAC
    m_settings.setPresentationModeEnabled(m_cache.m_fPresentationModeEnabled);
#endif /* Q_WS_MAC */
    m_settings.setHostScreenSaverDisabled(m_cache.m_fHostScreenSaverDisabled);

    /* Upload properties & settings to data: */
    UISettingsPageGlobal::uploadData(data);
}

void UIGlobalSettingsGeneral::setOrderAfter(QWidget *pWidget)
{
    /* Configure navigation: */
    setTabOrder(pWidget, m_pSelectorMachineFolder);
    setTabOrder(m_pSelectorMachineFolder, m_pSelectorVRDPLibName);
    setTabOrder(m_pSelectorVRDPLibName, m_pCheckBoxPresentationMode);
    setTabOrder(m_pCheckBoxPresentationMode, m_pCheckBoxHostScreenSaver);
}

void UIGlobalSettingsGeneral::retranslateUi()
{
    /* Translate uic generated strings: */
    Ui::UIGlobalSettingsGeneral::retranslateUi(this);
}

