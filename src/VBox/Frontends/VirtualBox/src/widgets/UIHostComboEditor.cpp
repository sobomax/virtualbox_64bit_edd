/* $Id: UIHostComboEditor.cpp $ */
/** @file
 *
 * VBox frontends: Qt GUI ("VirtualBox"):
 * VirtualBox Qt extensions: UIHostComboEditor class implementation
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
#include <QApplication>
#include <QStyleOption>
#include <QStylePainter>
#include <QKeyEvent>
#include <QTimer>

/* GUI includes: */
#include "UIHostComboEditor.h"
#include "VBoxGlobal.h"
#include "QIToolButton.h"
#include "UIIconPool.h"

#ifdef Q_WS_WIN
# undef LOWORD
# undef HIWORD
# undef LOBYTE
# undef HIBYTE
# include <windows.h>
# include "WinKeyboard.h"
#endif /* Q_WS_WIN */

#ifdef Q_WS_X11
# include <X11/Xlib.h>
# include <X11/Xutil.h>
# include <X11/keysym.h>
# ifdef KeyPress
   const int XKeyPress = KeyPress;
   const int XKeyRelease = KeyRelease;
#  undef KeyPress
#  undef KeyRelease
# endif /* KeyPress */
# include "XKeyboard.h"
# include <QX11Info>
#endif /* Q_WS_X11 */

#ifdef Q_WS_MAC
# include "UICocoaApplication.h"
# include "DarwinKeyboard.h"
# include "VBoxUtils.h"
# include <Carbon/Carbon.h>
#endif /* Q_WS_MAC */


#ifdef Q_WS_X11
namespace UINativeHotKey
{
    QMap<QString, QString> m_keyNames;
}
#endif /* Q_WS_X11 */

QString UINativeHotKey::toString(int iKeyCode)
{
    QString strKeyName;

#ifdef Q_WS_WIN
    /* MapVirtualKey doesn't distinguish between right and left vkeys,
     * even under XP, despite that it stated in MSDN. Do it by hands.
     * Besides that it can't recognize such virtual keys as
     * VK_DIVIDE & VK_PAUSE, this is also known bug. */
    int iScan;
    switch (iKeyCode)
    {
        /* Processing special keys... */
        case VK_PAUSE: iScan = 0x45 << 16; break;
        case VK_RSHIFT: iScan = 0x36 << 16; break;
        case VK_RCONTROL: iScan = (0x1D << 16) | (1 << 24); break;
        case VK_RMENU: iScan = (0x38 << 16) | (1 << 24); break;
        /* Processing extended keys... */
        case VK_APPS:
        case VK_LWIN:
        case VK_RWIN:
        case VK_NUMLOCK: iScan = (::MapVirtualKey(iKeyCode, 0) | 256) << 16; break;
        default: iScan = ::MapVirtualKey(iKeyCode, 0) << 16;
    }
    TCHAR *pKeyName = new TCHAR[256];
    if (::GetKeyNameText(iScan, pKeyName, 256))
    {
        strKeyName = QString::fromUtf16(pKeyName);
    }
    else
    {
        AssertMsgFailed(("That key have no name!\n"));
        strKeyName = UIHostComboEditor::tr("<key_%1>").arg(iKeyCode);
    }
    delete[] pKeyName;
#endif /* Q_WS_WIN */

#ifdef Q_WS_X11
    if (char *pNativeKeyName = ::XKeysymToString((KeySym)iKeyCode))
    {
        strKeyName = m_keyNames[pNativeKeyName].isEmpty() ?
                     QString(pNativeKeyName) : m_keyNames[pNativeKeyName];
    }
    else
    {
        AssertMsgFailed(("That key have no name!\n"));
        strKeyName = UIHostComboEditor::tr("<key_%1>").arg(iKeyCode);
    }
#endif /* Q_WS_X11 */

#ifdef Q_WS_MAC
    UInt32 modMask = DarwinKeyCodeToDarwinModifierMask(iKeyCode);
    switch (modMask)
    {
        case shiftKey:
        case optionKey:
        case controlKey:
        case cmdKey:
            strKeyName = UIHostComboEditor::tr("Left ");
            break;
        case rightShiftKey:
        case rightOptionKey:
        case rightControlKey:
        case kEventKeyModifierRightCmdKeyMask:
            strKeyName = UIHostComboEditor::tr("Right ");
            break;
        default:
            AssertMsgFailedReturn(("modMask=%#x\n", modMask), QString());
    }
    switch (modMask)
    {
        case shiftKey:
        case rightShiftKey:
            strKeyName += QChar(kShiftUnicode);
            break;
        case optionKey:
        case rightOptionKey:
            strKeyName += QChar(kOptionUnicode);
            break;
        case controlKey:
        case rightControlKey:
            strKeyName += QChar(kControlUnicode);
            break;
        case cmdKey:
        case kEventKeyModifierRightCmdKeyMask:
            strKeyName += QChar(kCommandUnicode);
            break;
    }
#endif /* Q_WS_MAC */

    return strKeyName;
}

