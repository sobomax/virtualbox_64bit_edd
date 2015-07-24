/* $Id: UIActionPool.cpp $ */
/** @file
 *
 * VBox frontends: Qt GUI ("VirtualBox"):
 * UIActionPool class implementation
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

/* Global includes: */
#include <QHelpEvent>
#include <QToolTip>

/* Local includes: */
#include "UIActionPool.h"
#include "UIActionPoolSelector.h"
#include "UIActionPoolRuntime.h"
#include "UIIconPool.h"
#include "UIShortcutPool.h"
#include "VBoxGlobal.h"

/* Action activation event: */
class ActivateActionEvent : public QEvent
{
public:

    ActivateActionEvent(QAction *pAction)
        : QEvent((QEvent::Type)ActivateActionEventType)
        , m_pAction(pAction) {}
    QAction* action() const { return m_pAction; }

private:

    QAction *m_pAction;
};

/* UIAction stuff: */
UIAction::UIAction(UIActionPool *pParent, UIActionType type)
    : QAction(pParent)
    , m_pActionPool(pParent)
    , m_type(type)
    , m_actionPoolType(pParent->type())
    , m_fShortcutHidden(false)
{
    /* By default there is no specific menu role.
     * It will be set explicitly later. */
    setMenuRole(QAction::NoRole);
}

UIActionState* UIAction::toStateAction()
{
    return qobject_cast<UIActionState*>(this);
}

void UIAction::setName(const QString &strName)
{
    /* Remember internal name: */
    m_strName = strName;
    /* Update text according new name: */
    updateText();
}

void UIAction::setShortcut(const QKeySequence &shortcut)
{
    /* Only for selector's action-pool: */
    if (m_actionPoolType == UIActionPoolType_Selector)
    {
        /* If shortcut is visible: */
        if (!m_fShortcutHidden)
            /* Call to base-class: */
            QAction::setShortcut(shortcut);
        /* Remember shortcut: */
        m_shortcut = shortcut;
    }
    /* Update text according new shortcut: */
    updateText();
}

void UIAction::showShortcut()
{
    m_fShortcutHidden = false;
    if (!m_shortcut.isEmpty())
        QAction::setShortcut(m_shortcut);
}

void UIAction::hideShortcut()
{
    m_fShortcutHidden = true;
    if (!shortcut().isEmpty())
        QAction::setShortcut(QKeySequence());
}

QString UIAction::nameInMenu() const
{
    /* Action-name format depends on action-pool type: */
    switch (m_actionPoolType)
    {
        /* Unchanged name for Selector UI: */
        case UIActionPoolType_Selector: return name();
        /* Filtered name for Runtime UI: */
        case UIActionPoolType_Runtime: return VBoxGlobal::removeAccelMark(name());
    }
    /* Nothing by default: */
    return QString();
}

void UIAction::updateText()
{
    /* Action-text format depends on action-pool type: */
    switch (m_actionPoolType)
    {
        /* The same as menu name for Selector UI: */
        case UIActionPoolType_Selector:
            setText(nameInMenu());
            break;
        /* With shortcut appended for Runtime UI: */
        case UIActionPoolType_Runtime:
            setText(vboxGlobal().insertKeyToActionText(nameInMenu(),
                                                       gShortcutPool->shortcut(actionPool(), this).toString()));
            break;
    }
}

/* UIMenu stuff: */
UIMenu::UIMenu()
    : m_fShowToolTips(false)
{
}

bool UIMenu::event(QEvent *pEvent)
{
    /* Handle particular event-types: */
    switch (pEvent->type())
    {
        /* Tool-tip request handler: */
        case QEvent::ToolTip:
        {
            /* Get current help-event: */
            QHelpEvent *pHelpEvent = static_cast<QHelpEvent*>(pEvent);
            /* Get action which caused help-event: */
            QAction *pAction = actionAt(pHelpEvent->pos());
            /* If action present => show action's tool-tip if needed: */
            if (pAction && m_fShowToolTips)
                QToolTip::showText(pHelpEvent->globalPos(), pAction->toolTip());
            break;
        }
        default:
            break;
    }
    /* Base-class event-handler: */
    return QMenu::event(pEvent);
}

