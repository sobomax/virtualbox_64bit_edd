/* $Id: UIDnDDropSource_win.cpp $ */
/** @file
 * VBox Qt GUI - UIDnDDropSource class implementation for Windows. This implements
 * the IDropSource interface.
 */

/*
 * Copyright (C) 2014 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#undef LOG_GROUP
#define LOG_GROUP LOG_GROUP_GUEST_DND
#include <VBox/log.h>

#include <iprt/assert.h>
#include <iprt/thread.h>

#include <windows.h>
#include <new> /* For bad_alloc. */

#include "UIDnDDropSource_win.h"



UIDnDDropSource::UIDnDDropSource(QWidget *pParent)
    : mRefCount(1),
      mpParent(pParent),
      mdwCurEffect(0),
      muCurAction(Qt::IgnoreAction)
{
    LogFlowFunc(("pParent=0x%p\n", mpParent));
}

UIDnDDropSource::~UIDnDDropSource(void)
{
    LogFlowFunc(("mRefCount=%RI32\n", mRefCount));
}

/*
 * IUnknown methods.
 */

STDMETHODIMP_(ULONG) UIDnDDropSource::AddRef(void)
{
    return InterlockedIncrement(&mRefCount);
}

STDMETHODIMP_(ULONG) UIDnDDropSource::Release(void)
{
    LONG lCount = InterlockedDecrement(&mRefCount);
    if (lCount == 0)
    {
        delete this;
        return 0;
    }

    return lCount;
}

STDMETHODIMP UIDnDDropSource::QueryInterface(REFIID iid, void **ppvObject)
{
    AssertPtrReturn(ppvObject, E_INVALIDARG);

    if (   iid == IID_IDropSource
        || iid == IID_IUnknown)
    {
        AddRef();
        *ppvObject = this;
        return S_OK;
    }

    *ppvObject = 0;
    return E_NOINTERFACE;
}

/*
 * IDropSource methods.
 */

/**
 * The system informs us about whether we should continue the drag'n drop
 * operation or not, depending on the sent key states.
 *
 * @return  HRESULT
 */
STDMETHODIMP UIDnDDropSource::QueryContinueDrag(BOOL fEscapePressed, DWORD dwKeyState)
{
#ifndef DEBUG_andy
    LogFlowFunc(("fEscapePressed=%RTbool, dwKeyState=0x%x, mdwCurEffect=%RI32, muCurAction=%RU32\n",
                 fEscapePressed, dwKeyState, mdwCurEffect, muCurAction));
#endif

    /* ESC pressed? Bail out. */
    if (fEscapePressed)
    {
        mdwCurEffect = 0;
        muCurAction = Qt::IgnoreAction;

        LogRel2(("DnD: User cancelled dropping data to the host\n"));
        return DRAGDROP_S_CANCEL;
    }

    bool fDropContent = false;

    /* Left mouse button released? Start "drop" action. */
    if ((dwKeyState & MK_LBUTTON) == 0)
        fDropContent = true;
    /** @todo Make this configurable? */

    if (fDropContent)
    {
        LogRel2(("DnD: User dropped data to the host\n"));
        return DRAGDROP_S_DROP;
    }

    /* No change, just continue. */
    return S_OK;
}

/**
 * The drop target gives our source feedback about whether
 * it can handle our data or not.
 *
 * @return  HRESULT
 */
STDMETHODIMP UIDnDDropSource::GiveFeedback(DWORD dwEffect)
{
     Qt::DropActions dropActions = Qt::IgnoreAction;

#ifndef DEBUG_andy
    LogFlowFunc(("dwEffect=0x%x\n", dwEffect));
#endif
    if (dwEffect)
    {
        if (dwEffect & DROPEFFECT_COPY)
            dropActions |= Qt::CopyAction;
        if (dwEffect & DROPEFFECT_MOVE)
            dropActions |= Qt::MoveAction;
        if (dwEffect & DROPEFFECT_LINK)
            dropActions |= Qt::LinkAction;
    }

    mdwCurEffect = dwEffect;
    muCurAction = dropActions;

    return DRAGDROP_S_USEDEFAULTCURSORS;
}

