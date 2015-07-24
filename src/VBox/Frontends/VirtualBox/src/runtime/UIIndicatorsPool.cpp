/* $Id: UIIndicatorsPool.cpp $ */
/** @file
 *
 * VBox frontends: Qt GUI ("VirtualBox"):
 * UIIndicatorsPool class implementation
 */

/*
 * Copyright (C) 2010-2012 Oracle Corporation
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
#include <QTimer>
#include <QPainter>

/* GUI includes: */
#include "UIIndicatorsPool.h"
#include "VBoxGlobal.h"
#include "UIMachineDefs.h"
#include "UIConverter.h"
#include "UIAnimationFramework.h"
#include "UIMedium.h"

/* COM includes: */
#include "CConsole.h"
#include "CMachine.h"
#include "CSystemProperties.h"
#include "CMachineDebugger.h"
#include "CGuest.h"
#include "CStorageController.h"
#include "CMediumAttachment.h"
#include "CNetworkAdapter.h"
#include "CUSBController.h"
#include "CUSBDeviceFilters.h"
#include "CUSBDevice.h"
#include "CSharedFolder.h"
#include "CVRDEServer.h"

/* Other VBox includes: */
#include <iprt/time.h>

class UIIndicatorHardDisks : public QIStateIndicator
{
    Q_OBJECT;

public:

    UIIndicatorHardDisks(CSession &session)
      : m_session(session)
    {
        setStateIcon(KDeviceActivity_Idle, QPixmap(":/hd_16px.png"));
        setStateIcon(KDeviceActivity_Reading, QPixmap(":/hd_read_16px.png"));
        setStateIcon(KDeviceActivity_Writing, QPixmap(":/hd_write_16px.png"));
        setStateIcon(KDeviceActivity_Null, QPixmap(":/hd_disabled_16px.png"));

        retranslateUi();
    }

    void retranslateUi()
    {
        updateAppearance();
    }

    void updateAppearance()
    {
        const CMachine &machine = m_session.GetMachine();

        QString strToolTip = QApplication::translate("UIIndicatorsPool", "<p style='white-space:pre'><nobr>Indicates the activity "
                                                     "of the virtual hard disks:</nobr>%1</p>", "HDD tooltip");

        QString strFullData;
        bool fAttachmentsPresent = false;

        const CStorageControllerVector &controllers = machine.GetStorageControllers();
        foreach (const CStorageController &controller, controllers)
        {
            QString strAttData;
            const CMediumAttachmentVector &attachments = machine.GetMediumAttachmentsOfController(controller.GetName());
            foreach (const CMediumAttachment &attachment, attachments)
            {
                if (attachment.GetType() != KDeviceType_HardDisk)
                    continue;
                strAttData += QString("<br>&nbsp;<nobr>%1:&nbsp;%2</nobr>")
                    .arg(gpConverter->toString(StorageSlot(controller.GetBus(), attachment.GetPort(), attachment.GetDevice())))
                    .arg(UIMedium(attachment.GetMedium(), UIMediumType_HardDisk).location());
                fAttachmentsPresent = true;
            }
            if (!strAttData.isNull())
                strFullData += QString("<br><nobr><b>%1</b></nobr>").arg(controller.GetName()) + strAttData;
        }

        /* For now we will hide that LED at all if there are no attachments! */
        if (!fAttachmentsPresent)
            setHidden(true);
        //if (!fAttachmentsPresent)
        //    strFullData += QApplication::translate("UIIndicatorsPool", "<br><nobr><b>No hard disks attached</b></nobr>", "HDD tooltip");

        setToolTip(strToolTip.arg(strFullData));
        setState(fAttachmentsPresent ? KDeviceActivity_Idle : KDeviceActivity_Null);
    }

protected:
    /* For compatibility reason we do it here, later this should be moved to
     * QIStateIndicator. */
    CSession &m_session;
};

class UIIndicatorOpticalDisks : public QIStateIndicator
{
    Q_OBJECT;

public:

    UIIndicatorOpticalDisks(CSession &session)
      : m_session(session)
    {
        setStateIcon(KDeviceActivity_Idle, QPixmap(":/cd_16px.png"));
        setStateIcon(KDeviceActivity_Reading, QPixmap(":/cd_read_16px.png"));
        setStateIcon(KDeviceActivity_Writing, QPixmap(":/cd_write_16px.png"));
        setStateIcon(KDeviceActivity_Null, QPixmap(":/cd_disabled_16px.png"));

        retranslateUi();
    }

    void retranslateUi()
    {
        updateAppearance();
    }

    void updateAppearance()
    {
        const CMachine &machine = m_session.GetMachine();

        QString strToolTip = QApplication::translate("UIIndicatorsPool", "<p style='white-space:pre'><nobr>Indicates the activity "
                                                     "of the CD/DVD devices:</nobr>%1</p>", "CD/DVD tooltip");

        QString strFullData;
        bool fAttachmentsPresent = false;
        bool fAttachmentsMounted = false;

        const CStorageControllerVector &controllers = machine.GetStorageControllers();
        foreach (const CStorageController &controller, controllers)
        {
            QString strAttData;
            const CMediumAttachmentVector &attachments = machine.GetMediumAttachmentsOfController(controller.GetName());
            foreach (const CMediumAttachment &attachment, attachments)
            {
                if (attachment.GetType() != KDeviceType_DVD)
                    continue;
                UIMedium vboxMedium(attachment.GetMedium(), UIMediumType_DVD);
                strAttData += QString("<br>&nbsp;<nobr>%1:&nbsp;%2</nobr>")
                    .arg(gpConverter->toString(StorageSlot(controller.GetBus(), attachment.GetPort(), attachment.GetDevice())))
                    .arg(vboxMedium.isNull() || vboxMedium.isHostDrive() ? vboxMedium.name() : vboxMedium.location());
                fAttachmentsPresent = true;
                if (!vboxMedium.isNull())
                    fAttachmentsMounted = true;
            }
            if (!strAttData.isNull())
                strFullData += QString("<br><nobr><b>%1</b></nobr>").arg(controller.GetName()) + strAttData;
        }

        /* For now we will hide that LED at all if there are no attachments! */
        if (!fAttachmentsPresent)
            setHidden(true);
        //if (!fAttachmentsPresent)
        //    strFullData = QApplication::translate("UIIndicatorsPool", "<br><nobr><b>No CD/DVD devices attached</b></nobr>", "CD/DVD tooltip");

        setToolTip(strToolTip.arg(strFullData));
        setState(fAttachmentsMounted ? KDeviceActivity_Idle : KDeviceActivity_Null);
    }

protected:
    /* For compatibility reason we do it here, later this should be moved to
     * QIStateIndicator. */
    CSession &m_session;
};

class UIIndicatorFloppyDisks : public QIStateIndicator
{
    Q_OBJECT;

public:

    UIIndicatorFloppyDisks(CSession &session)
      : m_session(session)
    {
        setStateIcon(KDeviceActivity_Idle, QPixmap(":/fd_16px.png"));
        setStateIcon(KDeviceActivity_Reading, QPixmap(":/fd_read_16px.png"));
        setStateIcon(KDeviceActivity_Writing, QPixmap(":/fd_write_16px.png"));
        setStateIcon(KDeviceActivity_Null, QPixmap(":/fd_disabled_16px.png"));

        retranslateUi();
    }

    void retranslateUi()
    {
        updateAppearance();
    }

