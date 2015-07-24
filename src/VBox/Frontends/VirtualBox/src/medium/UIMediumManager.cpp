/* $Id: UIMediumManager.cpp $ */
/** @file
 *
 * VBox frontends: Qt4 GUI ("VirtualBox"):
 * UIMediumManager class implementation
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

#ifdef VBOX_WITH_PRECOMPILED_HEADERS
# include "precomp.h"
#else  /* !VBOX_WITH_PRECOMPILED_HEADERS */

/* Qt includes: */
#include <QLabel>
#include <QProgressBar>
#include <QMenuBar>
#include <QHeaderView>
#include <QPushButton>
#include <QTimer>

/* GUI includes: */
#include "VBoxGlobal.h"
#include "UIMediumManager.h"
#include "UIWizardCloneVD.h"
#include "UIMessageCenter.h"
#include "UIToolBar.h"
#include "QILabel.h"
#include "UIIconPool.h"
#include "UIMediumTypeChangeDialog.h"
#include "UIMedium.h"

/* COM includes: */
#include "COMEnums.h"
#include "CMediumFormat.h"
#include "CStorageController.h"
#include "CMediumAttachment.h"

# ifdef Q_WS_MAC
#  include "UIWindowMenuManager.h"
# endif /* Q_WS_MAC */

#endif /* !VBOX_WITH_PRECOMPILED_HEADERS */


/* Medium-item: */
class UIMediumItem : public QTreeWidgetItem
{
public:

    /* Medium-item type: */
    enum { Type = QTreeWidgetItem::UserType + 1 };

    /* Constructor: Top-level item: */
    UIMediumItem(const UIMedium &medium, QTreeWidget *pParent)
        : QTreeWidgetItem(pParent, Type)
        , m_medium(medium)
    { refresh(); }

    /* Constructor: Child item: */
    UIMediumItem(const UIMedium &medium, UIMediumItem *pParent)
        : QTreeWidgetItem(pParent, Type)
        , m_medium(medium)
    { refresh(); }

    /* API: Refresh stuff: */
    void refreshAll()
    {
        m_medium.refresh();
        refresh();
    }

    /* API: Medium stuff: */
    const UIMedium& medium() const { return m_medium; }
    void setMedium(const UIMedium &medium)
    {
        m_medium = medium;
        refresh();
    }

    /* API: Medium-type stuff: */
    UIMediumType mediumType() const { return m_medium.type(); }

    /* API: State stuff: */
    KMediumState state() const { return m_medium.state(); }

    /* API: ID stuff: */
    QString id() const { return m_medium.id(); }

    /* API: Location stuff: */
    QString location() const { return m_medium.location(); }

    /* API: Hard-disk stuff: */
    QString hardDiskFormat() const { return m_medium.hardDiskFormat(); }
    QString hardDiskType() const { return m_medium.hardDiskType(); }

    /* API: Details stuff: */
    QString details() const { return m_medium.storageDetails(); }

    /* API: Tool-tip stuff: */
    QString toolTip() const { return m_medium.toolTip(); }

    /* API: Usage stuff: */
    QString usage() const { return m_medium.usage(); }
    bool isUsed() const { return m_medium.isUsed(); }
    bool isUsedInSnapshots() const { return m_medium.isUsedInSnapshots(); }

    /* API: Sorting stuff: */
    bool operator<(const QTreeWidgetItem &other) const
    {
        int column = treeWidget()->sortColumn();
        ULONG64 thisValue = vboxGlobal().parseSize(      text(column));
        ULONG64 thatValue = vboxGlobal().parseSize(other.text(column));
        return thisValue && thatValue ? thisValue < thatValue : QTreeWidgetItem::operator<(other);
    }

private:

    /* Helper: Refresh stuff: */
    void refresh()
    {
        /* Fill-in columns: */
        setIcon(0, m_medium.icon());
        setText(0, m_medium.name());
        setText(1, m_medium.logicalSize());
        setText(2, m_medium.size());
        /* All columns get the same tooltip: */
        QString strToolTip = m_medium.toolTip();
        for (int i = 0; i < treeWidget()->columnCount(); ++i)
            setToolTip(i, strToolTip);
    }

    /* Variables: */
    UIMedium m_medium;
};


/** Functor allowing to check if passed UIMediumItem is suitable by ID. */
class CheckIfSuitableByID : public CheckIfSuitableBy
{
public:
    /** Constructor. */
    CheckIfSuitableByID(const QString &strID) : m_strID(strID) {}

private:
    /** Determines whether passed UIMediumItem is suitable by ID. */
    bool isItSuitable(UIMediumItem *pItem) const { return pItem->id() == m_strID; }
    /** Holds the ID to compare to. */
    QString m_strID;
};

/** Functor allowing to check if passed UIMediumItem is suitable by @a state. */
class CheckIfSuitableByState : public CheckIfSuitableBy
{
public:
    /** Constructor. */
    CheckIfSuitableByState(KMediumState state) : m_state(state) {}

private:
    /** Determines whether passed UIMediumItem is suitable by @a state. */
    bool isItSuitable(UIMediumItem *pItem) const { return pItem->state() == m_state; }
    /** Holds the @a state to compare to. */
    KMediumState m_state;
};


/* Medium manager progress-bar: */
class UIEnumerationProgressBar : public QWidget
{
    Q_OBJECT;

public:

    /* Constructor: */
    UIEnumerationProgressBar(QWidget *pParent)
        : QWidget(pParent)
    {
        /* Prepare: */
        prepare();
    }

    /* API: Text stuff: */
    void setText(const QString &strText) { mText->setText(strText); }

    /* API: Value stuff: */
    int value() const { return m_pProgressBar->value(); }
    void setValue(int iValue) { m_pProgressBar->setValue(iValue); }
    void setMaximum(int iValue) { m_pProgressBar->setMaximum(iValue); }

private:

    /* Helper: Prepare stuff: */
    void prepare()
    {
        /* Create layout: */
        QHBoxLayout *pLayout = new QHBoxLayout(this);
        {
            /* Configure layout: */
            pLayout->setContentsMargins(0, 0, 0, 0);
            /* Create label: */
            mText = new QLabel;
            /* Create progress-bar: */
            m_pProgressBar = new QProgressBar;
            {
                /* Configure progress-bar: */
                m_pProgressBar->setTextVisible(false);
            }
            /* Add widgets into layout: */
            pLayout->addWidget(mText);
            pLayout->addWidget(m_pProgressBar);
        }
    }

    /* Widgets: */
    QLabel *mText;
    QProgressBar *m_pProgressBar;
};


/* static */
UIMediumManager* UIMediumManager::m_spInstance = 0;
UIMediumManager* UIMediumManager::instance() { return m_spInstance; }

UIMediumManager::UIMediumManager(QWidget *pCenterWidget, bool fRefresh /* = true*/)
    : QIWithRetranslateUI2<QIMainDialog>(0, Qt::Dialog)
    , m_pCenterWidget(pCenterWidget)
    , m_fRefresh(fRefresh)
    , m_fInaccessibleHD(false)
    , m_fInaccessibleCD(false)
    , m_fInaccessibleFD(false)
    , m_iconHD(UIIconPool::iconSet(":/hd_16px.png", ":/hd_disabled_16px.png"))
    , m_iconCD(UIIconPool::iconSet(":/cd_16px.png", ":/cd_disabled_16px.png"))
    , m_iconFD(UIIconPool::iconSet(":/fd_16px.png", ":/fd_disabled_16px.png"))
{
    /* Prepare: */
    prepare();
}

UIMediumManager::~UIMediumManager()
{
    /* Cleanup: */
    cleanup();

    /* Cleanup instance: */
    m_spInstance = 0;
}

/* static */
void UIMediumManager::showModeless(QWidget *pCenterWidget /* = 0*/, bool fRefresh /* = true*/)
{
    /* Prepare instance if not prepared: */
    if (!m_spInstance)
        m_spInstance = new UIMediumManager(pCenterWidget, fRefresh);

    /* Show instance: */
    m_spInstance->show();
    m_spInstance->setWindowState(m_spInstance->windowState() & ~Qt::WindowMinimized);
    m_spInstance->activateWindow();
}