/* UIActionSimple stuff: */
UIActionSimple::UIActionSimple(UIActionPool *pParent,
                               const QString &strIcon, const QString &strIconDis)
    : UIAction(pParent, UIActionType_Simple)
{
    if (!strIcon.isNull())
        setIcon(UIIconPool::iconSet(strIcon, strIconDis));
}

UIActionSimple::UIActionSimple(UIActionPool *pParent,
                               const QSize &normalSize, const QSize &smallSize,
                               const QString &strNormalIcon, const QString &strSmallIcon,
                               const QString &strNormalIconDis, const QString &strSmallIconDis)
    : UIAction(pParent, UIActionType_Simple)
{
    setIcon(UIIconPool::iconSetFull(normalSize, smallSize, strNormalIcon, strSmallIcon, strNormalIconDis, strSmallIconDis));
}

UIActionSimple::UIActionSimple(UIActionPool *pParent,
                               const QIcon& icon)
    : UIAction(pParent, UIActionType_Simple)
{
    if (!icon.isNull())
        setIcon(icon);
}

/* UIActionState stuff: */
UIActionState::UIActionState(UIActionPool *pParent,
                             const QString &strIcon, const QString &strIconDis)
    : UIAction(pParent, UIActionType_State)
    , m_iState(0)
{
    if (!strIcon.isNull())
        setIcon(UIIconPool::iconSet(strIcon, strIconDis));
}

UIActionState::UIActionState(UIActionPool *pParent,
                             const QSize &normalSize, const QSize &smallSize,
                             const QString &strNormalIcon, const QString &strSmallIcon,
                             const QString &strNormalIconDis, const QString &strSmallIconDis)
    : UIAction(pParent, UIActionType_State)
    , m_iState(0)
{
    setIcon(UIIconPool::iconSetFull(normalSize, smallSize, strNormalIcon, strSmallIcon, strNormalIconDis, strSmallIconDis));
}

UIActionState::UIActionState(UIActionPool *pParent,
                             const QIcon& icon)
    : UIAction(pParent, UIActionType_State)
    , m_iState(0)
{
    if (!icon.isNull())
        setIcon(icon);
}

/* UIActionToggle stuff: */
UIActionToggle::UIActionToggle(UIActionPool *pParent,
                               const QString &strIcon, const QString &strIconDis)
    : UIAction(pParent, UIActionType_Toggle)
{
    if (!strIcon.isNull())
        setIcon(UIIconPool::iconSet(strIcon, strIconDis));
    init();
}

UIActionToggle::UIActionToggle(UIActionPool *pParent,
                               const QSize &normalSize, const QSize &smallSize,
                               const QString &strNormalIcon, const QString &strSmallIcon,
                               const QString &strNormalIconDis, const QString &strSmallIconDis)
    : UIAction(pParent, UIActionType_Toggle)
{
    setIcon(UIIconPool::iconSetFull(normalSize, smallSize, strNormalIcon, strSmallIcon, strNormalIconDis, strSmallIconDis));
    init();
}

UIActionToggle::UIActionToggle(UIActionPool *pParent,
               const QString &strIconOn, const QString &strIconOff,
               const QString &strIconOnDis, const QString &strIconOffDis)
    : UIAction(pParent, UIActionType_Toggle)
{
    setIcon(UIIconPool::iconSetOnOff(strIconOn, strIconOff, strIconOnDis, strIconOffDis));
    init();
}

UIActionToggle::UIActionToggle(UIActionPool *pParent,
                               const QIcon &icon)
    : UIAction(pParent, UIActionType_Toggle)
{
    if (!icon.isNull())
        setIcon(icon);
    init();
}

void UIActionToggle::sltUpdate()
{
    retranslateUi();
}

void UIActionToggle::init()
{
    setCheckable(true);
    connect(this, SIGNAL(toggled(bool)), this, SLOT(sltUpdate()));
}