    void updateAppearance()
    {
        const CMachine &machine = m_session.GetMachine();

        QString strToolTip = QApplication::translate("UIIndicatorsPool", "<p style='white-space:pre'><nobr>Indicates the activity "
                                                     "of the floppy devices:</nobr>%1</p>", "FD tooltip");

        QString strFullData;
        bool fAttachmentsPresent = false;
        bool fAttachmentsMounted = false;

        const CStorageControllerVector &controllers = machine.GetStorageControllers();
        foreach (const CStorageController &controller, controllers)
        {
            QString strAttData;
            const CMediumAttachmentVector &attachments = machine.GetMediumAttachmentsOfController(controller.GetName());
            foreach (const CMediumAttachment &attachment, attachments)
            {
                if (attachment.GetType() != KDeviceType_Floppy)
                    continue;
                UIMedium vboxMedium(attachment.GetMedium(), UIMediumType_Floppy);
                strAttData += QString("<br>&nbsp;<nobr>%1:&nbsp;%2</nobr>")
                    .arg(gpConverter->toString(StorageSlot(controller.GetBus(), attachment.GetPort(), attachment.GetDevice())))
                    .arg(vboxMedium.isNull() || vboxMedium.isHostDrive() ? vboxMedium.name() : vboxMedium.location());
                fAttachmentsPresent = true;
                if (!vboxMedium.isNull())
                    fAttachmentsMounted = true;
            }
            if (!strAttData.isNull())
                strFullData += QString("<br><nobr><b>%1</b></nobr>").arg(controller.GetName()) + strAttData;
        }

        /* For now we will hide that LED at all if there are no attachments! */
        if (!fAttachmentsPresent)
            setHidden(true);
        //if (!fAttachmentsPresent)
        //    strFullData = QApplication::translate("UIIndicatorsPool", "<br><nobr><b>No floppy devices attached</b></nobr>", "FD tooltip");

        setToolTip(strToolTip.arg(strFullData));
        setState(fAttachmentsMounted ? KDeviceActivity_Idle : KDeviceActivity_Null);
    }

protected:
    /* For compatibility reason we do it here, later this should be moved to
     * QIStateIndicator. */
    CSession &m_session;
};

class UIIndicatorNetwork : public QIStateIndicator
{
    Q_OBJECT;

public:

    UIIndicatorNetwork(CSession &session)
      : m_session(session)
      , m_pUpdateTimer(new QTimer(this))
    {
        setStateIcon(KDeviceActivity_Idle, QPixmap(":/nw_16px.png"));
        setStateIcon(KDeviceActivity_Reading, QPixmap(":/nw_read_16px.png"));
        setStateIcon(KDeviceActivity_Writing, QPixmap(":/nw_write_16px.png"));
        setStateIcon(KDeviceActivity_Null, QPixmap(":/nw_disabled_16px.png"));

        connect(m_pUpdateTimer, SIGNAL(timeout()), SLOT(sltUpdateNetworkIPs()));
        m_pUpdateTimer->start(5000);

        retranslateUi();
    }

    void retranslateUi()
    {
        updateAppearance();
    }

    void updateAppearance()
    {
        const CMachine &machine = m_session.GetMachine();
        QString strFullData;

        ulong uMaxCount = vboxGlobal().virtualBox().GetSystemProperties().GetMaxNetworkAdapters(KChipsetType_PIIX3);

        QString strToolTip = QApplication::translate("UIIndicatorsPool",
                                 "<p style='white-space:pre'><nobr>Indicates the activity of the "
                                 "network interfaces:</nobr>%1</p>", "Network adapters tooltip");

        RTTIMESPEC time;
        uint64_t u64Now = RTTimeSpecGetNano(RTTimeNow(&time));

        QString strFlags, strCount;
        LONG64 iTimestamp;
        machine.GetGuestProperty("/VirtualBox/GuestInfo/Net/Count", strCount, iTimestamp, strFlags);
        bool fPropsValid = (u64Now - iTimestamp < UINT64_C(60000000000)); /* timeout beacon */

        QStringList ipList, macList;
        if (fPropsValid)
        {
            int cAdapters = RT_MIN(strCount.toInt(), (int)uMaxCount);
            for (int i = 0; i < cAdapters; ++i)
            {
                ipList << machine.GetGuestPropertyValue(QString("/VirtualBox/GuestInfo/Net/%1/V4/IP").arg(i));
                macList << machine.GetGuestPropertyValue(QString("/VirtualBox/GuestInfo/Net/%1/MAC").arg(i));
            }
        }

        ulong uEnabled = 0;
        for (ulong uSlot = 0; uSlot < uMaxCount; ++uSlot)
        {
            const CNetworkAdapter &adapter = machine.GetNetworkAdapter(uSlot);
            if (adapter.GetEnabled())
            {
                QString strGuestIp;
                if (fPropsValid)
                {
                    QString strGuestMac = adapter.GetMACAddress();
                    int iIp = macList.indexOf(strGuestMac);
                    if (iIp >= 0)
                        strGuestIp = ipList[iIp];
                }
                strFullData += QApplication::translate("UIIndicatorsPool",
                               "<br><nobr><b>Adapter %1 (%2)</b>: %3 cable %4</nobr>", "Network adapters tooltip")
                    .arg(uSlot + 1)
                    .arg(gpConverter->toString(adapter.GetAttachmentType()))
                    .arg(strGuestIp.isEmpty() ? "" : "IP " + strGuestIp + ", ")
                    .arg(adapter.GetCableConnected() ?
                         QApplication::translate("UIIndicatorsPool", "connected", "Network adapters tooltip") :
                         QApplication::translate("UIIndicatorsPool", "disconnected", "Network adapters tooltip"));
                ++uEnabled;
            }
        }

        setState(uEnabled > 0 ? KDeviceActivity_Idle : KDeviceActivity_Null);
        if (!uEnabled)
            setHidden(true);

        if (strFullData.isNull())
            strFullData = QApplication::translate("UIIndicatorsPool",
                              "<br><nobr><b>All network adapters are disabled</b></nobr>", "Network adapters tooltip");

        setToolTip(strToolTip.arg(strFullData));
    }
protected slots:

    void sltUpdateNetworkIPs()
    {
        updateAppearance();
    }

protected:
    /* For compatibility reason we do it here, later this should be moved to
     * QIStateIndicator. */
    CSession &m_session;
    QTimer *m_pUpdateTimer;
};

class UIIndicatorUSB : public QIStateIndicator
{
    Q_OBJECT;

public:

    UIIndicatorUSB(CSession &session)
      : m_session(session)
    {
        setStateIcon(KDeviceActivity_Idle, QPixmap(":/usb_16px.png"));
        setStateIcon(KDeviceActivity_Reading, QPixmap(":/usb_read_16px.png"));
        setStateIcon(KDeviceActivity_Writing, QPixmap(":/usb_write_16px.png"));
        setStateIcon(KDeviceActivity_Null, QPixmap(":/usb_disabled_16px.png"));

        retranslateUi();
    }

    void retranslateUi()
    {
        updateAppearance();
    }

    void updateAppearance()
    {
        const CMachine &machine = m_session.GetMachine();

        QString strToolTip = QApplication::translate("UIIndicatorsPool", "<p style='white-space:pre'><nobr>Indicates the activity of "
                                "the attached USB devices:</nobr>%1</p>", "USB device tooltip");
        QString strFullData;


        /*
         * Check whether there is at least one OHCI USB controllers with
         * an available proxy.
         */
        const CUSBDeviceFilters &filters = machine.GetUSBDeviceFilters();
        ULONG cOhciCtls = machine.GetUSBControllerCountByType(KUSBControllerType_OHCI);
        bool fUSBEnabled = !filters.isNull() && cOhciCtls && machine.GetUSBProxyAvailable();

        setState(fUSBEnabled ? KDeviceActivity_Idle : KDeviceActivity_Null);
        if (fUSBEnabled)
        {
            const CConsole &console = m_session.GetConsole();

            const CUSBDeviceVector &devsvec = console.GetUSBDevices();
            for (int i = 0; i < devsvec.size(); ++ i)
            {
                CUSBDevice usb = devsvec[i];
                strFullData += QString("<br><b><nobr>%1</nobr></b>").arg(vboxGlobal().details(usb));
            }
            if (strFullData.isNull())
                strFullData = QApplication::translate("UIIndicatorsPool", "<br><nobr><b>No USB devices attached</b></nobr>", "USB device tooltip");
        }
        else
            strFullData = QApplication::translate("UIIndicatorsPool", "<br><nobr><b>USB Controller is disabled</b></nobr>", "USB device tooltip");

        setToolTip(strToolTip.arg(strFullData));
    }

protected:
    /* For compatibility reason we do it here, later this should be moved to
     * QIStateIndicator. */
    CSession &m_session;
};

