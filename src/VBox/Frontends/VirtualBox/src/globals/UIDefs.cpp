/* $Id: UIDefs.cpp $ */
/** @file
 *
 * VBox frontends: Qt GUI ("VirtualBox"):
 * Global definitions and function implementations
 */

/*
 * Copyright (C) 2006-2012 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

/* GUI includes: */
#include "UIDefs.h"

/* Global definitions: */
const char* UIDefs::GUI_RenderMode = "GUI/RenderMode";
const char* UIDefs::GUI_LanguageId = "GUI/LanguageID";
const char* UIDefs::GUI_ExtPackName = "Oracle VM VirtualBox Extension Pack";
const char* UIDefs::GUI_PreventBetaWarning = "GUI/PreventBetaWarning";
const char* UIDefs::GUI_RecentFolderHD = "GUI/RecentFolderHD";
const char* UIDefs::GUI_RecentFolderCD = "GUI/RecentFolderCD";
const char* UIDefs::GUI_RecentFolderFD = "GUI/RecentFolderFD";
const char* UIDefs::GUI_RecentListHD = "GUI/RecentListHD";
const char* UIDefs::GUI_RecentListCD = "GUI/RecentListCD";
const char* UIDefs::GUI_RecentListFD = "GUI/RecentListFD";

/* Selector-window definitions: */
const char* UIDefs::GUI_Input_SelectorShortcuts = "GUI/Input/SelectorShortcuts";
const char* UIDefs::GUI_LastSelectorWindowPosition = "GUI/LastWindowPosition";
const char* UIDefs::GUI_SplitterSizes = "GUI/SplitterSizes";
const char* UIDefs::GUI_Toolbar = "GUI/Toolbar";
const char* UIDefs::GUI_Statusbar = "GUI/Statusbar";
const char* UIDefs::GUI_PreviewUpdate = "GUI/PreviewUpdate";
const char* UIDefs::GUI_DetailsPageBoxes = "GUI/DetailsPageBoxes";
const char* UIDefs::GUI_SelectorVMPositions = "GUI/SelectorVMPositions";
const char* UIDefs::GUI_LastVMSelected = "GUI/LastVMSelected";
const char* UIDefs::GUI_LastItemSelected = "GUI/LastItemSelected";
const char* UIDefs::GUI_GroupDefinitions = "GUI/GroupDefinitions";
const char* UIDefs::GUI_HideFromManager = "GUI/HideFromManager";
const char* UIDefs::GUI_PreventReconfiguration = "GUI/PreventReconfiguration";
const char* UIDefs::GUI_PreventSnapshotOperations = "GUI/PreventSnapshotOperations";
const char* UIDefs::GUI_HideDetails = "GUI/HideDetails";

/* Machine-window definitions: */
#ifndef Q_WS_MAC
const char* UIDefs::GUI_MachineWindowIcons = "GUI/MachineWindowIcons";
const char* UIDefs::GUI_MachineWindowNamePostfix = "GUI/MachineWindowNamePostfix";
#endif /* !Q_WS_MAC */
const char* UIDefs::GUI_RestrictedRuntimeMenus = "GUI/RestrictedRuntimeMenus";
#ifdef Q_WS_MAC
const char* UIDefs::GUI_RestrictedRuntimeApplicationMenuActions = "GUI/RestrictedRuntimeApplicationMenuActions";
#endif /* Q_WS_MAC */
const char* UIDefs::GUI_RestrictedRuntimeMachineMenuActions = "GUI/RestrictedRuntimeMachineMenuActions";
const char* UIDefs::GUI_RestrictedRuntimeViewMenuActions = "GUI/RestrictedRuntimeViewMenuActions";
const char* UIDefs::GUI_RestrictedRuntimeDevicesMenuActions = "GUI/RestrictedRuntimeDevicesMenuActions";
#ifdef VBOX_WITH_DEBUGGER_GUI
const char* UIDefs::GUI_RestrictedRuntimeDebuggerMenuActions = "GUI/RestrictedRuntimeDebuggerMenuActions";
#endif /* VBOX_WITH_DEBUGGER_GUI */
const char* UIDefs::GUI_RestrictedRuntimeHelpMenuActions = "GUI/RestrictedRuntimeHelpMenuActions";
const char* UIDefs::GUI_RestrictedVisualStates = "GUI/RestrictedVisualStates";
const char* UIDefs::GUI_Input_MachineShortcuts = "GUI/Input/MachineShortcuts";
const char* UIDefs::GUI_LastNormalWindowPosition = "GUI/LastNormalWindowPosition";
const char* UIDefs::GUI_LastScaleWindowPosition = "GUI/LastScaleWindowPosition";
const char* UIDefs::GUI_LastWindowState_Max = "max";
const char* UIDefs::GUI_LastGuestSizeHint = "GUI/LastGuestSizeHint";
const char* UIDefs::GUI_LastGuestSizeHintWasFullscreen = "GUI/LastGuestSizeHintWasFullscreen";
const char* UIDefs::GUI_Fullscreen = "GUI/Fullscreen";
const char* UIDefs::GUI_Seamless = "GUI/Seamless";
const char* UIDefs::GUI_Scale = "GUI/Scale";
#ifdef Q_WS_X11
const char* UIDefs::GUI_Fullscreen_LegacyMode = "GUI/Fullscreen/LegacyMode";
#endif /* Q_WS_X11 */
const char* UIDefs::GUI_VirtualScreenToHostScreen = "GUI/VirtualScreenToHostScreen";
const char* UIDefs::GUI_AutoresizeGuest = "GUI/AutoresizeGuest";
const char* UIDefs::GUI_AutomountGuestScreens = "GUI/AutomountGuestScreens";
const char* UIDefs::GUI_SaveMountedAtRuntime = "GUI/SaveMountedAtRuntime";
const char* UIDefs::GUI_PassCAD = "GUI/PassCAD";
const char* UIDefs::GUI_DefaultCloseAction = "GUI/DefaultCloseAction";
const char* UIDefs::GUI_RestrictedStatusBarIndicators = "GUI/RestrictedStatusBarIndicators";
const char* UIDefs::GUI_HidLedsSync = "GUI/HidLedsSync";
const char* UIDefs::GUI_MouseCapturePolicy = "GUI/MouseCapturePolicy";
const char* UIDefs::GUI_GuruMeditationHandler = "GUI/GuruMeditationHandler";
const char* UIDefs::GUI_HiDPIOptimization = "GUI/HiDPI/Optimization";