void UIMediumManager::refreshAll()
{
    vboxGlobal().startMediumEnumeration();
}

void UIMediumManager::sltHandleMediumCreated(const QString &strMediumID)
{
    /* Search for corresponding medium: */
    UIMedium medium = vboxGlobal().medium(strMediumID);

    /* Ignore non-interesting mediums: */
    if ((medium.isNull()) || (medium.isHostDrive()))
        return;

    /* Ignore mediums (and their children) which are
     * marked as hidden or attached to hidden machines only: */
    if (UIMedium::isMediumAttachedToHiddenMachinesOnly(medium))
        return;

    /* Prepare medium-item: */
    UIMediumItem *pMediumItem = 0;
    switch (medium.type())
    {
        case UIMediumType_HardDisk:
        {
            pMediumItem = createHardDiskItem(mTwHD, medium);
            AssertReturnVoid(pMediumItem);
            if (pMediumItem->id() == m_strSelectedIdHD)
            {
                setCurrentItem(mTwHD, pMediumItem);
                m_strSelectedIdHD = QString();
            }
            break;
        }
        case UIMediumType_DVD:
        {
            pMediumItem = new UIMediumItem(medium, mTwCD);
            AssertReturnVoid(pMediumItem);
            if (pMediumItem->id() == m_strSelectedIdCD)
            {
                setCurrentItem(mTwCD, pMediumItem);
                m_strSelectedIdCD = QString();
            }
            break;
        }
        case UIMediumType_Floppy:
        {
            pMediumItem = new UIMediumItem(medium, mTwFD);
            AssertReturnVoid(pMediumItem);
            if (pMediumItem->id() == m_strSelectedIdFD)
            {
                setCurrentItem(mTwFD, pMediumItem);
                m_strSelectedIdFD = QString();
            }
            break;
        }
        default: AssertFailed();
    }
    AssertPtrReturnVoid(pMediumItem);

    /* Update tab-icons: */
    updateTabIcons(pMediumItem, ItemAction_Added);

    /* If the media enumeration process is not started we have to select the
     * newly added item as the current one for the case of new image was added or created */
    if (!vboxGlobal().isMediumEnumerationInProgress())
        setCurrentItem(treeWidget(medium.type()), pMediumItem);

    /* Update stuff if that was current-item added: */
    if (pMediumItem == currentTreeWidget()->currentItem())
        sltHandleCurrentItemChanged(pMediumItem);
}

void UIMediumManager::sltHandleMediumDeleted(const QString &strMediumID)
{
    /* Get tree/item: */
    QList<UIMediumType> types;
    types << UIMediumType_HardDisk << UIMediumType_DVD << UIMediumType_Floppy;
    QTreeWidget *pTree = 0;
    UIMediumItem *pMediumItem = 0;
    foreach (UIMediumType type, types)
    {
        pTree = treeWidget(type);
        pMediumItem = searchItem(pTree, CheckIfSuitableByID(strMediumID));
        if (pMediumItem)
            break;
    }
    if (!pMediumItem)
        return;

    /* Update tab-icons: */
    updateTabIcons(pMediumItem, ItemAction_Removed);

    /* We need to silently delete item without selecting
     * the new one because of complex selection mechanism
     * which could provoke a segfault choosing the new
     * one item during last item deletion routine. So blocking
     * the tree-view for the time of item removing. */
    pTree->blockSignals(true);
    delete pMediumItem;
    pTree->blockSignals(false);

    /* Set new current-item: */
    setCurrentItem(pTree, pTree->currentItem());
}

void UIMediumManager::sltHandleMediumEnumerationStart()
{
    /* Reset inaccessibility flags: */
    m_fInaccessibleHD =
        m_fInaccessibleCD =
            m_fInaccessibleFD = false;

    /* Load default tab-widget icons: */
    mTabWidget->setTabIcon(HDTab, m_iconHD);
    mTabWidget->setTabIcon(CDTab, m_iconCD);
    mTabWidget->setTabIcon(FDTab, m_iconFD);

    /* Repopulate all medium-items: */
    QList<QString> mediumIDs = vboxGlobal().mediumIDs();
    prepareToRefresh(mediumIDs.size());
    foreach (const QString &strMediumID, mediumIDs)
        sltHandleMediumCreated(strMediumID);

    /* Select the first item to be the current one
     * if the previous saved item was not selected yet. */
    if (!mTwHD->currentItem())
        if (QTreeWidgetItem *pItem = mTwHD->topLevelItem(0))
            setCurrentItem(mTwHD, pItem);
    if (!mTwCD->currentItem())
        if (QTreeWidgetItem *pItem = mTwCD->topLevelItem(0))
            setCurrentItem(mTwCD, pItem);
    if (!mTwFD->currentItem())
        if (QTreeWidgetItem *pItem = mTwFD->topLevelItem(0))
            setCurrentItem(mTwFD, pItem);

    /* Update current tab: */
    sltHandleCurrentTabChanged();
}

void UIMediumManager::sltHandleMediumEnumerated(const QString &strMediumID)
{
    /* Search for corresponding medium: */
    UIMedium medium = vboxGlobal().medium(strMediumID);

    /* Ignore non-interesting mediums: */
    if ((medium.isNull()) || (medium.isHostDrive()))
        return;

    /* Ignore mediums (and their children) which are
     * marked as hidden or attached to hidden machines only: */
    if (UIMedium::isMediumAttachedToHiddenMachinesOnly(medium))
        return;

    /* Search for corresponding medium-item: */
    UIMediumItem *pMediumItem = 0;
    switch (medium.type())
    {
        case UIMediumType_HardDisk: pMediumItem = searchItem(mTwHD, CheckIfSuitableByID(medium.id())); break;
        case UIMediumType_DVD:      pMediumItem = searchItem(mTwCD, CheckIfSuitableByID(medium.id())); break;
        case UIMediumType_Floppy:   pMediumItem = searchItem(mTwFD, CheckIfSuitableByID(medium.id())); break;
        default: AssertFailed();
    }

    /* If medium-item was not found it's time to create it: */
    if (!pMediumItem)
        return sltHandleMediumCreated(strMediumID);

    /* Update medium-item: */
    pMediumItem->setMedium(medium);

    /* Update tab-icons: */
    updateTabIcons(pMediumItem, ItemAction_Updated);

    /* Update stuff if that was current-item updated: */
    if (pMediumItem == currentTreeWidget()->currentItem())
        sltHandleCurrentItemChanged(pMediumItem);

    /* Advance progress-bar: */
    m_pProgressBar->setValue(m_pProgressBar->value() + 1);
}

void UIMediumManager::sltHandleMediumEnumerationFinish()
{
    /* Hide progress-bar: */
    m_pProgressBar->hide();

    /* Enable 'refresh' action: */
    m_pActionRefresh->setEnabled(true);

    /* Unset 'busy' cursor: */
    unsetCursor();

    /* Update current tab: */
    sltHandleCurrentTabChanged();
}

void UIMediumManager::sltCopyMedium()
{
    /* Get current-item: */
    UIMediumItem *pMediumItem = toMediumItem(currentTreeWidget()->currentItem());

    /* Show Clone VD wizard: */
    UISafePointerWizard pWizard = new UIWizardCloneVD(this, pMediumItem->medium().medium());
    pWizard->prepare();
    pWizard->exec();

    /* Delete if still exists: */
    if (pWizard)
        delete pWizard;
}

void UIMediumManager::sltModifyMedium()
{
    /* Get current-item: */
    UIMediumItem *pMediumItem = toMediumItem(currentTreeWidget()->currentItem());

    /* Show Modify VD dialog: */
    UISafePointerDialog pDialog = new UIMediumTypeChangeDialog(this, pMediumItem->id());
    if (pDialog->exec() == QDialog::Accepted)
    {
        /* Safe spot because if dialog is deleted inside ::exec() =>
         * returned result will be QDialog::Rejected. */
        pMediumItem->refreshAll();
        m_pTypePane->setText(pMediumItem->hardDiskType());
    }

    /* Delete if still exists: */
    if (pDialog)
        delete pDialog;
}