class UIIndicatorSharedFolders : public QIStateIndicator
{
    Q_OBJECT;

public:

    UIIndicatorSharedFolders(CSession &session)
      : m_session(session)
    {
        setStateIcon(KDeviceActivity_Idle, QPixmap(":/sf_16px.png"));
        setStateIcon(KDeviceActivity_Reading, QPixmap(":/sf_read_16px.png"));
        setStateIcon(KDeviceActivity_Writing, QPixmap(":/sf_write_16px.png"));
        setStateIcon(KDeviceActivity_Null, QPixmap(":/sf_disabled_16px.png"));

        retranslateUi();
    }

    void retranslateUi()
    {
        updateAppearance();
    }

    void updateAppearance()
    {
        const CMachine &machine = m_session.GetMachine();
        const CConsole &console = m_session.GetConsole();

        QString strToolTip = QApplication::translate("UIIndicatorsPool", "<p style='white-space:pre'><nobr>Indicates the activity of "
                                "the machine's shared folders:</nobr>%1</p>", "Shared folders tooltip");

        QString strFullData;
        QMap<QString, QString> sfs;

        /* Permanent folders */
        const CSharedFolderVector &psfvec = machine.GetSharedFolders();

        for (int i = 0; i < psfvec.size(); ++ i)
        {
            const CSharedFolder &sf = psfvec[i];
            sfs.insert(sf.GetName(), sf.GetHostPath());
        }

        /* Transient folders */
        const CSharedFolderVector &tsfvec = console.GetSharedFolders();

        for (int i = 0; i < tsfvec.size(); ++ i)
        {
            const CSharedFolder &sf = tsfvec[i];
            sfs.insert(sf.GetName(), sf.GetHostPath());
        }

        for (QMap<QString, QString>::const_iterator it = sfs.constBegin(); it != sfs.constEnd(); ++ it)
        {
            /* Select slashes depending on the OS type */
            if (VBoxGlobal::isDOSType(console.GetGuest().GetOSTypeId()))
                strFullData += QString("<br><nobr><b>\\\\vboxsvr\\%1&nbsp;</b></nobr><nobr>%2</nobr>")
                                       .arg(it.key(), it.value());
            else
                strFullData += QString("<br><nobr><b>%1&nbsp;</b></nobr><nobr>%2</nobr>")
                                       .arg(it.key(), it.value());
        }

        if (sfs.count() == 0)
            strFullData = QApplication::translate("UIIndicatorsPool", "<br><nobr><b>No shared folders</b></nobr>", "Shared folders tooltip");

        setState(!sfs.isEmpty() ? KDeviceActivity_Idle : KDeviceActivity_Null);
        setToolTip(strToolTip.arg(strFullData));
    }

protected:
    /* For compatibility reason we do it here, later this should be moved to
     * QIStateIndicator. */
    CSession &m_session;
};

class UIIndicatorVideoCapture : public QIStateIndicator
{
    Q_OBJECT;
    Q_PROPERTY(double rotationAngleStart READ rotationAngleStart);
    Q_PROPERTY(double rotationAngleFinal READ rotationAngleFinal);
    Q_PROPERTY(double rotationAngle READ rotationAngle WRITE setRotationAngle);

public:

    /* State enumerator: */
    enum UIIndicatorStateVideoCapture
    {
        UIIndicatorStateVideoCapture_Disabled = 0,
        UIIndicatorStateVideoCapture_Enabled  = 1
    };

    /* Constructor: */
    UIIndicatorVideoCapture(CSession &session)
        : m_session(session)
        , m_pAnimation(0)
        , m_dRotationAngle(0)
    {
        /* Assign state icons: */
        setStateIcon(UIIndicatorStateVideoCapture_Disabled, QPixmap(":/video_capture_16px.png"));
        setStateIcon(UIIndicatorStateVideoCapture_Enabled, QPixmap(":/movie_reel_16px.png"));

        /* Prepare *enabled* state animation: */
        m_pAnimation = UIAnimationLoop::installAnimationLoop(this, "rotationAngle",
                                                                   "rotationAngleStart", "rotationAngleFinal",
                                                                   1000);

        /* Translate finally: */
        retranslateUi();
    }