/* UIActionMenu stuff: */
UIActionMenu::UIActionMenu(UIActionPool *pParent,
                           const QString &strIcon, const QString &strIconDis)
    : UIAction(pParent, UIActionType_Menu)
{
    if (!strIcon.isNull())
        setIcon(UIIconPool::iconSet(strIcon, strIconDis));
    setMenu(new UIMenu);
}

UIActionMenu::UIActionMenu(UIActionPool *pParent,
                           const QIcon &icon)
    : UIAction(pParent, UIActionType_Menu)
{
    if (!icon.isNull())
        setIcon(icon);
    setMenu(new UIMenu);
}

void UIActionMenu::updateText()
{
    setText(nameInMenu());
}


class UIActionSimpleLogDialog : public UIActionSimple
{
    Q_OBJECT;

public:

    UIActionSimpleLogDialog(UIActionPool *pParent)
        : UIActionSimple(pParent, QSize(32, 32), QSize(16, 16),
                         ":/vm_show_logs_32px.png", ":/vm_show_logs_16px.png",
                         ":/vm_show_logs_disabled_32px.png", ":/vm_show_logs_disabled_16px.png")
    {
        retranslateUi();
    }

protected:

    QString shortcutExtraDataID() const
    {
        return QString("ShowVMLog");
    }

    QKeySequence defaultShortcut(UIActionPoolType actionPoolType) const
    {
        switch (actionPoolType)
        {
            case UIActionPoolType_Selector: return QKeySequence("Ctrl+L");
            case UIActionPoolType_Runtime: break;
        }
        return QKeySequence();
    }

    void retranslateUi()
    {
        setName(QApplication::translate("UIActionPool", "Show &Log..."));
        setStatusTip(QApplication::translate("UIActionPool", "Show the log files of the selected virtual machine"));
    }
};

class UIActionMenuHelp : public UIActionMenu
{
    Q_OBJECT;

public:

    UIActionMenuHelp(UIActionPool *pParent)
        : UIActionMenu(pParent)
    {
        retranslateUi();
    }

protected:

    void retranslateUi()
    {
        setName(QApplication::translate("UIActionPool", "&Help"));
    }
};

class UIActionSimpleContents : public UIActionSimple
{
    Q_OBJECT;

public:

    UIActionSimpleContents(UIActionPool *pParent)
        : UIActionSimple(pParent, UIIconPool::defaultIcon(UIIconPool::DialogHelpIcon))
    {
        retranslateUi();
    }

protected:

    QString shortcutExtraDataID() const
    {
        return QString("Help");
    }

    QKeySequence defaultShortcut(UIActionPoolType actionPoolType) const
    {
        switch (actionPoolType)
        {
            case UIActionPoolType_Selector: return QKeySequence(QKeySequence::HelpContents);
            case UIActionPoolType_Runtime: break;
        }
        return QKeySequence();
    }

    void retranslateUi()
    {
        setName(QApplication::translate("UIActionPool", "&Contents..."));
        setStatusTip(QApplication::translate("UIActionPool", "Show help contents"));
    }
};

class UIActionSimpleWebSite : public UIActionSimple
{
    Q_OBJECT;

public:

    UIActionSimpleWebSite(UIActionPool *pParent)
        : UIActionSimple(pParent, ":/site_16px.png")
    {
        retranslateUi();
    }

protected:

    QString shortcutExtraDataID() const
    {
        return QString("Web");
    }

    void retranslateUi()
    {
        setName(QApplication::translate("UIActionPool", "&VirtualBox Web Site..."));
        setStatusTip(QApplication::translate("UIActionPool", "Open the browser and go to the VirtualBox product web site"));
    }
};

class UIActionSimpleResetWarnings : public UIActionSimple
{
    Q_OBJECT;

public:

    UIActionSimpleResetWarnings(UIActionPool *pParent)
        : UIActionSimple(pParent, ":/reset_warnings_16px.png")
    {
        retranslateUi();
    }

protected:

    QString shortcutExtraDataID() const
    {
        return QString("ResetWarnings");
    }