bool UINativeHotKey::isValidKey(int iKeyCode)
{
#ifdef Q_WS_WIN
    return ((iKeyCode >= VK_SHIFT && iKeyCode <= VK_CAPITAL) ||
            (iKeyCode >= VK_LSHIFT && iKeyCode <= VK_RMENU) ||
            (iKeyCode >= VK_F1 && iKeyCode <= VK_F24) ||
            iKeyCode == VK_NUMLOCK || iKeyCode == VK_SCROLL ||
            iKeyCode == VK_LWIN || iKeyCode == VK_RWIN ||
            iKeyCode == VK_APPS ||
            iKeyCode == VK_PRINT);
#endif /* Q_WS_WIN */

#ifdef Q_WS_X11
    return (IsModifierKey(iKeyCode) /* allow modifiers */ ||
            IsFunctionKey(iKeyCode) /* allow function keys */ ||
            IsMiscFunctionKey(iKeyCode) /* allow miscellaneous function keys */ ||
            iKeyCode == XK_Scroll_Lock /* allow 'Scroll Lock' missed in IsModifierKey() */) &&
           (iKeyCode != NoSymbol /* ignore some special symbol */ &&
            iKeyCode != XK_Insert /* ignore 'insert' included into IsMiscFunctionKey */);
#endif /* Q_WS_X11 */

#ifdef Q_WS_MAC
    UInt32 modMask = ::DarwinKeyCodeToDarwinModifierMask(iKeyCode);
    switch (modMask)
    {
        case shiftKey:
        case optionKey:
        case controlKey:
        case rightShiftKey:
        case rightOptionKey:
        case rightControlKey:
        case cmdKey:
        case kEventKeyModifierRightCmdKeyMask:
            return true;
        default:
            return false;
    }
#endif /* Q_WS_MAC */

    return false;
}

#ifdef Q_WS_WIN
int UINativeHotKey::distinguishModifierVKey(int wParam, int lParam)
{
    int iKeyCode = wParam;
    switch (iKeyCode)
    {
        case VK_SHIFT:
        {
            UINT uCurrentScanCode = (lParam & 0x01FF0000) >> 16;
            UINT uLeftScanCode = ::MapVirtualKey(iKeyCode, 0);
            if (uCurrentScanCode == uLeftScanCode)
                iKeyCode = VK_LSHIFT;
            else
                iKeyCode = VK_RSHIFT;
            break;
        }
        case VK_CONTROL:
        {
            UINT uCurrentScanCode = (lParam & 0x01FF0000) >> 16;
            UINT uLeftScanCode = ::MapVirtualKey(iKeyCode, 0);
            if (uCurrentScanCode == uLeftScanCode)
                iKeyCode = VK_LCONTROL;
            else
                iKeyCode = VK_RCONTROL;
            break;
        }
        case VK_MENU:
        {
            UINT uCurrentScanCode = (lParam & 0x01FF0000) >> 16;
            UINT uLeftScanCode = ::MapVirtualKey(iKeyCode, 0);
            if (uCurrentScanCode == uLeftScanCode)
                iKeyCode = VK_LMENU;
            else
                iKeyCode = VK_RMENU;
            break;
        }
    }
    return iKeyCode;
}
#endif /* Q_WS_WIN */