    /* API: Update stuff: */
    void updateAppearance()
    {
        /* Get machine: */
        CMachine machine = m_session.GetMachine();

        /* Update LED state: */
        setState(machine.GetVideoCaptureEnabled());

        /* Update LED tool-tip: */
        QString strToolTip = QApplication::translate("UIIndicatorsPool", "<nobr>Indicates video capturing activity:</nobr><br>%1");
        switch (state())
        {
            case UIIndicatorStateVideoCapture_Disabled:
            {
                strToolTip = strToolTip.arg(QApplication::translate("UIIndicatorsPool", "<nobr><b>Video capture disabled</b></nobr>"));
                break;
            }
            case UIIndicatorStateVideoCapture_Enabled:
            {
                strToolTip = strToolTip.arg(QApplication::translate("UIIndicatorsPool", "<nobr><b>Video capture file:</b> %1</nobr>"));
                strToolTip = strToolTip.arg(machine.GetVideoCaptureFile());
                break;
            }
            default:
                break;
        }
        setToolTip(strToolTip);
    }

public slots:

    /* Handler: State stuff: */
    void setState(int iState)
    {
        /* Update animation state: */
        switch (iState)
        {
            case UIIndicatorStateVideoCapture_Disabled:
                m_pAnimation->stop();
                m_dRotationAngle = 0;
                break;
            case UIIndicatorStateVideoCapture_Enabled:
                m_pAnimation->start();
                break;
            default:
                break;
        }

        /* Call to base-class: */
        QIStateIndicator::setState(iState);
    }

protected:

    /* Handler: Translate stuff: */
    void retranslateUi()
    {
        updateAppearance();
    }

    /* Handler: Paint stuff: */
    void paintEvent(QPaintEvent*)
    {
        /* Create new painter: */
        QPainter painter(this);
        /* Configure painter for *enabled* state: */
        if (state() == UIIndicatorStateVideoCapture_Enabled)
        {
            /* Configure painter for smooth animation: */
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            /* Shift rotation origin according pixmap center: */
            painter.translate(height() / 2, height() / 2);
            /* Rotate painter: */
            painter.rotate(rotationAngle());
            /* Unshift rotation origin according pixmap center: */
            painter.translate(- height() / 2, - height() / 2);
        }
        /* Draw contents: */
        drawContents(&painter);
    }

    /* Properties: Rotation stuff: */
    double rotationAngleStart() const { return 0; }
    double rotationAngleFinal() const { return 360; }
    double rotationAngle() const { return m_dRotationAngle; }
    void setRotationAngle(double dRotationAngle) { m_dRotationAngle = dRotationAngle; update(); }

    /* For compatibility reason we do it here,
     * later this should be moved to QIStateIndicator. */
    CSession &m_session;
    UIAnimationLoop *m_pAnimation;
    double m_dRotationAngle;
};

class UIIndicatorFeatures : public QIStateIndicator
{
    Q_OBJECT;

public:

    UIIndicatorFeatures(CSession &session)
      : m_session(session)
    {
        setStateIcon(0, QPixmap(":/vtx_amdv_disabled_16px.png"));
        setStateIcon(1, QPixmap(":/vtx_amdv_16px.png"));

        retranslateUi();
    }

    void retranslateUi()
    {
        updateAppearance();
    }