/* Settings dialogs stuff: */
const char* UIDefs::GUI_RestrictedGlobalSettingsPages = "GUI/RestrictedGlobalSettingsPages";
const char* UIDefs::GUI_RestrictedMachineSettingsPages = "GUI/RestrictedMachineSettingsPages";

/* Settings dialogs: Display page stuff: */
const char* UIDefs::GUI_ActivateHoveredMachineWindow = "GUI/ActivateHoveredMachineWindow";

/* Mini tool-bar definitions: */
const char* UIDefs::GUI_ShowMiniToolBar = "GUI/ShowMiniToolBar";
const char* UIDefs::GUI_MiniToolBarAlignment = "GUI/MiniToolBarAlignment";
const char* UIDefs::GUI_MiniToolBarAutoHide = "GUI/MiniToolBarAutoHide";

/* Close-dialog definitions: */
const char* UIDefs::GUI_RestrictedCloseActions = "GUI/RestrictedCloseActions";
const char* UIDefs::GUI_LastCloseAction = "GUI/LastCloseAction";
const char* UIDefs::GUI_CloseActionHook = "GUI/CloseActionHook";

/* Wizards definitions: */
const char* UIDefs::GUI_FirstRun = "GUI/FirstRun";
const char* UIDefs::GUI_HideDescriptionForWizards = "GUI/HideDescriptionForWizards";
const char* UIDefs::GUI_Export_StorageType = "GUI/Export/StorageType";
const char* UIDefs::GUI_Export_Username = "GUI/Export/Username";
const char* UIDefs::GUI_Export_Hostname = "GUI/Export/Hostname";
const char* UIDefs::GUI_Export_Bucket = "GUI/Export/Bucket";

/* Message-center definitions: */
const char* UIDefs::GUI_SuppressMessages = "GUI/SuppressMessages";
const char* UIDefs::GUI_InvertMessageOption = "GUI/InvertMessageOption";

#ifdef VBOX_GUI_WITH_NETWORK_MANAGER
/* Update manager definitions: */
const char* UIDefs::GUI_PreventApplicationUpdate = "GUI/PreventApplicationUpdate";
const char* UIDefs::GUI_UpdateDate = "GUI/UpdateDate";
const char* UIDefs::GUI_UpdateCheckCount = "GUI/UpdateCheckCount";
#endif /* VBOX_GUI_WITH_NETWORK_MANAGER */

/* Information dialog definitions: */
const char* UIDefs::GUI_InfoDlgState = "GUI/InfoDlgState";

#ifdef VBOX_WITH_DEBUGGER_GUI
/* Debugger GUI declarations: */
const char* UIDefs::GUI_DbgEnabled = "GUI/Dbg/Enabled";
const char* UIDefs::GUI_DbgAutoShow = "GUI/Dbg/AutoShow";
#endif /* VBOX_WITH_DEBUGGER_GUI */

#ifdef Q_WS_X11
/* License GUI declarations: */
const char* UIDefs::GUI_LicenseKey = "GUI/LicenseAgreed";
#endif /* Q_WS_X11 */

#ifdef Q_WS_MAC
/* Mac dock declarations: */
const char* UIDefs::GUI_RealtimeDockIconUpdateEnabled = "GUI/RealtimeDockIconUpdateEnabled";
const char* UIDefs::GUI_RealtimeDockIconUpdateMonitor = "GUI/RealtimeDockIconUpdateMonitor";
const char* UIDefs::GUI_PresentationModeEnabled = "GUI/PresentationModeEnabled";
#endif /* Q_WS_MAC */

#ifdef VBOX_WITH_VIDEOHWACCEL
/* Video-acceleration declarations: */
const char* UIDefs::GUI_Accelerate2D_StretchLinear = "GUI/Accelerate2D/StretchLinear";
const char* UIDefs::GUI_Accelerate2D_PixformatYV12 = "GUI/Accelerate2D/PixformatYV12";
const char* UIDefs::GUI_Accelerate2D_PixformatUYVY = "GUI/Accelerate2D/PixformatUYVY";
const char* UIDefs::GUI_Accelerate2D_PixformatYUY2 = "GUI/Accelerate2D/PixformatYUY2";
const char* UIDefs::GUI_Accelerate2D_PixformatAYUV = "GUI/Accelerate2D/PixformatAYUV";
#endif /* VBOX_WITH_VIDEOHWACCEL */

/* File extensions definitions: */
QStringList UIDefs::VBoxFileExts = QStringList() << "xml" << "vbox";
QStringList UIDefs::VBoxExtPackFileExts = QStringList() << "vbox-extpack";
QStringList UIDefs::OVFFileExts = QStringList() << "ovf" << "ova";