#ifdef Q_WS_X11
void UINativeHotKey::retranslateKeyNames()
{
    m_keyNames["Shift_L"]          = UIHostComboEditor::tr("Left Shift");
    m_keyNames["Shift_R"]          = UIHostComboEditor::tr("Right Shift");
    m_keyNames["Control_L"]        = UIHostComboEditor::tr("Left Ctrl");
    m_keyNames["Control_R"]        = UIHostComboEditor::tr("Right Ctrl");
    m_keyNames["Alt_L"]            = UIHostComboEditor::tr("Left Alt");
    m_keyNames["Alt_R"]            = UIHostComboEditor::tr("Right Alt");
    m_keyNames["Super_L"]          = UIHostComboEditor::tr("Left WinKey");
    m_keyNames["Super_R"]          = UIHostComboEditor::tr("Right WinKey");
    m_keyNames["Menu"]             = UIHostComboEditor::tr("Menu key");
    m_keyNames["ISO_Level3_Shift"] = UIHostComboEditor::tr("Alt Gr");
    m_keyNames["Caps_Lock"]        = UIHostComboEditor::tr("Caps Lock");
    m_keyNames["Scroll_Lock"]      = UIHostComboEditor::tr("Scroll Lock");
}
#endif /* Q_WS_X11 */


namespace UIHostCombo
{
    int m_iMaxComboSize = 3;
}

int UIHostCombo::hostComboModifierIndex()
{
    return -1;
}

QString UIHostCombo::hostComboModifierName()
{
    return UIHostComboEditor::tr("Host+");
}

QString UIHostCombo::hostComboCacheKey()
{
    return QString(GUI_Input_MachineShortcuts) + "/" + "HostCombo";
}

QString UIHostCombo::toReadableString(const QString &strKeyCombo)
{
    QStringList encodedKeyList = strKeyCombo.split(',');
    QStringList readableKeyList;
    for (int i = 0; i < encodedKeyList.size(); ++i)
        if (int iKeyCode = encodedKeyList[i].toInt())
            readableKeyList << UINativeHotKey::toString(iKeyCode);
    return readableKeyList.isEmpty() ? UIHostComboEditor::tr("None") : readableKeyList.join(" + ");
}

QList<int> UIHostCombo::toKeyCodeList(const QString &strKeyCombo)
{
    QStringList encodedKeyList = strKeyCombo.split(',');
    QList<int> keyCodeList;
    for (int i = 0; i < encodedKeyList.size(); ++i)
        if (int iKeyCode = encodedKeyList[i].toInt())
            keyCodeList << iKeyCode;
    return keyCodeList;
}

bool UIHostCombo::isValidKeyCombo(const QString &strKeyCombo)
{
    QList<int> keyCodeList = toKeyCodeList(strKeyCombo);
    if (keyCodeList.size() > m_iMaxComboSize)
        return false;
    for (int i = 0; i < keyCodeList.size(); ++i)
        if (!UINativeHotKey::isValidKey(keyCodeList[i]))
            return false;
    return true;
}


UIHostComboEditor::UIHostComboEditor(QWidget *pParent)
    : QIWithRetranslateUI<QWidget>(pParent)
{
    /* Prepare: */
    prepare();
}

void UIHostComboEditor::sltCommitData()
{
    /* Commit data to the listener: */
    emit sigCommitData(this);
}

void UIHostComboEditor::prepare()
{
    /* Configure self: */
    setAutoFillBackground(true);
    /* Create layout: */
    QHBoxLayout *pLayout = new QHBoxLayout(this);
    {
        /* Configure layout: */
        pLayout->setSpacing(4);
        pLayout->setContentsMargins(0, 0, 0, 0);
        /* Create UIHostComboEditorPrivate instance: */
        m_pEditor = new UIHostComboEditorPrivate;
        {
            /* Configure UIHostComboEditorPrivate instance: */
            setFocusProxy(m_pEditor);
            connect(m_pEditor, SIGNAL(sigDataChanged()), this, SLOT(sltCommitData()));
        }
        /* Create 'clear' tool-button: */
        m_pButtonClear = new QIToolButton;
        {
            /* Configure 'clear' tool-button: */
            m_pButtonClear->removeBorder();
            m_pButtonClear->setIcon(UIIconPool::iconSet(":/eraser_16px.png"));
            connect(m_pButtonClear, SIGNAL(clicked(bool)), m_pEditor, SLOT(sltClear()));
        }
        /* Add widgets to layout: */
        pLayout->addWidget(m_pEditor);
        pLayout->addWidget(m_pButtonClear);
    }
    /* Translate finally: */
    retranslateUi();
}