void UIMediumManager::sltRemoveMedium()
{
    /* Get current medium-item: */
    UIMediumItem *pMediumItem = toMediumItem(currentTreeWidget()->currentItem());
    AssertMsgReturnVoid(pMediumItem, ("Current item must not be null"));

    /* Remember ID/type as they may get lost after the closure/deletion: */
    QString strMediumID = pMediumItem->id();
    AssertReturnVoid(!strMediumID.isNull());
    UIMediumType type = pMediumItem->mediumType();

    /* Confirm medium removal: */
    if (!msgCenter().confirmMediumRemoval(pMediumItem->medium(), this))
        return;

    /* Prepare result: */
    COMResult result;
    switch (type)
    {
        case UIMediumType_HardDisk:
        {
            /* We don't want to try to delete inaccessible storage as it will most likely fail.
             * Note that UIMessageCenter::confirmMediumRemoval() is aware of that and
             * will give a corresponding hint. Therefore, once the code is changed below,
             * the hint should be re-checked for validity. */
            bool fDeleteStorage = false;
            qulonglong uCapability = 0;
            QVector<KMediumFormatCapabilities> capabilities = pMediumItem->medium().medium().GetMediumFormat().GetCapabilities();
            foreach (KMediumFormatCapabilities capability, capabilities)
                uCapability |= capability;
            if (pMediumItem->state() != KMediumState_Inaccessible &&
                uCapability & MediumFormatCapabilities_File)
            {
                int rc = msgCenter().confirmDeleteHardDiskStorage(pMediumItem->location(), this);
                if (rc == AlertButton_Cancel)
                    return;
                fDeleteStorage = rc == AlertButton_Choice1;
            }

            /* Get hard-disk: */
            CMedium hardDisk = pMediumItem->medium().medium();

            if (fDeleteStorage)
            {
                /* Remember hard-disk attributes: */
                QString strLocation = hardDisk.GetLocation();
                /* Prepare delete storage progress: */
                CProgress progress = hardDisk.DeleteStorage();
                if (hardDisk.isOk())
                {
                    /* Show delete storage progress: */
                    msgCenter().showModalProgressDialog(progress, windowTitle(), ":/progress_media_delete_90px.png", this);
                    if (!progress.isOk() || progress.GetResultCode() != 0)
                    {
                        msgCenter().cannotDeleteHardDiskStorage(progress, strLocation, this);
                        return;
                    }
                }
                else
                {
                    msgCenter().cannotDeleteHardDiskStorage(hardDisk, strLocation, this);
                    return;
                }
            }

            /* Close hard-disk: */
            hardDisk.Close();
            result = hardDisk;
            break;
        }
        case UIMediumType_DVD:
        {
            /* Get optical-disk: */
            CMedium image = pMediumItem->medium().medium();
            /* Close optical-disk: */
            image.Close();
            result = image;
            break;
        }
        case UIMediumType_Floppy:
        {
            /* Get floppy-disk: */
            CMedium image = pMediumItem->medium().medium();
            /* Close floppy-disk: */
            image.Close();
            result = image;
            break;
        }
        default: AssertFailedReturnVoid();
    }

    /* Verify result: */
    if (result.isOk())
        vboxGlobal().deleteMedium(strMediumID);
    else
        msgCenter().cannotCloseMedium(pMediumItem->medium(), result, this);
}

void UIMediumManager::sltReleaseMedium()
{
    /* Get current medium-item: */
    UIMediumItem *pMediumItem = toMediumItem(currentTreeWidget()->currentItem());
    AssertMsgReturnVoid(pMediumItem, ("Current item must not be null"));
    AssertReturnVoid(!pMediumItem->id().isNull());

    /* Refresh attached VM id list: */
    pMediumItem->refreshAll();
    const QList<QString> machineIds(pMediumItem->medium().curStateMachineIds());
    /* If the machine id list is empty: */
    if (machineIds.isEmpty())
    {
        /* This may happen if medium was already released by a third party,
         * update the details and silently return. */
        sltHandleCurrentItemChanged(pMediumItem);
        return;
    }

    /* Gather usage list: */
    QStringList usage;
    foreach (const QString &strMachineId, machineIds)
    {
        CMachine machine = m_vbox.FindMachine(strMachineId);
        if (!m_vbox.isOk())
            continue;
        usage << machine.GetName();
    }
    AssertReturnVoid(!usage.isEmpty());

    /* Confirm release: */
    if (!msgCenter().confirmMediumRelease(pMediumItem->medium(), usage.join(", "), this))
        return;

    /* Release: */
    foreach (const QString &strMachineId, machineIds)
        if (!releaseMediumFrom(pMediumItem->medium(), strMachineId))
            break;
}

void UIMediumManager::sltHandleCurrentTabChanged()
{
    /* Get current-tree: */
    QTreeWidget *pTree = currentTreeWidget();

    /* If other tree was focused previously, moving focus to new tree: */
    if (qobject_cast<QTreeWidget*>(focusWidget()))
        pTree->setFocus();

    /* Update current tree-item: */
    sltHandleCurrentItemChanged(pTree->currentItem());
}

void UIMediumManager::sltHandleCurrentItemChanged(QTreeWidgetItem *pItem,
                                                  QTreeWidgetItem *pPrevItem /* = 0 */)
{
    /* Get medium-item: */
    UIMediumItem *pMediumItem = toMediumItem(pItem);

    /* We have to make sure some item is always selected: */
    if (!pMediumItem && pPrevItem)
    {
        /* If the new item is 0, set the old item again. */
        UIMediumItem *pPrevMediumItem = toMediumItem(pPrevItem);
        setCurrentItem(currentTreeWidget(), pPrevMediumItem);
    }

    if (pMediumItem)
    {
        /* Set the file for the proxy icon: */
        setFileForProxyIcon(pMediumItem->location());
        /* Ensures current item visible every time we are switching page: */
        pMediumItem->treeWidget()->scrollToItem(pMediumItem, QAbstractItemView::EnsureVisible);
    }

    /* Update action availability: */
    bool fNotInEnumeration = !vboxGlobal().isMediumEnumerationInProgress();
    bool fActionEnabledCopy    = currentTreeWidgetType() == UIMediumType_HardDisk &&
                                 fNotInEnumeration && pMediumItem && checkMediumFor(pMediumItem, Action_Copy);
    bool fActionEnabledModify  = currentTreeWidgetType() == UIMediumType_HardDisk &&
                                 fNotInEnumeration && pMediumItem && checkMediumFor(pMediumItem, Action_Modify);
    bool fActionEnabledRemove  = fNotInEnumeration && pMediumItem && checkMediumFor(pMediumItem, Action_Remove);
    bool fActionEnabledRelease = pMediumItem && checkMediumFor(pMediumItem, Action_Release);
    m_pActionCopy->setEnabled(fActionEnabledCopy);
    m_pActionModify->setEnabled(fActionEnabledModify);
    m_pActionRemove->setEnabled(fActionEnabledRemove);
    m_pActionRelease->setEnabled(fActionEnabledRelease);

    /* Update information-panes: */
    if (pMediumItem)
    {
        QString strDetails = pMediumItem->details();
        QString strUsage = pMediumItem->usage().isNull() ?
                           formatPaneText(QApplication::translate("VBoxMediaManagerDlg", "<i>Not&nbsp;Attached</i>"), false) :
                           formatPaneText(pMediumItem->usage());
        if (pMediumItem->treeWidget() == mTwHD)
        {
            m_pTypePane->setText(pMediumItem->hardDiskType());
            m_pLocationPane->setText(formatPaneText(pMediumItem->location(), true, "end"));
            m_pFormatPane->setText(pMediumItem->hardDiskFormat());
            m_pDetailsPane->setText(strDetails);
            m_pUsagePane->setText(strUsage);
        }
        else if (pMediumItem->treeWidget() == mTwCD)
        {
            mIpCD1->setText(formatPaneText(pMediumItem->location(), true, "end"));
            mIpCD2->setText(strUsage);
        }
        else if (pMediumItem->treeWidget() == mTwFD)
        {
            mIpFD1->setText(formatPaneText(pMediumItem->location(), true, "end"));
            mIpFD2->setText(strUsage);
        }
    }
    else
        clearInfoPanes();

    /* Enable/disable information-pane containers: */
    mHDContainer->setEnabled(pMediumItem);
    mCDContainer->setEnabled(pMediumItem);
    mFDContainer->setEnabled(pMediumItem);
}