    void retranslateUi()
    {
        setName(QApplication::translate("UIActionPool", "&Reset All Warnings"));
        setStatusTip(QApplication::translate("UIActionPool", "Go back to showing all suppressed warnings and messages"));
    }
};

#ifdef VBOX_GUI_WITH_NETWORK_MANAGER
class UIActionSimpleNetworkAccessManager : public UIActionSimple
{
    Q_OBJECT;

public:

    UIActionSimpleNetworkAccessManager(UIActionPool *pParent)
        : UIActionSimple(pParent, ":/nw_16px.png", ":/nw_disabled_16px.png")
    {
        retranslateUi();
    }

protected:

    QString shortcutExtraDataID() const
    {
        return QString("NetworkAccessManager");
    }

    void retranslateUi()
    {
        setName(QApplication::translate("UIActionPool", "&Network Operations Manager..."));
        setStatusTip(QApplication::translate("UIActionPool", "Show Network Operations Manager"));
    }
};

class UIActionSimpleCheckForUpdates : public UIActionSimple
{
    Q_OBJECT;

public:

    UIActionSimpleCheckForUpdates(UIActionPool *pParent)
        : UIActionSimple(pParent, ":/refresh_16px.png", ":/refresh_disabled_16px.png")
    {
        setMenuRole(QAction::ApplicationSpecificRole);
        retranslateUi();
    }

protected:

    QString shortcutExtraDataID() const
    {
        return QString("Update");
    }

    void retranslateUi()
    {
        setName(QApplication::translate("UIActionPool", "C&heck for Updates..."));
        setStatusTip(QApplication::translate("UIActionPool", "Check for a new VirtualBox version"));
    }
};
#endif /* VBOX_GUI_WITH_NETWORK_MANAGER */

class UIActionSimpleAbout : public UIActionSimple
{
    Q_OBJECT;

public:

    UIActionSimpleAbout(UIActionPool *pParent)
        : UIActionSimple(pParent, ":/about_16px.png")
    {
        setMenuRole(QAction::AboutRole);
        retranslateUi();
    }

protected:

    QString shortcutExtraDataID() const
    {
        return QString("About");
    }

    void retranslateUi()
    {
        setName(QApplication::translate("UIActionPool", "&About VirtualBox..."));
        setStatusTip(QApplication::translate("UIActionPool", "Show a window with product information"));
    }
};


/* UIActionPool stuff: */
UIActionPool* UIActionPool::m_pInstance = 0;

/* static */
UIActionPool* UIActionPool::instance()
{
    return m_pInstance;
}

/* static */
void UIActionPool::create(UIActionPoolType type)
{
    /* Check that instance do NOT exists: */
    if (m_pInstance)
        return;

    /* Create instance: */
    switch (type)
    {
        case UIActionPoolType_Selector: new UIActionPoolSelector; break;
        case UIActionPoolType_Runtime: new UIActionPoolRuntime; break;
        default: break;
    }

    /* Prepare instance: */
    m_pInstance->prepare();
}

/* static */
void UIActionPool::destroy()
{
    /* Check that instance exists: */
    if (!m_pInstance)
        return;

    /* Cleanup instance: */
    m_pInstance->cleanup();

    /* Delete instance: */
    delete m_pInstance;
}

/* static */
void UIActionPool::createTemporary(UIActionPoolType type)
{
    UIActionPool *pHelperPool = 0;
    switch (type)
    {
        case UIActionPoolType_Selector: pHelperPool = new UIActionPoolSelector; break;
        case UIActionPoolType_Runtime: pHelperPool = new UIActionPoolRuntime; break;
        default: break;
    }
    if (pHelperPool)
    {
        pHelperPool->prepare();
        pHelperPool->cleanup();
        delete pHelperPool;
    }
}

UIActionPool::UIActionPool(UIActionPoolType type)
    : m_type(type)
{
    /* Prepare instance: */
    if (!m_pInstance)
        m_pInstance = this;
}

UIActionPool::~UIActionPool()
{
    /* Cleanup instance: */
    if (m_pInstance == this)
        m_pInstance = 0;
}