void UIHostComboEditor::retranslateUi()
{
    /* Translate 'clear' tool-button: */
    m_pButtonClear->setToolTip(QApplication::translate("UIHotKeyEditor", "Unset shortcut"));
}

void UIHostComboEditor::setCombo(const UIHostComboWrapper &strCombo)
{
    /* Pass combo to child: */
    m_pEditor->setCombo(strCombo);
}

UIHostComboWrapper UIHostComboEditor::combo() const
{
    /* Acquire combo from child: */
    return m_pEditor->combo();
}


UIHostComboEditorPrivate::UIHostComboEditorPrivate()
    : m_pReleaseTimer(0)
    , m_fStartNewSequence(true)
#ifdef Q_WS_WIN
    , m_pAltGrMonitor(0)
#endif /* Q_WS_WIN */
{
    /* Configure widget: */
    setAttribute(Qt::WA_NativeWindow);
    setContextMenuPolicy(Qt::NoContextMenu);
    setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Expanding);
    connect(this, SIGNAL(selectionChanged()), this, SLOT(sltDeselect()));

    /* Setup release-pending-keys timer: */
    m_pReleaseTimer = new QTimer(this);
    m_pReleaseTimer->setInterval(200);
    connect(m_pReleaseTimer, SIGNAL(timeout()), this, SLOT(sltReleasePendingKeys()));

#if defined(Q_WS_X11)
     /* Initialize the X keyboard subsystem: */
     initMappedX11Keyboard(QX11Info::display(), vboxGlobal().settings().publicProperty("GUI/RemapScancodes"));
#elif defined(Q_WS_MAC)
     m_uDarwinKeyModifiers = 0;
     UICocoaApplication::instance()->registerForNativeEvents(RT_BIT_32(10) | RT_BIT_32(11) | RT_BIT_32(12) /* NSKeyDown  | NSKeyUp | | NSFlagsChanged */, UIHostComboEditorPrivate::darwinEventHandlerProc, this);
     ::DarwinGrabKeyboard(false /* just modifiers */);
#elif defined(Q_WS_WIN)
    /* Prepare AltGR monitor: */
    m_pAltGrMonitor = new WinAltGrMonitor;
#endif /* Q_WS_WIN */
}

UIHostComboEditorPrivate::~UIHostComboEditorPrivate()
{
#if defined(Q_WS_WIN)
    /* Cleanup AltGR monitor: */
    delete m_pAltGrMonitor;
    m_pAltGrMonitor = 0;
#elif defined(Q_WS_MAC)
    ::DarwinReleaseKeyboard();
    UICocoaApplication::instance()->unregisterForNativeEvents(RT_BIT_32(10) | RT_BIT_32(11) | RT_BIT_32(12) /* NSKeyDown  | NSKeyUp | | NSFlagsChanged */, UIHostComboEditorPrivate::darwinEventHandlerProc, this);
#endif /* Q_WS_MAC */
}

void UIHostComboEditorPrivate::setCombo(const UIHostComboWrapper &strCombo)
{
    /* Cleanup old combo: */
    m_shownKeys.clear();
    /* Parse newly passed combo: */
    QList<int> keyCodeList = UIHostCombo::toKeyCodeList(strCombo.toString());
    for (int i = 0; i < keyCodeList.size(); ++i)
        if (int iKeyCode = keyCodeList[i])
            m_shownKeys.insert(iKeyCode, UINativeHotKey::toString(iKeyCode));
    /* Update text: */
    updateText();
}

UIHostComboWrapper UIHostComboEditorPrivate::combo() const
{
    /* Compose current combination: */
    QStringList keyCodeStringList;
    QList<int> keyCodeList = m_shownKeys.keys();
    for (int i = 0; i < keyCodeList.size(); ++i)
        keyCodeStringList << QString::number(keyCodeList[i]);
    /* Return current combination or "0" for "None": */
    return keyCodeStringList.isEmpty() ? "0" : keyCodeStringList.join(",");
}

void UIHostComboEditorPrivate::sltDeselect()
{
    deselect();
}

void UIHostComboEditorPrivate::sltClear()
{
    /* Cleanup combo: */
    m_shownKeys.clear();
    /* Update text: */
    updateText();
    /* Move the focus to text-field: */
    setFocus();
    /* Notify data changed: */
    emit sigDataChanged();
}