void UIMediumManager::sltHandleDoubleClick()
{
    /* Call for modify-action if hard-disk double-clicked: */
    if (currentTreeWidgetType() == UIMediumType_HardDisk)
        sltModifyMedium();
}

void UIMediumManager::sltHandleContextMenuCall(const QPoint &position)
{
    /* Get surrent widget/item: */
    QTreeWidget *pTree = currentTreeWidget();
    QTreeWidgetItem *pItem = pTree->itemAt(position);
    if (pItem)
    {
        /* Make sure the item is selected and current: */
        setCurrentItem(pTree, pItem);
        /* Show context menu: */
        m_pContextMenu->exec(pTree->viewport()->mapToGlobal(position));
    }
}

void UIMediumManager::sltMakeRequestForTableAdjustment()
{
    /* We have to perform tables adjustment only after all the [auto]resize
     * events processed for every column of corresponding table. */
    QTimer::singleShot(0, this, SLOT(sltPerformTablesAdjustment()));
}

void UIMediumManager::sltPerformTablesAdjustment()
{
    /* Get all the tree-widgets: */
    QList<QITreeWidget*> trees;
    trees << mTwHD;
    trees << mTwCD;
    trees << mTwFD;

    /* Calculate deduction for every header: */
    QList<int> deductions;
    foreach (QITreeWidget *pTree, trees)
    {
        int iDeduction = 0;
        for (int iHeaderIndex = 1; iHeaderIndex < pTree->header()->count(); ++iHeaderIndex)
            iDeduction += pTree->header()->sectionSize(iHeaderIndex);
        deductions << iDeduction;
    }

    /* Adjust the table's first column: */
    for (int iTreeIndex = 0; iTreeIndex < trees.size(); ++iTreeIndex)
    {
        QITreeWidget *pTree = trees[iTreeIndex];
        int iSize0 = pTree->viewport()->width() - deductions[iTreeIndex];
        if (pTree->header()->sectionSize(0) != iSize0)
            pTree->header()->resizeSection(0, iSize0);
    }
}

void UIMediumManager::prepare()
{
    /* Prepare this: */
    prepareThis();
    /* Prepare actions: */
    prepareActions();
    /* Prepare menu-bar: */
    prepareMenuBar();
    /* Prepare tool-bar: */
    prepareToolBar();
    /* Prepare context-menu: */
    prepareContextMenu();
    /* Prepare tab-widget: */
    preapreTabWidget();
    /* Prepare tree-widgets: */
    prepareTreeWidgets();
    /* Prepare information-panes: */
    prepareInformationPanes();
    /* Prepare button-box: */
    prepareButtonBox();
    /* Prepare progress-bar: */
    prepareProgressBar();

    /* Translate dialog: */
    retranslateUi();

#ifdef Q_WS_MAC
    /* Prepare Mac window-menu.
     * Should go *after* translation! */
    prepareMacWindowMenu();
#endif /* Q_WS_MAC */

    /* Center according passed widget: */
    centerAccording(m_pCenterWidget);

    /* Populate content for tree-widgets: */
    populateTreeWidgets();
}

void UIMediumManager::prepareThis()
{
    /* Dialog should delete itself on 'close': */
    setAttribute(Qt::WA_DeleteOnClose);

    /* And no need to count it as important for application.
     * This way it will NOT be taken into account
     * when other top-level windows will be closed: */
    setAttribute(Qt::WA_QuitOnClose, false);

    /* Apply window icons: */
    setWindowIcon(UIIconPool::iconSetFull(QSize(32, 32), QSize(16, 16),
                                          ":/diskimage_32px.png", ":/diskimage_16px.png"));

    /* Apply UI decorations: */
    Ui::UIMediumManager::setupUi(this);

    /* Prepare some global stuff: */
    m_vbox = vboxGlobal().virtualBox();
    Assert(!m_vbox.isNull());

    /* Configure medium-processing connections: */
    connect(&vboxGlobal(), SIGNAL(sigMediumCreated(const QString&)),
            this, SLOT(sltHandleMediumCreated(const QString&)));
    connect(&vboxGlobal(), SIGNAL(sigMediumDeleted(const QString&)),
            this, SLOT(sltHandleMediumDeleted(const QString&)));

    /* Configure medium-enumeration connections: */
    connect(&vboxGlobal(), SIGNAL(sigMediumEnumerationStarted()),
            this, SLOT(sltHandleMediumEnumerationStart()));
    connect(&vboxGlobal(), SIGNAL(sigMediumEnumerated(const QString&)),
            this, SLOT(sltHandleMediumEnumerated(const QString&)));
    connect(&vboxGlobal(), SIGNAL(sigMediumEnumerationFinished()),
            this, SLOT(sltHandleMediumEnumerationFinish()));
}

void UIMediumManager::prepareActions()
{
    /* Create copy-action: */
    m_pActionCopy = new QAction(this);
    {
        /* Configure copy-action: */
        m_pActionCopy->setShortcut(QKeySequence("Ctrl+O"));
        m_pActionCopy->setIcon(UIIconPool::iconSetFull(QSize(22, 22), QSize(16, 16),
                                                       ":/hd_add_22px.png", ":/hd_add_16px.png",
                                                       ":/hd_add_disabled_22px.png", ":/hd_add_disabled_16px.png"));
        connect(m_pActionCopy, SIGNAL(triggered()), this, SLOT(sltCopyMedium()));
    }

    /* Create modify-action: */
    m_pActionModify = new QAction(this);
    {
        /* Configure modify-action: */
        m_pActionModify->setShortcut(QKeySequence("Ctrl+Space"));
        m_pActionModify->setIcon(UIIconPool::iconSetFull(QSize(22, 22), QSize(16, 16),
                                                         ":/hd_new_22px.png", ":/hd_new_16px.png",
                                                         ":/hd_new_disabled_22px.png", ":/hd_new_disabled_16px.png"));
        connect(m_pActionModify, SIGNAL(triggered()), this, SLOT(sltModifyMedium()));
    }

    /* Create remove-action: */
    m_pActionRemove  = new QAction(this);
    {
        /* Configure remove-action: */
        m_pActionRemove->setShortcut(QKeySequence(QKeySequence::Delete));
        m_pActionRemove->setIcon(UIIconPool::iconSetFull(QSize(22, 22), QSize(16, 16),
                                                         ":/hd_remove_22px.png", ":/hd_remove_16px.png",
                                                         ":/hd_remove_disabled_22px.png", ":/hd_remove_disabled_16px.png"));
        connect(m_pActionRemove, SIGNAL(triggered()), this, SLOT(sltRemoveMedium()));
    }

    /* Create release-action: */
    m_pActionRelease = new QAction(this);
    {
        /* Configure release-action: */
        m_pActionRelease->setShortcut(QKeySequence("Ctrl+L"));
        m_pActionRelease->setIcon(UIIconPool::iconSetFull(QSize(22, 22), QSize(16, 16),
                                                          ":/hd_release_22px.png", ":/hd_release_16px.png",
                                                          ":/hd_release_disabled_22px.png", ":/hd_release_disabled_16px.png"));
        connect(m_pActionRelease, SIGNAL(triggered()), this, SLOT(sltReleaseMedium()));
    }

    /* Create refresh-action: */
    m_pActionRefresh = new QAction(this);
    {
        /* Configure refresh-action: */
        m_pActionRefresh->setShortcut(QKeySequence(QKeySequence::Refresh));
        m_pActionRefresh->setIcon(UIIconPool::iconSetFull(QSize(22, 22), QSize(16, 16),
                                                          ":/refresh_22px.png", ":/refresh_16px.png",
                                                          ":/refresh_disabled_22px.png", ":/refresh_disabled_16px.png"));
        connect(m_pActionRefresh, SIGNAL(triggered()), this, SLOT(refreshAll()));
    }
}