void UIActionPool::prepare()
{
    /* Create actions: */
    createActions();
    /* Create menus: */
    createMenus();
    /* Apply shortcuts: */
    sltApplyShortcuts();
}

void UIActionPool::cleanup()
{
    /* Destroy pool: */
    destroyPool();
}

bool UIActionPool::processHotKey(const QKeySequence &key)
{
    /* Get the list of keys: */
    QList<int> keys = m_pool.keys();
    /* Iterate through the whole list of keys: */
    for (int i = 0; i < keys.size(); ++i)
    {
        /* Get current action: */
        UIAction *pAction = m_pool[keys[i]];
        /* Skip menus/separators: */
        if (pAction->type() == UIActionType_Menu)
            continue;
        /* Get the hot key of the current action: */
        QString strHotKey = VBoxGlobal::extractKeyFromActionText(pAction->text());
        if (pAction->isEnabled() && pAction->isVisible() && !strHotKey.isEmpty())
        {
            if (key.matches(QKeySequence(strHotKey)) == QKeySequence::ExactMatch)
            {
                /* We asynchronously post a special event instead of calling
                 * pAction->trigger() directly, to let key presses and
                 * releases be processed correctly by Qt first.
                 * Note: we assume that nobody will delete the menu item
                 * corresponding to the key sequence, so that the pointer to
                 * menu data posted along with the event will remain valid in
                 * the event handler, at least until the main window is closed. */
                QApplication::postEvent(this, new ActivateActionEvent(pAction));
                return true;
            }
        }
    }
    return false;
}

void UIActionPool::sltApplyShortcuts()
{
    gShortcutPool->applyShortcuts(this);
}

void UIActionPool::createActions()
{
    /* Various dialog actions: */
    m_pool[UIActionIndex_Simple_LogDialog] = new UIActionSimpleLogDialog(this);
    /* 'Help' actions: */
    m_pool[UIActionIndex_Simple_Contents] = new UIActionSimpleContents(this);
    m_pool[UIActionIndex_Simple_WebSite] = new UIActionSimpleWebSite(this);
    m_pool[UIActionIndex_Simple_ResetWarnings] = new UIActionSimpleResetWarnings(this);
#ifdef VBOX_GUI_WITH_NETWORK_MANAGER
    m_pool[UIActionIndex_Simple_NetworkAccessManager] = new UIActionSimpleNetworkAccessManager(this);
    m_pool[UIActionIndex_Simple_CheckForUpdates] = new UIActionSimpleCheckForUpdates(this);
#endif /* VBOX_GUI_WITH_NETWORK_MANAGER */
    m_pool[UIActionIndex_Simple_About] = new UIActionSimpleAbout(this);
}

void UIActionPool::createMenus()
{
    /* On Mac OS X, all QMenu's are consumed by Qt after they are added to another QMenu or a QMenuBar.
     * This means we have to recreate all QMenus when creating a new QMenuBar.
     * For simplicity we doing this on all platforms right now. */

    /* 'Help' menu: */
    if (m_pool[UIActionIndex_Menu_Help])
        delete m_pool[UIActionIndex_Menu_Help];
    m_pool[UIActionIndex_Menu_Help] = new UIActionMenuHelp(this);
}

void UIActionPool::destroyPool()
{
    /* Get the list of keys: */
    QList<int> keys = m_pool.keys();
    /* Delete all the items of the map: */
    for (int i = 0; i < keys.size(); ++i)
        delete m_pool[keys[i]];
}

bool UIActionPool::event(QEvent *pEvent)
{
    /* Depending on event-type: */
    switch (pEvent->type())
    {
        case ActivateActionEventType:
        {
            /* Process specific event: */
            ActivateActionEvent *pActionEvent = static_cast<ActivateActionEvent*>(pEvent);
            pActionEvent->action()->trigger();
            pEvent->accept();
            return true;
        }
        default:
            break;
    }
    /* Pass to the base-class: */
    return QObject::event(pEvent);
}

#include "UIActionPool.moc"