#ifdef Q_WS_WIN
bool UIHostComboEditorPrivate::winEvent(MSG *pMsg, long* /* pResult */)
{
    switch (pMsg->message)
    {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYUP:
        {
            /* Get key-code: */
            int iKeyCode = UINativeHotKey::distinguishModifierVKey((int)pMsg->wParam, (int)pMsg->lParam);
            unsigned iDownScanCode = (pMsg->lParam >> 16) & 0x7F;
            bool fPressed = !(pMsg->lParam & 0x80000000);
            bool fExtended = pMsg->lParam & 0x1000000;

            /* If present - why not just assert this? */
            if (m_pAltGrMonitor)
            {
                /* Update AltGR monitor state from key-event: */
                m_pAltGrMonitor->updateStateFromKeyEvent(iDownScanCode, fPressed, fExtended);
                /* And release left Ctrl key early (if required): */
                if (m_pAltGrMonitor->isLeftControlReleaseNeeded())
                {
                    m_pressedKeys.remove(VK_LCONTROL);
                    m_shownKeys.remove(VK_LCONTROL);
                }
                /* Fake LCtrl release events can also end up in the released
                 * key set.  Detect them on the immediately following RAlt up. */
                if (!m_pressedKeys.contains(VK_LCONTROL))
                    m_releasedKeys.remove(VK_LCONTROL);
            }

            /* Process the key event: */
            return processKeyEvent(iKeyCode, pMsg->message == WM_KEYDOWN || pMsg->message == WM_SYSKEYDOWN);
        }
        default:
            break;
    }

    return false;
}
#endif /* Q_WS_WIN */

#ifdef Q_WS_X11
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
bool UIHostComboEditorPrivate::x11Event(XEvent *pEvent)
{
    switch (pEvent->type)
    {
        case XKeyPress:
        case XKeyRelease:
        {
            /* Get key-code: */
            XKeyEvent *pKeyEvent = (XKeyEvent*)pEvent;
            KeySym ks = ::XKeycodeToKeysym(pKeyEvent->display, pKeyEvent->keycode, 0);

            int iKeySym = (int)ks;

            /* Process the key event: */
            return processKeyEvent(iKeySym, pEvent->type == XKeyPress);
        }
        default:
            break;
    }

    return false;
}
#pragma GCC diagnostic pop
#endif /* Q_WS_X11 */

#ifdef Q_WS_MAC
/* static */
bool UIHostComboEditorPrivate::darwinEventHandlerProc(const void *pvCocoaEvent, const void *pvCarbonEvent, void *pvUser)
{
    UIHostComboEditorPrivate *pEditor = static_cast<UIHostComboEditorPrivate*>(pvUser);
    EventRef inEvent = (EventRef)pvCarbonEvent;
    UInt32 EventClass = ::GetEventClass(inEvent);
    if (EventClass == kEventClassKeyboard)
        return pEditor->darwinKeyboardEvent(pvCocoaEvent, inEvent);
    return false;
}

bool UIHostComboEditorPrivate::darwinKeyboardEvent(const void *pvCocoaEvent, EventRef inEvent)
{
    /* Ignore key changes unless we're the focus widget: */
    if (!hasFocus())
        return false;

    UInt32 eventKind = ::GetEventKind(inEvent);
    switch (eventKind)
    {
        //case kEventRawKeyDown:
        //case kEventRawKeyUp:
        //case kEventRawKeyRepeat:
        case kEventRawKeyModifiersChanged:
        {
            /* Get modifier mask: */
            UInt32 modifierMask = 0;
            ::GetEventParameter(inEvent, kEventParamKeyModifiers, typeUInt32, NULL,
                                sizeof(modifierMask), NULL, &modifierMask);
            modifierMask = ::DarwinAdjustModifierMask(modifierMask, pvCocoaEvent);
            UInt32 changed = m_uDarwinKeyModifiers ^ modifierMask;

            if (!changed)
                break;

            /* Convert to keycode: */
            unsigned uKeyCode = ::DarwinModifierMaskToDarwinKeycode(changed);

            if (!uKeyCode || uKeyCode == ~0U)
                return false;

            /* Process the key event: */
            if (processKeyEvent(uKeyCode, changed & modifierMask))
            {
                /* Save the new modifier mask state. */
                m_uDarwinKeyModifiers = modifierMask;
                return true;
            }
            break;
        }
        default:
            break;
    }
    return false;
}
#endif /* Q_WS_MAC */