void UIMediumManager::prepareMenuBar()
{
    /* Create actions-menu for menu-bar: */
    m_pMenu = menuBar()->addMenu(QString());
    {
        /* Configure menu-bar menu: */
        m_pMenu->addAction(m_pActionCopy);
        m_pMenu->addAction(m_pActionModify);
        m_pMenu->addAction(m_pActionRemove);
        m_pMenu->addAction(m_pActionRelease);
        m_pMenu->addAction(m_pActionRefresh);
    }
}

void UIMediumManager::prepareToolBar()
{
    /* Create tool-bar: */
    m_pToolBar = new UIToolBar(this);
    {
        /* Configure tool-bar: */
        m_pToolBar->setIconSize(QSize(22, 22));
        m_pToolBar->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        m_pToolBar->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
        /* Add tool-bar actions: */
        m_pToolBar->addAction(m_pActionCopy);
        m_pToolBar->addAction(m_pActionModify);
        m_pToolBar->addAction(m_pActionRemove);
        m_pToolBar->addAction(m_pActionRelease);
        m_pToolBar->addAction(m_pActionRefresh);
        /* Integrate tool-bar into dialog: */
        QVBoxLayout *pMainLayout = qobject_cast<QVBoxLayout*>(centralWidget()->layout());
        Assert(pMainLayout);
#if MAC_LEOPARD_STYLE
        /* Enable unified tool-bars on Mac OS X. Available on Qt >= 4.3: */
        addToolBar(m_pToolBar);
        m_pToolBar->setMacToolbar();
        /* No spacing/margin on the Mac: */
        pMainLayout->setContentsMargins(0, 0, 0, 0);
        pMainLayout->insertSpacing(0, 10);
#else /* MAC_LEOPARD_STYLE */
        /* Add the tool-bar: */
        pMainLayout->insertWidget(0, m_pToolBar);
        /* Set spacing/margin like in the selector window: */
        pMainLayout->setSpacing(5);
        pMainLayout->setContentsMargins(5, 5, 5, 5);
#endif /* MAC_LEOPARD_STYLE */
    }
}

void UIMediumManager::prepareContextMenu()
{
    /* Create context-menu: */
    m_pContextMenu = new QMenu(this);
    {
        /* Configure contex-menu: */
        m_pContextMenu->addAction(m_pActionCopy);
        m_pContextMenu->addAction(m_pActionModify);
        m_pContextMenu->addAction(m_pActionRemove);
        m_pContextMenu->addAction(m_pActionRelease);
    }
}

void UIMediumManager::preapreTabWidget()
{
    /* Tab-widget created in .ui file. */
    {
        /* Setup tab-widget: */
        mTabWidget->setFocusPolicy(Qt::TabFocus);
        mTabWidget->setTabIcon(HDTab, m_iconHD);
        mTabWidget->setTabIcon(CDTab, m_iconCD);
        mTabWidget->setTabIcon(FDTab, m_iconFD);
        connect(mTabWidget, SIGNAL(currentChanged(int)), this, SLOT(sltHandleCurrentTabChanged()));
    }
}

void UIMediumManager::prepareTreeWidgets()
{
    /* Prepare tree-widget HD: */
    prepareTreeWidgetHD();
    /* Prepare tree-widget CD: */
    prepareTreeWidgetCD();
    /* Prepare tree-widget FD: */
    prepareTreeWidgetFD();

    /* Focus current tree: */
    currentTreeWidget()->setFocus();
}

void UIMediumManager::prepareTreeWidgetHD()
{
    /* HD tree-widget created in .ui file. */
    {
        /* Setup HD tree-widget: */
        mTwHD->setColumnCount(3);
        mTwHD->sortItems(0, Qt::AscendingOrder);
        mTwHD->header()->setResizeMode(0, QHeaderView::Fixed);
        mTwHD->header()->setResizeMode(1, QHeaderView::ResizeToContents);
        mTwHD->header()->setResizeMode(2, QHeaderView::ResizeToContents);
        mTwHD->header()->setStretchLastSection(false);
        mTwHD->setSortingEnabled(true);
        connect(mTwHD, SIGNAL(currentItemChanged(QTreeWidgetItem*, QTreeWidgetItem*)),
                this, SLOT(sltHandleCurrentItemChanged(QTreeWidgetItem*, QTreeWidgetItem*)));
        connect(mTwHD, SIGNAL(itemDoubleClicked(QTreeWidgetItem*, int)),
                this, SLOT(sltHandleDoubleClick()));
        connect(mTwHD, SIGNAL(customContextMenuRequested(const QPoint&)),
                this, SLOT(sltHandleContextMenuCall(const QPoint&)));
        connect(mTwHD, SIGNAL(resized(const QSize&, const QSize&)),
                this, SLOT(sltMakeRequestForTableAdjustment()));
        connect(mTwHD->header(), SIGNAL(sectionResized(int, int, int)),
                this, SLOT(sltMakeRequestForTableAdjustment()));
    }
}

void UIMediumManager::prepareTreeWidgetCD()
{
    /* CD tree-widget created in .ui file. */
    {
        /* Setup CD tree-widget: */
        mTwCD->setColumnCount(2);
        mTwCD->sortItems(0, Qt::AscendingOrder);
        mTwCD->header()->setResizeMode(0, QHeaderView::Fixed);
        mTwCD->header()->setResizeMode(1, QHeaderView::ResizeToContents);
        mTwCD->header()->setStretchLastSection(false);
        mTwCD->setSortingEnabled(true);
        connect(mTwCD, SIGNAL(currentItemChanged(QTreeWidgetItem*, QTreeWidgetItem*)),
                this, SLOT(sltHandleCurrentItemChanged(QTreeWidgetItem*, QTreeWidgetItem*)));
        connect(mTwCD, SIGNAL(itemDoubleClicked(QTreeWidgetItem*, int)),
                this, SLOT(sltHandleDoubleClick()));
        connect(mTwCD, SIGNAL(customContextMenuRequested(const QPoint&)),
                this, SLOT(sltHandleContextMenuCall(const QPoint&)));
        connect(mTwCD, SIGNAL(resized(const QSize&, const QSize&)),
                this, SLOT(sltMakeRequestForTableAdjustment()));
        connect(mTwCD->header(), SIGNAL(sectionResized(int, int, int)),
                this, SLOT(sltMakeRequestForTableAdjustment()));
    }
}

void UIMediumManager::prepareTreeWidgetFD()
{
    /* FD tree-widget created in .ui file. */
    {
        /* Setup FD tree-widget: */
        mTwFD->setColumnCount(2);
        mTwFD->sortItems(0, Qt::AscendingOrder);
        mTwFD->header()->setResizeMode(0, QHeaderView::Fixed);
        mTwFD->header()->setResizeMode(1, QHeaderView::ResizeToContents);
        mTwFD->header()->setStretchLastSection(false);
        mTwFD->setSortingEnabled(true);
        connect(mTwFD, SIGNAL(currentItemChanged(QTreeWidgetItem*, QTreeWidgetItem*)),
                this, SLOT(sltHandleCurrentItemChanged(QTreeWidgetItem*, QTreeWidgetItem*)));
        connect(mTwFD, SIGNAL(itemDoubleClicked(QTreeWidgetItem*, int)),
                this, SLOT(sltHandleDoubleClick()));
        connect(mTwFD, SIGNAL(customContextMenuRequested(const QPoint&)),
                this, SLOT(sltHandleContextMenuCall(const QPoint&)));
        connect(mTwFD, SIGNAL(resized(const QSize&, const QSize&)),
                this, SLOT(sltMakeRequestForTableAdjustment()));
        connect(mTwFD->header(), SIGNAL(sectionResized(int, int, int)),
                this, SLOT(sltMakeRequestForTableAdjustment()));
    }
}

void UIMediumManager::prepareInformationPanes()
{
    /* Information-panes created in .ui file. */
    {
        /* Configure information-panes: */
        QList<QILabel*> panes = findChildren<QILabel*>();
        foreach (QILabel *pPane, panes)
            pPane->setFullSizeSelection(true);
    }
}

void UIMediumManager::prepareButtonBox()
{
    /* Button-box created in .ui file. */
    {
        /* Configure button-box: */
        mButtonBox->button(QDialogButtonBox::Ok)->setDefault(true);
        connect(mButtonBox, SIGNAL(accepted()), this, SLOT(accept()));
        connect(mButtonBox, SIGNAL(helpRequested()), &msgCenter(), SLOT(sltShowHelpHelpDialog()));
    }
}