    void updateAppearance()
    {
        const CConsole &console = m_session.GetConsole();
        if (console.isNull())
            return;

        const CMachineDebugger &debugger = console.GetDebugger();
        if (debugger.isNull())
            return;

        bool bVirtEnabled = debugger.GetHWVirtExEnabled();
        QString virtualization = bVirtEnabled ?
            VBoxGlobal::tr("Enabled", "details report (VT-x/AMD-V)") :
            VBoxGlobal::tr("Disabled", "details report (VT-x/AMD-V)");

        bool bNestEnabled = debugger.GetHWVirtExNestedPagingEnabled();
        QString nestedPaging = bNestEnabled ?
            VBoxGlobal::tr("Enabled", "details report (Nested Paging)") :
            VBoxGlobal::tr("Disabled", "details report (Nested Paging)");

        bool bUXEnabled = debugger.GetHWVirtExUXEnabled();
        QString unrestrictExec = bUXEnabled ?
            VBoxGlobal::tr("Enabled", "details report (Unrestricted Execution)") :
            VBoxGlobal::tr("Disabled", "details report (Unrestricted Execution)");

        QString strCPUExecCap = QString::number(console.GetMachine().GetCPUExecutionCap());

        QString tip(QApplication::translate("UIIndicatorsPool",
                                            "Additional feature status:"
                                            "<br><nobr><b>%1:</b>&nbsp;%2</nobr>"
                                            "<br><nobr><b>%3:</b>&nbsp;%4</nobr>"
                                            "<br><nobr><b>%5:</b>&nbsp;%6</nobr>"
                                            "<br><nobr><b>%7:</b>&nbsp;%8%</nobr>",
                                            "Virtualization Stuff LED")
                    .arg(VBoxGlobal::tr("VT-x/AMD-V", "details report"), virtualization)
                    .arg(VBoxGlobal::tr("Nested Paging"), nestedPaging)
                    .arg(VBoxGlobal::tr("Unrestricted Execution"), unrestrictExec)
                    .arg(VBoxGlobal::tr("Execution Cap", "details report"), strCPUExecCap));

        int cpuCount = console.GetMachine().GetCPUCount();
        if (cpuCount > 1)
            tip += QApplication::translate("UIIndicatorsPool", "<br><nobr><b>%1:</b>&nbsp;%2</nobr>", "Virtualization Stuff LED")
                      .arg(VBoxGlobal::tr("Processor(s)", "details report")).arg(cpuCount);

        setToolTip(tip);
        setState(bVirtEnabled);
    }

protected:
    /* For compatibility reason we do it here, later this should be moved to
     * QIStateIndicator. */
    CSession &m_session;
};

class UIIndicatorMouse : public QIStateIndicator
{
    Q_OBJECT;

public:

    UIIndicatorMouse(CSession &session)
      : m_session(session)
    {
        setStateIcon(0, QPixmap(":/mouse_disabled_16px.png"));
        setStateIcon(1, QPixmap(":/mouse_16px.png"));
        setStateIcon(2, QPixmap(":/mouse_seamless_16px.png"));
        setStateIcon(3, QPixmap(":/mouse_can_seamless_16px.png"));
        setStateIcon(4, QPixmap(":/mouse_can_seamless_uncaptured_16px.png"));

        retranslateUi();
    }

    void retranslateUi()
    {
        setToolTip(QApplication::translate("UIIndicatorsPool", "Indicates whether the host mouse pointer is captured by the guest OS:<br>"
                      "<nobr><img src=:/mouse_disabled_16px.png/>&nbsp;&nbsp;pointer is not captured</nobr><br>"
                      "<nobr><img src=:/mouse_16px.png/>&nbsp;&nbsp;pointer is captured</nobr><br>"
                      "<nobr><img src=:/mouse_seamless_16px.png/>&nbsp;&nbsp;mouse integration (MI) is On</nobr><br>"
                      "<nobr><img src=:/mouse_can_seamless_16px.png/>&nbsp;&nbsp;MI is Off, pointer is captured</nobr><br>"
                      "<nobr><img src=:/mouse_can_seamless_uncaptured_16px.png/>&nbsp;&nbsp;MI is Off, pointer is not captured</nobr><br>"
                      "Note that the mouse integration feature requires Guest Additions to be installed in the guest OS."));
    }

public slots:

    void setState(int iState)
    {
        if ((iState & UIMouseStateType_MouseAbsoluteDisabled) &&
            (iState & UIMouseStateType_MouseAbsolute) &&
            !(iState & UIMouseStateType_MouseCaptured))
        {
            QIStateIndicator::setState(4);
        }
        else
        {
            QIStateIndicator::setState(iState & (UIMouseStateType_MouseAbsolute | UIMouseStateType_MouseCaptured));
        }
    }

protected:
    /* For compatibility reason we do it here, later this should be moved to
     * QIStateIndicator. */
    CSession &m_session;
};

class UIIndicatorKeyboard : public QIStateIndicator
{
    Q_OBJECT;

public:

    UIIndicatorKeyboard(CSession &session)
      : m_session(session)
    {
        setStateIcon(0, QPixmap(":/hostkey_16px.png"));
        setStateIcon(1, QPixmap(":/hostkey_captured_16px.png"));
        setStateIcon(2, QPixmap(":/hostkey_pressed_16px.png"));
        setStateIcon(3, QPixmap(":/hostkey_captured_pressed_16px.png"));

        retranslateUi();
    }

    void retranslateUi()
    {
        setToolTip(QApplication::translate("UIIndicatorsPool", "Indicates whether the keyboard is captured by the guest OS "
                      "(<img src=:/hostkey_captured_16px.png/>) or not (<img src=:/hostkey_16px.png/>)."));
    }

protected:
    /* For compatibility reason we do it here, later this should be moved to
     * QIStateIndicator. */
    CSession &m_session;
};

UIIndicatorsPool::UIIndicatorsPool(CSession &session, QObject *pParent)
    : QObject(pParent)
    , m_session(session)
    , m_pool(IndicatorType_Max)
{
    /* Prepare: */
    prepare();
}

UIIndicatorsPool::~UIIndicatorsPool()
{
    /* Cleanup: */
    cleanup();
}

QIStateIndicator* UIIndicatorsPool::indicator(IndicatorType index)
{
    /* Just return what already exists: */
    return m_pool[index];
}

void UIIndicatorsPool::prepare()
{
    /* Get the list of restricted indicators: */
    CMachine machine = m_session.GetMachine();
    QList<IndicatorType> restrictedIndicators = vboxGlobal().restrictedStatusBarIndicators(machine);

    /* Populate indicator-pool: */
    for (int iIndex = 0; iIndex < IndicatorType_Max; ++iIndex)
    {
        /* Make sure indicator presence is permitted: */
        IndicatorType index = static_cast<IndicatorType>(iIndex);
        if (restrictedIndicators.contains(index))
            continue;

        /* Prepare indicator: */
        switch (index)
        {
            case IndicatorType_HardDisks:     m_pool[index] = new UIIndicatorHardDisks(m_session); break;
            case IndicatorType_OpticalDisks:  m_pool[index] = new UIIndicatorOpticalDisks(m_session); break;
            case IndicatorType_FloppyDisks:   m_pool[index] = new UIIndicatorFloppyDisks(m_session); break;
            case IndicatorType_Network:       m_pool[index] = new UIIndicatorNetwork(m_session); break;
            case IndicatorType_USB:           m_pool[index] = new UIIndicatorUSB(m_session); break;
            case IndicatorType_SharedFolders: m_pool[index] = new UIIndicatorSharedFolders(m_session); break;
            case IndicatorType_VideoCapture:  m_pool[index] = new UIIndicatorVideoCapture(m_session); break;
            case IndicatorType_Features:      m_pool[index] = new UIIndicatorFeatures(m_session); break;
            case IndicatorType_Mouse:         m_pool[index] = new UIIndicatorMouse(m_session); break;
            case IndicatorType_Keyboard:      m_pool[index] = new UIIndicatorKeyboard(m_session); break;
            default: break;
        }
    }
}

void UIIndicatorsPool::cleanup()
{
    /* Wipe-out indicator-pool: */
    for (int iIndex = 0; iIndex < IndicatorType_Max; ++iIndex)
    {
        /* Wipe-out indicator: */
        delete m_pool[iIndex];
        m_pool[iIndex] = 0;
    }
}

#include "UIIndicatorsPool.moc"