void UIHostComboEditorPrivate::keyPressEvent(QKeyEvent *pEvent)
{
    /* Ignore most of key presses... */
    switch (pEvent->key())
    {
        case Qt::Key_Enter:
        case Qt::Key_Return:
        case Qt::Key_Tab:
        case Qt::Key_Backtab:
        case Qt::Key_Escape:
            return QLineEdit::keyPressEvent(pEvent);
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_Left:
        case Qt::Key_Right:
            pEvent->ignore();
            return;
        default:
            break;
    }
}

void UIHostComboEditorPrivate::keyReleaseEvent(QKeyEvent *pEvent)
{
    /* Ignore most of key presses... */
    switch (pEvent->key())
    {
        case Qt::Key_Tab:
        case Qt::Key_Backtab:
        case Qt::Key_Escape:
            return QLineEdit::keyReleaseEvent(pEvent);
        case Qt::Key_Up:
        case Qt::Key_Down:
        case Qt::Key_Left:
        case Qt::Key_Right:
            pEvent->ignore();
            return;
        default:
            break;
    }
}

void UIHostComboEditorPrivate::mousePressEvent(QMouseEvent *pEvent)
{
    /* Handle like for usual QWidget: */
    QWidget::mousePressEvent(pEvent);
}

void UIHostComboEditorPrivate::mouseReleaseEvent(QMouseEvent *pEvent)
{
    /* Handle like for usual QWidget: */
    QWidget::mouseReleaseEvent(pEvent);
}

void UIHostComboEditorPrivate::sltReleasePendingKeys()
{
    /* Stop the timer, we process all pending keys at once: */
    m_pReleaseTimer->stop();
    /* Something to do? */
    if (!m_releasedKeys.isEmpty())
    {
        /* Remove every key: */
        QSetIterator<int> iterator(m_releasedKeys);
        while (iterator.hasNext())
        {
            int iKeyCode = iterator.next();
            m_pressedKeys.remove(iKeyCode);
            m_shownKeys.remove(iKeyCode);
        }
        m_releasedKeys.clear();
        if (m_pressedKeys.isEmpty())
            m_fStartNewSequence = true;
        /* Notify data changed: */
        emit sigDataChanged();
    }
    /* Make sure the user see what happens: */
    updateText();
}

bool UIHostComboEditorPrivate::processKeyEvent(int iKeyCode, bool fKeyPress)
{
    /* Check if symbol is valid else pass it to Qt: */
    if (!UINativeHotKey::isValidKey(iKeyCode))
        return false;

    /* Stop the release-pending-keys timer: */
    m_pReleaseTimer->stop();

    /* Key press: */
    if (fKeyPress)
    {
        /* Clear reflected symbols if new sequence started: */
        if (m_fStartNewSequence)
            m_shownKeys.clear();
        /* Make sure any keys pending for releasing are processed: */
        sltReleasePendingKeys();
        /* Check maximum combo size: */
        if (m_shownKeys.size() < UIHostCombo::m_iMaxComboSize)
        {
            /* Remember pressed symbol: */
            m_pressedKeys << iKeyCode;
            m_shownKeys.insert(iKeyCode, UINativeHotKey::toString(iKeyCode));
            /* Remember what we already started a sequence: */
            m_fStartNewSequence = false;
            /* Notify data changed: */
            emit sigDataChanged();
        }
    }
    /* Key release: */
    else
    {
        /* Queue released symbol for processing: */
        m_releasedKeys << iKeyCode;

        /* If all pressed keys are now pending for releasing we should stop further handling.
         * Now we have the status the user want: */
        if (m_pressedKeys == m_releasedKeys)
        {
            m_pressedKeys.clear();
            m_releasedKeys.clear();
            m_fStartNewSequence = true;
        }
        else
            m_pReleaseTimer->start();
    }

    /* Update text: */
    updateText();

    /* Prevent passing to Qt: */
    return true;
}

void UIHostComboEditorPrivate::updateText()
{
    QStringList shownKeyNames(m_shownKeys.values());
    setText(shownKeyNames.isEmpty() ? UIHostComboEditor::tr("None") : shownKeyNames.join(" + "));
}