void UIMediumManager::prepareProgressBar()
{
    /* Create progress-bar: */
    m_pProgressBar = new UIEnumerationProgressBar(this);
    {
        /* Hidden by default: */
        m_pProgressBar->hide();
        /* Integrate to the button-box: */
        mButtonBox->addExtraWidget(m_pProgressBar);
    }
}

#ifdef Q_WS_MAC
void UIMediumManager::prepareMacWindowMenu()
{
    /* Create window-menu for menu-bar: */
    menuBar()->addMenu(UIWindowMenuManager::instance()->createMenu(this));
    UIWindowMenuManager::instance()->addWindow(this);
}
#endif /* Q_WS_MAC */

void UIMediumManager::populateTreeWidgets()
{
    /* If refresh was requested and enumeration was not yet started: */
    if (m_fRefresh && !vboxGlobal().isMediumEnumerationInProgress())
    {
        /* Just start medium-enumeration: */
        vboxGlobal().startMediumEnumeration();
    }
    /* If refresh was not requested or enumeration already started: */
    else
    {
        /* Emulate (possible partial) medium-enumeration: */
        QList<QString> mediumIDs = vboxGlobal().mediumIDs();
        prepareToRefresh(mediumIDs.size());
        foreach (const QString &strMediumID, mediumIDs)
        {
            /* Get corresponding medium: */
            const UIMedium medium = vboxGlobal().medium(strMediumID);
            /* Create corresponding medium-item: */
            sltHandleMediumCreated(strMediumID);
            /* Advance progress-bar only for created mediums: */
            if (medium.state() != KMediumState_NotCreated)
                m_pProgressBar->setValue(m_pProgressBar->value() + 1);
        }
        /* Finally, emulate enumeration finish,
         * if enumeration already finished or wasn't started: */
        if (!vboxGlobal().isMediumEnumerationInProgress())
            sltHandleMediumEnumerationFinish();
    }

    /* For a newly opened dialog, select the first item: */
    if (!mTwHD->currentItem())
        if (QTreeWidgetItem *pItem = mTwHD->topLevelItem(0))
            setCurrentItem(mTwHD, pItem);
    if (!mTwCD->currentItem())
        if (QTreeWidgetItem *pItem = mTwCD->topLevelItem(0))
            setCurrentItem(mTwCD, pItem);
    if (!mTwFD->currentItem())
        if (QTreeWidgetItem *pItem = mTwFD->topLevelItem(0))
            setCurrentItem(mTwFD, pItem);
}

#ifdef Q_WS_MAC
void UIMediumManager::cleanupMacWindowMenu()
{
    /* Destroy window-menu of menu-bar: */
    UIWindowMenuManager::instance()->removeWindow(this);
    UIWindowMenuManager::instance()->destroyMenu(this);
}
#endif /* Q_WS_MAC */

void UIMediumManager::cleanup()
{
#ifdef Q_WS_MAC
    /* Cleanup Mac window-menu: */
    cleanupMacWindowMenu();
#endif /* Q_WS_MAC */
}

void UIMediumManager::retranslateUi()
{
    /* Translate uic generated strings: */
    Ui::UIMediumManager::retranslateUi(this);

    /* Menu: */
    m_pMenu->setTitle(QApplication::translate("VBoxMediaManagerDlg", "&Actions"));
    /* Action names: */
    m_pActionCopy->setText(QApplication::translate("VBoxMediaManagerDlg", "&Copy..."));
    m_pActionModify->setText(QApplication::translate("VBoxMediaManagerDlg", "&Modify..."));
    m_pActionRemove->setText(QApplication::translate("VBoxMediaManagerDlg", "R&emove"));
    m_pActionRelease->setText(QApplication::translate("VBoxMediaManagerDlg", "Re&lease"));
    m_pActionRefresh->setText(QApplication::translate("VBoxMediaManagerDlg", "Re&fresh"));
    /* Action tool-tips: */
    m_pActionCopy->setToolTip(m_pActionCopy->text().remove('&') + QString(" (%1)").arg(m_pActionCopy->shortcut().toString()));
    m_pActionModify->setToolTip(m_pActionModify->text().remove('&') + QString(" (%1)").arg(m_pActionModify->shortcut().toString()));
    m_pActionRemove->setToolTip(m_pActionRemove->text().remove('&') + QString(" (%1)").arg(m_pActionRemove->shortcut().toString()));
    m_pActionRelease->setToolTip(m_pActionRelease->text().remove('&') + QString(" (%1)").arg(m_pActionRelease->shortcut().toString()));
    m_pActionRefresh->setToolTip(m_pActionRefresh->text().remove('&') + QString(" (%1)").arg(m_pActionRefresh->shortcut().toString()));
    /* Action status-tips: */
    m_pActionCopy->setStatusTip(QApplication::translate("VBoxMediaManagerDlg", "Copy an existing disk image file"));
    m_pActionModify->setStatusTip(QApplication::translate("VBoxMediaManagerDlg", "Modify the attributes of the selected disk image file"));
    m_pActionRemove->setStatusTip(QApplication::translate("VBoxMediaManagerDlg", "Remove the selected disk image file"));
    m_pActionRelease->setStatusTip(QApplication::translate("VBoxMediaManagerDlg", "Release the selected disk image file by detaching it from the machines"));
    m_pActionRefresh->setStatusTip(QApplication::translate("VBoxMediaManagerDlg", "Refresh the list of disk image files"));

    /* Tool-bar: */
#ifdef Q_WS_MAC
# ifdef QT_MAC_USE_COCOA
    /* There is a bug in Qt Cocoa which result in showing a "more arrow" when
       the necessary size of the toolbar is increased. Also for some languages
       the with doesn't match if the text increase. So manually adjust the size
       after changing the text. */
    m_pToolBar->updateLayout();
# endif /* QT_MAC_USE_COCOA */
#endif /* Q_WS_MAC */

    // TODO: Just rename translation context in .nls files!
    /* Translations moved from VBoxMediaManagerDlg.ui file to keep old translation context: */
    setWindowTitle(QApplication::translate("VBoxMediaManagerDlg", "Virtual Media Manager"));
    mTabWidget->setTabText(0, tr("&Hard drives"));
    mTwHD->headerItem()->setText(0, QApplication::translate("VBoxMediaManagerDlg", "Name"));
    mTwHD->headerItem()->setText(1, QApplication::translate("VBoxMediaManagerDlg", "Virtual Size"));
    mTwHD->headerItem()->setText(2, QApplication::translate("VBoxMediaManagerDlg", "Actual Size"));
    m_pTypeLabel->setText(QApplication::translate("VBoxMediaManagerDlg", "Type:"));
    m_pLocationLabel->setText(QApplication::translate("VBoxMediaManagerDlg", "Location:"));
    m_pFormatLabel->setText(QApplication::translate("VBoxMediaManagerDlg", "Format:"));
    m_pDetailsLabel->setText(QApplication::translate("VBoxMediaManagerDlg", "Storage details:"));
    m_pUsageLabel->setText(QApplication::translate("VBoxMediaManagerDlg", "Attached to:"));
    mTabWidget->setTabText(1, tr("&Optical disks"));
    mTwCD->headerItem()->setText(0, QApplication::translate("VBoxMediaManagerDlg", "Name"));
    mTwCD->headerItem()->setText(1, QApplication::translate("VBoxMediaManagerDlg", "Size"));
    mLbCD1->setText(QApplication::translate("VBoxMediaManagerDlg", "Location:"));
    mLbCD2->setText(QApplication::translate("VBoxMediaManagerDlg", "Attached to:"));
    mTabWidget->setTabText(2, tr("&Floppy disks"));
    mTwFD->headerItem()->setText(0, QApplication::translate("VBoxMediaManagerDlg", "Name"));
    mTwFD->headerItem()->setText(1, QApplication::translate("VBoxMediaManagerDlg", "Size"));
    mLbFD1->setText(QApplication::translate("VBoxMediaManagerDlg", "Location:"));
    mLbFD2->setText(QApplication::translate("VBoxMediaManagerDlg", "Attached to:"));

    /* Progress-bar: */
    m_pProgressBar->setText(QApplication::translate("VBoxMediaManagerDlg", "Checking accessibility"));
#ifdef Q_WS_MAC
    /* Make sure that the widgets aren't jumping around
     * while the progress-bar get visible. */
    m_pProgressBar->adjustSize();
    int h = m_pProgressBar->height();
    mButtonBox->setMinimumHeight(h + 12);
#endif /* Q_WS_MAC */

    /* Button-box: */
    mButtonBox->button(QDialogButtonBox::Ok)->setText(tr("C&lose"));

    /* Full refresh if there is at least one item present: */
    if (mTwHD->topLevelItemCount() || mTwCD->topLevelItemCount() || mTwFD->topLevelItemCount())
        refreshAll();
}

bool UIMediumManager::releaseMediumFrom(const UIMedium &medium, const QString &strMachineId)
{
    /* Open session: */
    CSession session = vboxGlobal().openSession(strMachineId);
    if (session.isNull())
        return false;

    /* Get machine: */
    CMachine machine = session.GetMachine();

    /* Prepare result: */
    bool fSuccess = true;

    /* Depending on medium-type: */
    switch (medium.type())
    {
        case UIMediumType_HardDisk: fSuccess = releaseHardDiskFrom(medium, machine); break;
        case UIMediumType_DVD:      fSuccess = releaseOpticalDiskFrom(medium, machine); break;
        case UIMediumType_Floppy:   fSuccess = releaseFloppyDiskFrom(medium, machine); break;
        default: AssertMsgFailed(("Medium-type unknown: %d\n", medium.type())); break;
    }

    /* If medium was released: */
    if (fSuccess)
    {
        /* Save machine settings: */
        machine.SaveSettings();
        if (!machine.isOk())
        {
            msgCenter().cannotSaveMachineSettings(machine, this);
            fSuccess = false;
        }
    }

    /* Close session: */
    session.UnlockMachine();

    /* Return result: */
    return fSuccess;
}

bool UIMediumManager::releaseHardDiskFrom(const UIMedium &medium, CMachine &machine)
{
    /* Enumerate attachments: */
    CMediumAttachmentVector attachments = machine.GetMediumAttachments();
    foreach (const CMediumAttachment &attachment, attachments)
    {
        /* Skip non-hard-disks: */
        if (attachment.GetType() != KDeviceType_HardDisk)
            continue;

        /* Skip unrelated hard-disks: */
        if (attachment.GetMedium().GetId() != medium.id())
            continue;

        /* Try detaching device: */
        machine.DetachDevice(attachment.GetController(), attachment.GetPort(), attachment.GetDevice());
        if (machine.isOk())
        {
            /* Return success: */
            return true;
        }
        else
        {
            /* Show error: */
            CStorageController controller = machine.GetStorageControllerByName(attachment.GetController());
            msgCenter().cannotDetachDevice(machine, UIMediumType_HardDisk, medium.location(),
                                           StorageSlot(controller.GetBus(), attachment.GetPort(), attachment.GetDevice()), this);
            /* Return failure: */
            return false;
        }
    }
    /* Return failure: */
    return false;
}

bool UIMediumManager::releaseOpticalDiskFrom(const UIMedium &medium, CMachine &machine)
{
    /* Enumerate attachments: */
    CMediumAttachmentVector attachments = machine.GetMediumAttachments();
    foreach (const CMediumAttachment &attachment, attachments)
    {
        /* Skip non-optical-disks: */
        if (attachment.GetType() != KDeviceType_DVD)
            continue;

        /* Skip unrelated optical-disks: */
        if (attachment.GetMedium().GetId() != medium.id())
            continue;

        /* Try device unmounting: */
        machine.MountMedium(attachment.GetController(), attachment.GetPort(), attachment.GetDevice(), CMedium(), false /* force */);
        if (machine.isOk())
        {
            /* Return success: */
            return true;
        }
        else
        {
            /* Show error: */
            msgCenter().cannotRemountMedium(machine, medium, false /* mount? */, false /* retry? */, this);
            /* Return failure: */
            return false;
        }
    }
    /* Return failure: */
    return false;
}

bool UIMediumManager::releaseFloppyDiskFrom(const UIMedium &medium, CMachine &machine)
{
    /* Enumerate attachments: */
    CMediumAttachmentVector attachments = machine.GetMediumAttachments();
    foreach (const CMediumAttachment &attachment, attachments)
    {
        /* Skip non-floppy-disks: */
        if (attachment.GetType() != KDeviceType_Floppy)
            continue;

        /* Skip unrelated floppy-disks: */
        if (attachment.GetMedium().GetId() != medium.id())
            continue;

        /* Try device unmounting: */
        machine.MountMedium(attachment.GetController(), attachment.GetPort(), attachment.GetDevice(), CMedium(), false /* force */);
        if (machine.isOk())
        {
            /* Return success: */
            return true;
        }
        else
        {
            /* Show error: */
            msgCenter().cannotRemountMedium(machine, medium, false /* mount? */, false /* retry? */, this);
            /* Return failure: */
            return false;
        }
    }
    /* Return failure: */
    return false;
}

QTreeWidget* UIMediumManager::treeWidget(UIMediumType type) const
{
    /* Return corresponding tree-widget: */
    switch (type)
    {
        case UIMediumType_HardDisk: return mTwHD;
        case UIMediumType_DVD:      return mTwCD;
        case UIMediumType_Floppy:   return mTwFD;
        default: AssertMsgFailed(("Medium-type unknown: %d\n", type)); break;
    }
    /* Null by default: */
    return 0;
}

UIMediumType UIMediumManager::currentTreeWidgetType() const
{
    /* Return corresponding medium-type: */
    switch (mTabWidget->currentIndex())
    {
        case HDTab: return UIMediumType_HardDisk;
        case CDTab: return UIMediumType_DVD;
        case FDTab: return UIMediumType_Floppy;
        default: AssertMsgFailed(("Page-type unknown: %d\n", mTabWidget->currentIndex())); break;
    }
    /* Invalid by default: */
    return UIMediumType_Invalid;
}

QTreeWidget* UIMediumManager::currentTreeWidget() const
{
    return treeWidget(currentTreeWidgetType());
}

void UIMediumManager::setCurrentItem(QTreeWidget *pTree, QTreeWidgetItem *pItem)
{
    if (pTree && pItem)
    {
        pItem->setSelected(true);
        pTree->setCurrentItem(pItem);
        pTree->scrollToItem(pItem, QAbstractItemView::EnsureVisible);
        sltHandleCurrentItemChanged(pItem);
    }
    else
        sltHandleCurrentTabChanged();
}

UIMediumItem* UIMediumManager::toMediumItem(QTreeWidgetItem *pItem) const
{
    /* Return UIMediumItem based on QTreeWidgetItem if it is valid: */
    if (pItem && pItem->type() == UIMediumItem::Type)
        return static_cast<UIMediumItem*>(pItem);
    /* Null by default: */
    return 0;
}

UIMediumItem* UIMediumManager::searchItem(QTreeWidget *pTree, const CheckIfSuitableBy &functor) const
{
    /* Make sure argument is valid: */
    if (!pTree)
        return 0;

    /* Return wrapper: */
    return searchItem(pTree->invisibleRootItem(), functor);
}

UIMediumItem* UIMediumManager::searchItem(QTreeWidgetItem *pParentItem, const CheckIfSuitableBy &functor) const
{
    /* Make sure argument is valid: */
    if (!pParentItem)
        return 0;

    /* Verify passed item if it is of 'medium' type too: */
    if (UIMediumItem *pMediumParentItem = toMediumItem(pParentItem))
        if (functor.isItSuitable(pMediumParentItem))
            return pMediumParentItem;

    /* Iterate other all the children: */
    for (int iChildIndex = 0; iChildIndex < pParentItem->childCount(); ++iChildIndex)
        if (UIMediumItem *pMediumChildItem = toMediumItem(pParentItem->child(iChildIndex)))
            if (UIMediumItem *pRequiredMediumChildItem = searchItem(pMediumChildItem, functor))
                return pRequiredMediumChildItem;

    /* Null by default: */
    return 0;
}

UIMediumItem* UIMediumManager::createHardDiskItem(QTreeWidget *pTree, const UIMedium &medium) const
{
    /* Make sure passed medium is valid: */
    AssertReturn(!medium.medium().isNull(), 0);

    /* Search for medium-item: */
    UIMediumItem *pMediumItem = searchItem(pTree, CheckIfSuitableByID(medium.id()));

    /* If medium-item do not exists: */
    if (!pMediumItem)
    {
        /* If medium have a parent: */
        if (medium.parentID() != UIMedium::nullID())
        {
            /* Try to find parent medium-item: */
            UIMediumItem *pParentMediumItem = searchItem(pTree, CheckIfSuitableByID(medium.parentID()));
            /* If parent medium-item was not found: */
            if (!pParentMediumItem)
            {
                /* Make sure such corresponding parent medium is already cached! */
                UIMedium parentMedium = vboxGlobal().medium(medium.parentID());
                if (parentMedium.isNull())
                    AssertMsgFailed(("Parent medium with ID={%s} was not found!\n", medium.parentID().toAscii().constData()));
                /* Try to create parent medium-item: */
                else
                    pParentMediumItem = createHardDiskItem(pTree, parentMedium);
            }
            /* If parent medium-item was found: */
            if (pParentMediumItem)
                pMediumItem = new UIMediumItem(medium, pParentMediumItem);
        }
        /* Else just create item as top-level one: */
        if (!pMediumItem)
            pMediumItem = new UIMediumItem(medium, pTree);
    }

    /* Return medium-item: */
    return pMediumItem;
}

void UIMediumManager::updateTabIcons(UIMediumItem *pMediumItem, ItemAction action)
{
    /* Make sure medium-item is valid: */
    AssertReturnVoid(pMediumItem);

    /* Prepare data for tab: */
    int iTab = -1;
    const QIcon *pIcon = 0;
    bool *pfInaccessible = 0;
    switch (pMediumItem->mediumType())
    {
        case UIMediumType_HardDisk:
            iTab = HDTab;
            pIcon = &m_iconHD;
            pfInaccessible = &m_fInaccessibleHD;
            break;
        case UIMediumType_DVD:
            iTab = CDTab;
            pIcon = &m_iconCD;
            pfInaccessible = &m_fInaccessibleCD;
            break;
        case UIMediumType_Floppy:
            iTab = FDTab;
            pIcon = &m_iconFD;
            pfInaccessible = &m_fInaccessibleFD;
            break;
        default:
            AssertFailed();
    }
    AssertReturnVoid(iTab != -1 && pIcon && pfInaccessible);

    switch (action)
    {
        case ItemAction_Added:
        {
            /* Does it change the overall state? */
            if (*pfInaccessible || pMediumItem->state() != KMediumState_Inaccessible)
                break; /* no */

            *pfInaccessible = true;

            mTabWidget->setTabIcon(iTab, vboxGlobal().warningIcon());

            break;
        }
        case ItemAction_Updated:
        case ItemAction_Removed:
        {
            bool fCheckRest = false;

            if (action == ItemAction_Updated)
            {
                /* Does it change the overall state? */
                if ((*pfInaccessible && pMediumItem->state() == KMediumState_Inaccessible) ||
                    (!*pfInaccessible && pMediumItem->state() != KMediumState_Inaccessible))
                    break; /* no */

                /* Is the given item in charge? */
                if (!*pfInaccessible && pMediumItem->state() == KMediumState_Inaccessible)
                    *pfInaccessible = true; /* yes */
                else
                    fCheckRest = true; /* no */
            }
            else
                fCheckRest = true;

            if (fCheckRest)
            {
                /* Find the first KMediumState_Inaccessible item to be in charge: */
                UIMediumItem *pInaccessibleMediumItem =
                    searchItem(pMediumItem->treeWidget(), CheckIfSuitableByState(KMediumState_Inaccessible));
                *pfInaccessible = !!pInaccessibleMediumItem;
            }

            if (*pfInaccessible)
                mTabWidget->setTabIcon(iTab, vboxGlobal().warningIcon());
            else
                mTabWidget->setTabIcon(iTab, *pIcon);

            break;
        }
    }
}

bool UIMediumManager::checkMediumFor(UIMediumItem *pItem, Action action)
{
    /* Make sure passed ID is valid: */
    AssertReturn(pItem, false);

    switch (action)
    {
        case Action_Edit:
        {
            /* Edit means changing the description and alike; any media that is
             * not being read to or written from can be altered in these terms. */
            switch (pItem->state())
            {
                case KMediumState_NotCreated:
                case KMediumState_Inaccessible:
                case KMediumState_LockedRead:
                case KMediumState_LockedWrite:
                    return false;
                default:
                    break;
            }
            return true;
        }
        case Action_Copy:
        {
            /* False for children: */
            return pItem->medium().parentID() == UIMedium::nullID();
        }
        case Action_Modify:
        {
            /* False for children: */
            return pItem->medium().parentID() == UIMedium::nullID();
        }
        case Action_Remove:
        {
            /* Removable if not attached to anything: */
            return !pItem->isUsed();
        }
        case Action_Release:
        {
            /* Releasable if attached but not in snapshots: */
            return pItem->isUsed() && !pItem->isUsedInSnapshots();
        }
    }

    AssertFailedReturn(false);
}

void UIMediumManager::clearInfoPanes()
{
    m_pTypePane->clear(); m_pLocationPane->clear(); m_pFormatPane->clear(); m_pDetailsPane->clear(); m_pUsagePane->clear();
    mIpCD1->clear(); mIpCD2->clear();
    mIpFD1->clear(); mIpFD2->clear();
}

void UIMediumManager::prepareToRefresh(int iTotal)
{
    /* Info panel clearing: */
    clearInfoPanes();

    /* Prepare progressbar: */
    if (m_pProgressBar)
    {
        m_pProgressBar->setMaximum(iTotal);
        m_pProgressBar->setValue(0);
        m_pProgressBar->setVisible(true);
    }

    /* Enable refresh action: */
    m_pActionRefresh->setEnabled(false);

    /* Set busy cursor: */
    setCursor(QCursor(Qt::BusyCursor));

    /* Store the current list selections: */
    UIMediumItem *pMediumItem = 0;
    pMediumItem = toMediumItem(mTwHD->currentItem());
    if (m_strSelectedIdHD.isNull())
        m_strSelectedIdHD = pMediumItem ? pMediumItem->id() : QString();
    pMediumItem = toMediumItem(mTwCD->currentItem());
    if (m_strSelectedIdCD.isNull())
        m_strSelectedIdCD = pMediumItem ? pMediumItem->id() : QString();
    pMediumItem = toMediumItem(mTwFD->currentItem());
    if (m_strSelectedIdFD.isNull())
        m_strSelectedIdFD = pMediumItem ? pMediumItem->id() : QString();

    /* Finally, clear all the lists...
     * Qt4 has interesting bug here. It sends the currentChanged (cur, prev)
     * signal during list clearing with 'cur' set to null and 'prev' pointing
     * to already excluded element if this element is not topLevelItem
     * (has parent). Cause the Hard Disk list has such elements there is
     * seg-fault when trying to make 'prev' element the current due to 'cur'
     * is null and at least one element have to be selected (by policy).
     * So just blocking any signals outgoing from the list during clearing. */
    mTwHD->blockSignals(true);
    mTwHD->clear();
    mTwHD->blockSignals(false);
    mTwCD->clear();
    mTwFD->clear();
}

/* static */
QString UIMediumManager::formatPaneText(const QString &strText, bool fCompact /* = true */,
                                        const QString &strElipsis /* = "middle" */)
{
    QString compactString = QString("<compact elipsis=\"%1\">").arg(strElipsis);
    QString strInfo = QString("<nobr>%1%2%3</nobr>")
                              .arg(fCompact ? compactString : "")
                              .arg(strText.isEmpty() ?
                                   QApplication::translate("VBoxMediaManagerDlg", "--", "no info") :
                                   strText)
                              .arg(fCompact ? "</compact>" : "");
    return strInfo;
}

#include "UIMediumManager.moc"

