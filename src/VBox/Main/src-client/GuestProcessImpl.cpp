/* $Id: GuestProcessImpl.cpp $ */
/** @file
 * VirtualBox Main - Guest process handling.
 */

/*
 * Copyright (C) 2012-2013 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

/**
 * Locking rules:
 * - When the main dispatcher (callbackDispatcher) is called it takes the
 *   WriteLock while dispatching to the various on* methods.
 * - All other outer functions (accessible by Main) must not own a lock
 *   while waiting for a callback or for an event.
 * - Only keep Read/WriteLocks as short as possible and only when necessary.
 */

/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#include "GuestProcessImpl.h"
#include "GuestSessionImpl.h"
#include "GuestCtrlImplPrivate.h"
#include "ConsoleImpl.h"
#include "VirtualBoxErrorInfoImpl.h"

#include "Global.h"
#include "AutoCaller.h"
#include "VBoxEvents.h"

#include <memory> /* For auto_ptr. */

#include <iprt/asm.h>
#include <iprt/cpp/utils.h> /* For unconst(). */
#include <iprt/getopt.h>

#include <VBox/com/listeners.h>

#include <VBox/com/array.h>

#ifdef LOG_GROUP
 #undef LOG_GROUP
#endif
#define LOG_GROUP LOG_GROUP_GUEST_CONTROL
#include <VBox/log.h>


class GuestProcessTask
{
public:

    GuestProcessTask(GuestProcess *pProcess)
        : mProcess(pProcess),
          mRC(VINF_SUCCESS) { }

    virtual ~GuestProcessTask(void) { }

    int rc(void) const { return mRC; }
    bool isOk(void) const { return RT_SUCCESS(mRC); }
    const ComObjPtr<GuestProcess> &Process(void) const { return mProcess; }

protected:

    const ComObjPtr<GuestProcess>    mProcess;
    int                              mRC;
};

class GuestProcessStartTask : public GuestProcessTask
{
public:

    GuestProcessStartTask(GuestProcess *pProcess)
        : GuestProcessTask(pProcess) { }
};

/**
 * Internal listener class to serve events in an
 * active manner, e.g. without polling delays.
 */
class GuestProcessListener
{
public:

    GuestProcessListener(void)
    {
    }

    HRESULT init(GuestProcess *pProcess)
    {
        AssertPtrReturn(pProcess, E_POINTER);
        mProcess = pProcess;
        return S_OK;
    }

    void uninit(void)
    {
        mProcess = NULL;
    }

    STDMETHOD(HandleEvent)(VBoxEventType_T aType, IEvent *aEvent)
    {
        switch (aType)
        {
            case VBoxEventType_OnGuestProcessStateChanged:
            case VBoxEventType_OnGuestProcessInputNotify:
            case VBoxEventType_OnGuestProcessOutput:
            {
                AssertPtrReturn(mProcess, E_POINTER);
                int rc2 = mProcess->signalWaitEvent(aType, aEvent);
#ifdef DEBUG
                LogFlowThisFunc(("Signalling events of type=%RU32, pProcess=%p resulted in rc=%Rrc\n",
                                 aType, &mProcess, rc2));
#endif
                break;
            }

            default:
                AssertMsgFailed(("Unhandled event %RU32\n", aType));
                break;
        }

        return S_OK;
    }

private:

    GuestProcess *mProcess;
};
typedef ListenerImpl<GuestProcessListener, GuestProcess*> GuestProcessListenerImpl;

VBOX_LISTENER_DECLARE(GuestProcessListenerImpl)

// constructor / destructor
/////////////////////////////////////////////////////////////////////////////

DEFINE_EMPTY_CTOR_DTOR(GuestProcess)

HRESULT GuestProcess::FinalConstruct(void)
{
    LogFlowThisFuncEnter();
    return BaseFinalConstruct();
}

void GuestProcess::FinalRelease(void)
{
    LogFlowThisFuncEnter();
    uninit();
    BaseFinalRelease();
    LogFlowThisFuncLeave();
}

// public initializer/uninitializer for internal purposes only
/////////////////////////////////////////////////////////////////////////////

int GuestProcess::init(Console *aConsole, GuestSession *aSession,
                       ULONG aProcessID, const GuestProcessStartupInfo &aProcInfo)
{
    LogFlowThisFunc(("aConsole=%p, aSession=%p, aProcessID=%RU32\n",
                     aConsole, aSession, aProcessID));

    AssertPtrReturn(aConsole, VERR_INVALID_POINTER);
    AssertPtrReturn(aSession, VERR_INVALID_POINTER);

    /* Enclose the state transition NotReady->InInit->Ready. */
    AutoInitSpan autoInitSpan(this);
    AssertReturn(autoInitSpan.isOk(), VERR_OBJECT_DESTROYED);

#ifndef VBOX_WITH_GUEST_CONTROL
    autoInitSpan.setSucceeded();
    return VINF_SUCCESS;
#else
    HRESULT hr;

    int vrc = bindToSession(aConsole, aSession, aProcessID /* Object ID */);
    if (RT_SUCCESS(vrc))
    {
        hr = unconst(mEventSource).createObject();
        if (FAILED(hr))
            vrc = VERR_NO_MEMORY;
        else
        {
            hr = mEventSource->init();
            if (FAILED(hr))
                vrc = VERR_COM_UNEXPECTED;
        }
    }

    if (RT_SUCCESS(vrc))
    {
        try
        {
            GuestProcessListener *pListener = new GuestProcessListener();
            ComObjPtr<GuestProcessListenerImpl> thisListener;
            hr = thisListener.createObject();
            if (SUCCEEDED(hr))
                hr = thisListener->init(pListener, this);

            if (SUCCEEDED(hr))
            {
                com::SafeArray <VBoxEventType_T> eventTypes;
                eventTypes.push_back(VBoxEventType_OnGuestProcessStateChanged);
                eventTypes.push_back(VBoxEventType_OnGuestProcessInputNotify);
                eventTypes.push_back(VBoxEventType_OnGuestProcessOutput);
                hr = mEventSource->RegisterListener(thisListener,
                                                    ComSafeArrayAsInParam(eventTypes),
                                                    TRUE /* Active listener */);
                if (SUCCEEDED(hr))
                {
                    vrc = baseInit();
                    if (RT_SUCCESS(vrc))
                    {
                        mLocalListener = thisListener;
                    }
                }
                else
                    vrc = VERR_COM_UNEXPECTED;
            }
            else
                vrc = VERR_COM_UNEXPECTED;
        }
        catch(std::bad_alloc &)
        {
            vrc = VERR_NO_MEMORY;
        }
    }

    if (RT_SUCCESS(vrc))
    {
        mData.mProcess = aProcInfo;
        mData.mExitCode = 0;
        mData.mPID = 0;
        mData.mLastError = VINF_SUCCESS;
        mData.mStatus = ProcessStatus_Undefined;
        /* Everything else will be set by the actual starting routine. */

        /* Confirm a successful initialization when it's the case. */
        autoInitSpan.setSucceeded();

        return vrc;
    }

    autoInitSpan.setFailed();
    return vrc;
#endif
}

/**
 * Uninitializes the instance.
 * Called from FinalRelease() or IGuestSession::uninit().
 */
void GuestProcess::uninit(void)
{
    /* Enclose the state transition Ready->InUninit->NotReady. */
    AutoUninitSpan autoUninitSpan(this);
    if (autoUninitSpan.uninitDone())
        return;

#ifdef VBOX_WITH_GUEST_CONTROL
    LogFlowThisFunc(("mCmd=%s, PID=%RU32\n",
                     mData.mProcess.mCommand.c_str(), mData.mPID));

    /* Terminate process if not already done yet. */
    int guestRc = VINF_SUCCESS;
    int vrc = terminateProcess(30 * 1000, &guestRc); /** @todo Make timeouts configurable. */
    /* Note: Don't return here yet; first uninit all other stuff in
     *       case of failure. */

    baseUninit();

    LogFlowThisFunc(("Returning rc=%Rrc, guestRc=%Rrc\n",
                     vrc, guestRc));
#endif
}

// implementation of public getters/setters for attributes
/////////////////////////////////////////////////////////////////////////////

STDMETHODIMP GuestProcess::COMGETTER(Arguments)(ComSafeArrayOut(BSTR, aArguments))
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    CheckComArgOutSafeArrayPointerValid(aArguments);

    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    com::SafeArray<BSTR> collection(mData.mProcess.mArguments.size());
    size_t s = 0;
    for (ProcessArguments::const_iterator it = mData.mProcess.mArguments.begin();
         it != mData.mProcess.mArguments.end();
         it++, s++)
    {
        Bstr tmp = *it;
        tmp.cloneTo(&collection[s]);
    }

    collection.detachTo(ComSafeArrayOutArg(aArguments));

    return S_OK;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

STDMETHODIMP GuestProcess::COMGETTER(Environment)(ComSafeArrayOut(BSTR, aEnvironment))
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    CheckComArgOutSafeArrayPointerValid(aEnvironment);

    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    com::SafeArray<BSTR> arguments(mData.mProcess.mEnvironment.Size());
    for (size_t i = 0; i < arguments.size(); i++)
    {
        Bstr tmp = mData.mProcess.mEnvironment.Get(i);
        tmp.cloneTo(&arguments[i]);
    }
    arguments.detachTo(ComSafeArrayOutArg(aEnvironment));

    return S_OK;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

STDMETHODIMP GuestProcess::COMGETTER(EventSource)(IEventSource ** aEventSource)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    CheckComArgOutPointerValid(aEventSource);

    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    // no need to lock - lifetime constant
    mEventSource.queryInterfaceTo(aEventSource);

    LogFlowThisFuncLeave();
    return S_OK;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

STDMETHODIMP GuestProcess::COMGETTER(ExecutablePath)(BSTR *aExecutablePath)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    CheckComArgOutPointerValid(aExecutablePath);

    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    mData.mProcess.mCommand.cloneTo(aExecutablePath);

    return S_OK;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

STDMETHODIMP GuestProcess::COMGETTER(ExitCode)(LONG *aExitCode)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    CheckComArgOutPointerValid(aExitCode);

    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    *aExitCode = mData.mExitCode;

    return S_OK;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

STDMETHODIMP GuestProcess::COMGETTER(Name)(BSTR *aName)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    CheckComArgOutPointerValid(aName);

    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    mData.mProcess.mName.cloneTo(aName);

    return S_OK;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

STDMETHODIMP GuestProcess::COMGETTER(PID)(ULONG *aPID)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    CheckComArgOutPointerValid(aPID);

    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    *aPID = mData.mPID;

    return S_OK;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

STDMETHODIMP GuestProcess::COMGETTER(Status)(ProcessStatus_T *aStatus)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    *aStatus = mData.mStatus;

    return S_OK;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

// private methods
/////////////////////////////////////////////////////////////////////////////

int GuestProcess::callbackDispatcher(PVBOXGUESTCTRLHOSTCBCTX pCbCtx, PVBOXGUESTCTRLHOSTCALLBACK pSvcCb)
{
    AssertPtrReturn(pCbCtx, VERR_INVALID_POINTER);
    AssertPtrReturn(pSvcCb, VERR_INVALID_POINTER);
#ifdef DEBUG
    LogFlowThisFunc(("uPID=%RU32, uContextID=%RU32, uFunction=%RU32, pSvcCb=%p\n",
                     mData.mPID, pCbCtx->uContextID, pCbCtx->uFunction, pSvcCb));
#endif

    int vrc;
    switch (pCbCtx->uFunction)
    {
        case GUEST_DISCONNECTED:
        {
            vrc = onGuestDisconnected(pCbCtx, pSvcCb);
            break;
        }

        case GUEST_EXEC_STATUS:
        {
            vrc = onProcessStatusChange(pCbCtx, pSvcCb);
            break;
        }

        case GUEST_EXEC_OUTPUT:
        {
            vrc = onProcessOutput(pCbCtx, pSvcCb);
            break;
        }

        case GUEST_EXEC_INPUT_STATUS:
        {
            vrc = onProcessInputStatus(pCbCtx, pSvcCb);
            break;
        }

        default:
            /* Silently ignore not implemented functions. */
            vrc = VERR_NOT_SUPPORTED;
            break;
    }

#ifdef DEBUG
    LogFlowFuncLeaveRC(vrc);
#endif
    return vrc;
}

/**
 * Checks if the current assigned PID matches another PID (from a callback).
 *
 * In protocol v1 we don't have the possibility to terminate/kill
 * processes so it can happen that a formerly started process A
 * (which has the context ID 0 (session=0, process=0, count=0) will
 * send a delayed message to the host if this process has already
 * been discarded there and the same context ID was reused by
 * a process B. Process B in turn then has a different guest PID.
 *
 * Note: This also can happen when restoring from a saved state which
 *       had a guest process running.
 *
 * @return  IPRT status code.
 * @param   uPID                    PID to check.
 */
inline int GuestProcess::checkPID(uint32_t uPID)
{
    int rc = VINF_SUCCESS;

    /* Was there a PID assigned yet? */
    if (mData.mPID)
    {
        if (RT_UNLIKELY(mData.mPID != uPID))
        {
            LogFlowFunc(("Stale guest process (PID=%RU32) sent data to a newly started process (pProcesS=%p, PID=%RU32, status=%RU32)\n",
                         uPID, this, mData.mPID, mData.mStatus));
            rc = VERR_NOT_FOUND;
        }
    }

    return rc;
}

/* static */
Utf8Str GuestProcess::guestErrorToString(int guestRc)
{
    Utf8Str strError;

    /** @todo pData->u32Flags: int vs. uint32 -- IPRT errors are *negative* !!! */
    switch (guestRc)
    {
        case VERR_FILE_NOT_FOUND: /* This is the most likely error. */
            strError += Utf8StrFmt(tr("The specified file was not found on guest"));
            break;

        case VERR_INVALID_VM_HANDLE:
            strError += Utf8StrFmt(tr("VMM device is not available (is the VM running?)"));
            break;

        case VERR_HGCM_SERVICE_NOT_FOUND:
            strError += Utf8StrFmt(tr("The guest execution service is not available"));
            break;

        case VERR_PATH_NOT_FOUND:
            strError += Utf8StrFmt(tr("Could not resolve path to specified file was not found on guest"));
            break;

        case VERR_BAD_EXE_FORMAT:
            strError += Utf8StrFmt(tr("The specified file is not an executable format on guest"));
            break;

        case VERR_AUTHENTICATION_FAILURE:
            strError += Utf8StrFmt(tr("The specified user was not able to logon on guest"));
            break;

        case VERR_INVALID_NAME:
            strError += Utf8StrFmt(tr("The specified file is an invalid name"));
            break;

        case VERR_TIMEOUT:
            strError += Utf8StrFmt(tr("The guest did not respond within time"));
            break;

        case VERR_CANCELLED:
            strError += Utf8StrFmt(tr("The execution operation was canceled"));
            break;

        case VERR_PERMISSION_DENIED:
            strError += Utf8StrFmt(tr("Invalid user/password credentials"));
            break;

        case VERR_MAX_PROCS_REACHED:
            strError += Utf8StrFmt(tr("Maximum number of concurrent guest processes has been reached"));
            break;

        case VERR_NOT_EQUAL: /** @todo Imprecise to the user; can mean anything and all. */
            strError += Utf8StrFmt(tr("Unable to retrieve requested information"));
            break;

        case VERR_NOT_FOUND:
            strError += Utf8StrFmt(tr("The guest execution service is not ready (yet)"));
            break;

        default:
            strError += Utf8StrFmt("%Rrc", guestRc);
            break;
    }

    return strError;
}

inline bool GuestProcess::isAlive(void)
{
    return (   mData.mStatus == ProcessStatus_Started
            || mData.mStatus == ProcessStatus_Paused
            || mData.mStatus == ProcessStatus_Terminating);
}

inline bool GuestProcess::hasEnded(void)
{
    return (   mData.mStatus == ProcessStatus_TerminatedNormally
            || mData.mStatus == ProcessStatus_TerminatedSignal
            || mData.mStatus == ProcessStatus_TerminatedAbnormally
            || mData.mStatus == ProcessStatus_TimedOutKilled
            || mData.mStatus == ProcessStatus_TimedOutAbnormally
            || mData.mStatus == ProcessStatus_Down
            || mData.mStatus == ProcessStatus_Error);
}

int GuestProcess::onGuestDisconnected(PVBOXGUESTCTRLHOSTCBCTX pCbCtx, PVBOXGUESTCTRLHOSTCALLBACK pSvcCbData)
{
    AssertPtrReturn(pCbCtx, VERR_INVALID_POINTER);
    AssertPtrReturn(pSvcCbData, VERR_INVALID_POINTER);

    int vrc = setProcessStatus(ProcessStatus_Down, VINF_SUCCESS);

    LogFlowFuncLeaveRC(vrc);
    return vrc;
}

int GuestProcess::onProcessInputStatus(PVBOXGUESTCTRLHOSTCBCTX pCbCtx, PVBOXGUESTCTRLHOSTCALLBACK pSvcCbData)
{
    AssertPtrReturn(pCbCtx, VERR_INVALID_POINTER);
    AssertPtrReturn(pSvcCbData, VERR_INVALID_POINTER);
    /* pCallback is optional. */

    if (pSvcCbData->mParms < 5)
        return VERR_INVALID_PARAMETER;

    CALLBACKDATA_PROC_INPUT dataCb;
    /* pSvcCb->mpaParms[0] always contains the context ID. */
    int vrc = pSvcCbData->mpaParms[1].getUInt32(&dataCb.uPID);
    AssertRCReturn(vrc, vrc);
    vrc = pSvcCbData->mpaParms[2].getUInt32(&dataCb.uStatus);
    AssertRCReturn(vrc, vrc);
    vrc = pSvcCbData->mpaParms[3].getUInt32(&dataCb.uFlags);
    AssertRCReturn(vrc, vrc);
    vrc = pSvcCbData->mpaParms[4].getUInt32(&dataCb.uProcessed);
    AssertRCReturn(vrc, vrc);

    LogFlowThisFunc(("uPID=%RU32, uStatus=%RU32, uFlags=%RI32, cbProcessed=%RU32\n",
                     dataCb.uPID, dataCb.uStatus, dataCb.uFlags, dataCb.uProcessed));

    vrc = checkPID(dataCb.uPID);
    if (RT_SUCCESS(vrc))
    {
        ProcessInputStatus_T inputStatus = ProcessInputStatus_Undefined;
        switch (dataCb.uStatus)
        {
            case INPUT_STS_WRITTEN:
                inputStatus = ProcessInputStatus_Written;
                break;
            case INPUT_STS_ERROR:
                inputStatus = ProcessInputStatus_Broken;
                break;
            case INPUT_STS_TERMINATED:
                inputStatus = ProcessInputStatus_Broken;
                break;
            case INPUT_STS_OVERFLOW:
                inputStatus = ProcessInputStatus_Overflow;
                break;
            case INPUT_STS_UNDEFINED:
                /* Fall through is intentional. */
            default:
                AssertMsg(!dataCb.uProcessed, ("Processed data is not 0 in undefined input state\n"));
                break;
        }

        if (inputStatus != ProcessInputStatus_Undefined)
        {
            AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

            /* Copy over necessary data before releasing lock again. */
            uint32_t uPID = mData.mPID;
            /** @todo Also handle mSession? */

            alock.release(); /* Release lock before firing off event. */

            fireGuestProcessInputNotifyEvent(mEventSource, mSession, this,
                                             uPID, 0 /* StdIn */, dataCb.uProcessed, inputStatus);
        }
    }

    LogFlowFuncLeaveRC(vrc);
    return vrc;
}

int GuestProcess::onProcessNotifyIO(PVBOXGUESTCTRLHOSTCBCTX pCbCtx, PVBOXGUESTCTRLHOSTCALLBACK pSvcCbData)
{
    AssertPtrReturn(pCbCtx, VERR_INVALID_POINTER);
    AssertPtrReturn(pSvcCbData, VERR_INVALID_POINTER);

    return VERR_NOT_IMPLEMENTED;
}

int GuestProcess::onProcessStatusChange(PVBOXGUESTCTRLHOSTCBCTX pCbCtx, PVBOXGUESTCTRLHOSTCALLBACK pSvcCbData)
{
    AssertPtrReturn(pCbCtx, VERR_INVALID_POINTER);
    AssertPtrReturn(pSvcCbData, VERR_INVALID_POINTER);

    if (pSvcCbData->mParms < 5)
        return VERR_INVALID_PARAMETER;

    CALLBACKDATA_PROC_STATUS dataCb;
    /* pSvcCb->mpaParms[0] always contains the context ID. */
    int vrc = pSvcCbData->mpaParms[1].getUInt32(&dataCb.uPID);
    AssertRCReturn(vrc, vrc);
    vrc = pSvcCbData->mpaParms[2].getUInt32(&dataCb.uStatus);
    AssertRCReturn(vrc, vrc);
    vrc = pSvcCbData->mpaParms[3].getUInt32(&dataCb.uFlags);
    AssertRCReturn(vrc, vrc);
    vrc = pSvcCbData->mpaParms[4].getPointer(&dataCb.pvData, &dataCb.cbData);
    AssertRCReturn(vrc, vrc);

    LogFlowThisFunc(("uPID=%RU32, uStatus=%RU32, uFlags=%RU32\n",
                     dataCb.uPID, dataCb.uStatus, dataCb.uFlags));

    vrc = checkPID(dataCb.uPID);
    if (RT_SUCCESS(vrc))
    {
        ProcessStatus_T procStatus = ProcessStatus_Undefined;
        int procRc = VINF_SUCCESS;

        switch (dataCb.uStatus)
        {
            case PROC_STS_STARTED:
            {
                procStatus = ProcessStatus_Started;

                AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
                mData.mPID = dataCb.uPID; /* Set the process PID. */
                break;
            }

            case PROC_STS_TEN:
            {
                procStatus = ProcessStatus_TerminatedNormally;

                AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
                mData.mExitCode = dataCb.uFlags; /* Contains the exit code. */
                break;
            }

            case PROC_STS_TES:
            {
                procStatus = ProcessStatus_TerminatedSignal;

                AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
                mData.mExitCode = dataCb.uFlags; /* Contains the signal. */
                break;
            }

            case PROC_STS_TEA:
            {
                procStatus = ProcessStatus_TerminatedAbnormally;
                break;
            }

            case PROC_STS_TOK:
            {
                procStatus = ProcessStatus_TimedOutKilled;
                break;
            }

            case PROC_STS_TOA:
            {
                procStatus = ProcessStatus_TimedOutAbnormally;
                break;
            }

            case PROC_STS_DWN:
            {
                procStatus = ProcessStatus_Down;
                break;
            }

            case PROC_STS_ERROR:
            {
                procRc = dataCb.uFlags; /* mFlags contains the IPRT error sent from the guest. */
                procStatus = ProcessStatus_Error;
                break;
            }

            case PROC_STS_UNDEFINED:
            default:
            {
                /* Silently skip this request. */
                procStatus = ProcessStatus_Undefined;
                break;
            }
        }

        LogFlowThisFunc(("Got rc=%Rrc, procSts=%RU32, procRc=%Rrc\n",
                         vrc, procStatus, procRc));

        /* Set the process status. */
        int rc2 = setProcessStatus(procStatus, procRc);
        if (RT_SUCCESS(vrc))
            vrc = rc2;
    }

    LogFlowFuncLeaveRC(vrc);
    return vrc;
}

int GuestProcess::onProcessOutput(PVBOXGUESTCTRLHOSTCBCTX pCbCtx, PVBOXGUESTCTRLHOSTCALLBACK pSvcCbData)
{
    AssertPtrReturn(pSvcCbData, VERR_INVALID_POINTER);

    if (pSvcCbData->mParms < 5)
        return VERR_INVALID_PARAMETER;

    CALLBACKDATA_PROC_OUTPUT dataCb;
    /* pSvcCb->mpaParms[0] always contains the context ID. */
    int vrc = pSvcCbData->mpaParms[1].getUInt32(&dataCb.uPID);
    AssertRCReturn(vrc, vrc);
    vrc = pSvcCbData->mpaParms[2].getUInt32(&dataCb.uHandle);
    AssertRCReturn(vrc, vrc);
    vrc = pSvcCbData->mpaParms[3].getUInt32(&dataCb.uFlags);
    AssertRCReturn(vrc, vrc);
    vrc = pSvcCbData->mpaParms[4].getPointer(&dataCb.pvData, &dataCb.cbData);
    AssertRCReturn(vrc, vrc);

    LogFlowThisFunc(("uPID=%RU32, uHandle=%RU32, uFlags=%RI32, pvData=%p, cbData=%RU32\n",
                     dataCb.uPID, dataCb.uHandle, dataCb.uFlags, dataCb.pvData, dataCb.cbData));

    vrc = checkPID(dataCb.uPID);
    if (RT_SUCCESS(vrc))
    {
        com::SafeArray<BYTE> data((size_t)dataCb.cbData);
        if (dataCb.cbData)
            data.initFrom((BYTE*)dataCb.pvData, dataCb.cbData);

        fireGuestProcessOutputEvent(mEventSource, mSession, this,
                                    mData.mPID, dataCb.uHandle, dataCb.cbData, ComSafeArrayAsInParam(data));
    }

    LogFlowFuncLeaveRC(vrc);
    return vrc;
}

/**
 * Called by IGuestSession right before this process gets
 * removed from the public process list.
 */
int GuestProcess::onRemove(void)
{
    LogFlowThisFuncEnter();

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    int vrc = VINF_SUCCESS;

    /*
     * Note: The event source stuff holds references to this object,
     *       so make sure that this is cleaned up *before* calling uninit().
     */
    if (!mEventSource.isNull())
    {
        mEventSource->UnregisterListener(mLocalListener);

        mLocalListener.setNull();
        unconst(mEventSource).setNull();
    }

    LogFlowFuncLeaveRC(vrc);
    return vrc;
}

int GuestProcess::readData(uint32_t uHandle, uint32_t uSize, uint32_t uTimeoutMS,
                           void *pvData, size_t cbData, uint32_t *pcbRead, int *pGuestRc)
{
    LogFlowThisFunc(("uPID=%RU32, uHandle=%RU32, uSize=%RU32, uTimeoutMS=%RU32, pvData=%p, cbData=%RU32, pGuestRc=%p\n",
                     mData.mPID, uHandle, uSize, uTimeoutMS, pvData, cbData, pGuestRc));
    AssertReturn(uSize, VERR_INVALID_PARAMETER);
    AssertPtrReturn(pvData, VERR_INVALID_POINTER);
    AssertReturn(cbData >= uSize, VERR_INVALID_PARAMETER);
    /* pcbRead is optional. */

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    if (   mData.mStatus != ProcessStatus_Started
        /* Skip reading if the process wasn't started with the appropriate
         * flags. */
        || (   (   uHandle == OUTPUT_HANDLE_ID_STDOUT
                || uHandle == OUTPUT_HANDLE_ID_STDOUT_DEPRECATED)
            && !(mData.mProcess.mFlags & ProcessCreateFlag_WaitForStdOut))
        || (   uHandle == OUTPUT_HANDLE_ID_STDERR
            && !(mData.mProcess.mFlags & ProcessCreateFlag_WaitForStdErr))
       )
    {
        if (pcbRead)
            *pcbRead = 0;
        if (pGuestRc)
            *pGuestRc = VINF_SUCCESS;
        return VINF_SUCCESS; /* Nothing to read anymore. */
    }

    int vrc;

    GuestWaitEvent *pEvent = NULL;
    GuestEventTypes eventTypes;
    try
    {
        /*
         * On Guest Additions < 4.3 there is no guarantee that the process status
         * change arrives *after* the output event, e.g. if this was the last output
         * block being read and the process will report status "terminate".
         * So just skip checking for process status change and only wait for the
         * output event.
         */
        if (mSession->getProtocolVersion() >= 2)
            eventTypes.push_back(VBoxEventType_OnGuestProcessStateChanged);
        eventTypes.push_back(VBoxEventType_OnGuestProcessOutput);

        vrc = registerWaitEvent(eventTypes, &pEvent);
    }
    catch (std::bad_alloc)
    {
        vrc = VERR_NO_MEMORY;
    }

    if (RT_FAILURE(vrc))
        return vrc;

    if (RT_SUCCESS(vrc))
    {
        VBOXHGCMSVCPARM paParms[8];
        int i = 0;
        paParms[i++].setUInt32(pEvent->ContextID());
        paParms[i++].setUInt32(mData.mPID);
        paParms[i++].setUInt32(uHandle);
        paParms[i++].setUInt32(0 /* Flags, none set yet. */);

        alock.release(); /* Drop the write lock before sending. */

        vrc = sendCommand(HOST_EXEC_GET_OUTPUT, i, paParms);
    }

    if (RT_SUCCESS(vrc))
        vrc = waitForOutput(pEvent, uHandle, uTimeoutMS,
                            pvData, cbData, pcbRead);

    unregisterWaitEvent(pEvent);

    LogFlowFuncLeaveRC(vrc);
    return vrc;
}

/* Does not do locking; caller is responsible for that! */
int GuestProcess::setProcessStatus(ProcessStatus_T procStatus, int procRc)
{
    LogFlowThisFuncEnter();

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    LogFlowThisFunc(("oldStatus=%RU32, newStatus=%RU32, procRc=%Rrc\n",
                     mData.mStatus, procStatus, procRc));

    if (procStatus == ProcessStatus_Error)
    {
        AssertMsg(RT_FAILURE(procRc), ("Guest rc must be an error (%Rrc)\n", procRc));
        /* Do not allow overwriting an already set error. If this happens
         * this means we forgot some error checking/locking somewhere. */
        AssertMsg(RT_SUCCESS(mData.mLastError), ("Guest rc already set (to %Rrc)\n", mData.mLastError));
    }
    else
        AssertMsg(RT_SUCCESS(procRc), ("Guest rc must not be an error (%Rrc)\n", procRc));

    int rc = VINF_SUCCESS;

    if (mData.mStatus != procStatus) /* Was there a process status change? */
    {
        mData.mStatus    = procStatus;
        mData.mLastError = procRc;

        ComObjPtr<VirtualBoxErrorInfo> errorInfo;
        HRESULT hr = errorInfo.createObject();
        ComAssertComRC(hr);
        if (RT_FAILURE(mData.mLastError))
        {
            hr = errorInfo->initEx(VBOX_E_IPRT_ERROR, mData.mLastError,
                                   COM_IIDOF(IGuestProcess), getComponentName(),
                                   guestErrorToString(mData.mLastError));
            ComAssertComRC(hr);
        }

        /* Copy over necessary data before releasing lock again. */
        uint32_t uPID =  mData.mPID;
        /** @todo Also handle mSession? */

        alock.release(); /* Release lock before firing off event. */

        fireGuestProcessStateChangedEvent(mEventSource, mSession, this,
                                          uPID, procStatus, errorInfo);
#if 0
        /*
         * On Guest Additions < 4.3 there is no guarantee that outstanding
         * requests will be delivered to the host after the process has ended,
         * so just cancel all waiting events here to not let clients run
         * into timeouts.
         */
        if (   mSession->getProtocolVersion() < 2
            && hasEnded())
        {
            LogFlowThisFunc(("Process ended, canceling outstanding wait events ...\n"));
            rc = cancelWaitEvents();
        }
#endif
    }

    return rc;
}

/* static */
HRESULT GuestProcess::setErrorExternal(VirtualBoxBase *pInterface, int guestRc)
{
    AssertPtr(pInterface);
    AssertMsg(RT_FAILURE(guestRc), ("Guest rc does not indicate a failure when setting error\n"));

    return pInterface->setError(VBOX_E_IPRT_ERROR, GuestProcess::guestErrorToString(guestRc).c_str());
}

int GuestProcess::startProcess(uint32_t uTimeoutMS, int *pGuestRc)
{
    LogFlowThisFunc(("uTimeoutMS=%RU32, procCmd=%s, procTimeoutMS=%RU32, procFlags=%x, sessionID=%RU32\n",
                     uTimeoutMS, mData.mProcess.mCommand.c_str(), mData.mProcess.mTimeoutMS, mData.mProcess.mFlags,
                     mSession->getId()));

    /* Wait until the caller function (if kicked off by a thread)
     * has returned and continue operation. */
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    mData.mStatus = ProcessStatus_Starting;

    int vrc;

    GuestWaitEvent *pEvent = NULL;
    GuestEventTypes eventTypes;
    try
    {
        eventTypes.push_back(VBoxEventType_OnGuestProcessStateChanged);

        vrc = registerWaitEvent(eventTypes, &pEvent);
    }
    catch (std::bad_alloc)
    {
        vrc = VERR_NO_MEMORY;
    }

    if (RT_FAILURE(vrc))
        return vrc;

    GuestSession *pSession = mSession;
    AssertPtr(pSession);

    const GuestCredentials &sessionCreds = pSession->getCredentials();

    /* Prepare arguments. */
    char *pszArgs = NULL;
    size_t cArgs = mData.mProcess.mArguments.size();
    if (cArgs >= UINT32_MAX)
        vrc = VERR_BUFFER_OVERFLOW;

    if (   RT_SUCCESS(vrc)
        && cArgs)
    {
        char **papszArgv = (char**)RTMemAlloc((cArgs + 1) * sizeof(char*));
        AssertReturn(papszArgv, VERR_NO_MEMORY);

        for (size_t i = 0; i < cArgs && RT_SUCCESS(vrc); i++)
        {
            const char *pszCurArg = mData.mProcess.mArguments[i].c_str();
            AssertPtr(pszCurArg);
            vrc = RTStrDupEx(&papszArgv[i], pszCurArg);
        }
        papszArgv[cArgs] = NULL;

        if (RT_SUCCESS(vrc))
            vrc = RTGetOptArgvToString(&pszArgs, papszArgv, RTGETOPTARGV_CNV_QUOTE_BOURNE_SH);

        if (papszArgv)
        {
            size_t i = 0;
            while (papszArgv[i])
                RTStrFree(papszArgv[i++]);
            RTMemFree(papszArgv);
        }
    }

    /* Calculate arguments size (in bytes). */
    size_t cbArgs = 0;
    if (RT_SUCCESS(vrc))
        cbArgs = pszArgs ? strlen(pszArgs) + 1 : 0; /* Include terminating zero. */

    /* Prepare environment. */
    void *pvEnv = NULL;
    size_t cbEnv = 0;
    if (RT_SUCCESS(vrc))
        vrc = mData.mProcess.mEnvironment.BuildEnvironmentBlock(&pvEnv, &cbEnv, NULL /* cEnv */);

    if (RT_SUCCESS(vrc))
    {
        AssertPtr(mSession);
        uint32_t uProtocol = mSession->getProtocolVersion();

        /* Prepare HGCM call. */
        VBOXHGCMSVCPARM paParms[16];
        int i = 0;
        paParms[i++].setUInt32(pEvent->ContextID());
        paParms[i++].setPointer((void*)mData.mProcess.mCommand.c_str(),
                                (ULONG)mData.mProcess.mCommand.length() + 1);
        paParms[i++].setUInt32(mData.mProcess.mFlags);
        paParms[i++].setUInt32((uint32_t)mData.mProcess.mArguments.size());
        paParms[i++].setPointer((void*)pszArgs, (uint32_t)cbArgs);
        paParms[i++].setUInt32((uint32_t)mData.mProcess.mEnvironment.Size());
        paParms[i++].setUInt32((uint32_t)cbEnv);
        paParms[i++].setPointer((void*)pvEnv, (uint32_t)cbEnv);
        if (uProtocol < 2)
        {
            /* In protocol v1 (VBox < 4.3) the credentials were part of the execution
             * call. In newer protocols these credentials are part of the opened guest
             * session, so not needed anymore here. */
            paParms[i++].setPointer((void*)sessionCreds.mUser.c_str(), (ULONG)sessionCreds.mUser.length() + 1);
            paParms[i++].setPointer((void*)sessionCreds.mPassword.c_str(), (ULONG)sessionCreds.mPassword.length() + 1);
        }
        /*
         * If the WaitForProcessStartOnly flag is set, we only want to define and wait for a timeout
         * until the process was started - the process itself then gets an infinite timeout for execution.
         * This is handy when we want to start a process inside a worker thread within a certain timeout
         * but let the started process perform lengthly operations then.
         */
        if (mData.mProcess.mFlags & ProcessCreateFlag_WaitForProcessStartOnly)
            paParms[i++].setUInt32(UINT32_MAX /* Infinite timeout */);
        else
            paParms[i++].setUInt32(mData.mProcess.mTimeoutMS);
        if (uProtocol >= 2)
        {
            paParms[i++].setUInt32(mData.mProcess.mPriority);
            /* CPU affinity: We only support one CPU affinity block at the moment,
             * so that makes up to 64 CPUs total. This can be more in the future. */
            paParms[i++].setUInt32(1);
            /* The actual CPU affinity blocks. */
            paParms[i++].setPointer((void*)&mData.mProcess.mAffinity, sizeof(mData.mProcess.mAffinity));
        }

        alock.release(); /* Drop the write lock before sending. */

        vrc = sendCommand(HOST_EXEC_CMD, i, paParms);
        if (RT_FAILURE(vrc))
        {
            int rc2 = setProcessStatus(ProcessStatus_Error, vrc);
            AssertRC(rc2);
        }
    }

    GuestEnvironment::FreeEnvironmentBlock(pvEnv);
    if (pszArgs)
        RTStrFree(pszArgs);

    if (RT_SUCCESS(vrc))
        vrc = waitForStatusChange(pEvent, uTimeoutMS,
                                  NULL /* Process status */, pGuestRc);
    unregisterWaitEvent(pEvent);

    LogFlowFuncLeaveRC(vrc);
    return vrc;
}

int GuestProcess::startProcessAsync(void)
{
    LogFlowThisFuncEnter();

    int vrc;

    try
    {
        /* Asynchronously start the process on the guest by kicking off a
         * worker thread. */
        std::auto_ptr<GuestProcessStartTask> pTask(new GuestProcessStartTask(this));
        AssertReturn(pTask->isOk(), pTask->rc());

        vrc = RTThreadCreate(NULL, GuestProcess::startProcessThread,
                             (void *)pTask.get(), 0,
                             RTTHREADTYPE_MAIN_WORKER, 0,
                             "gctlPrcStart");
        if (RT_SUCCESS(vrc))
        {
            /* pTask is now owned by startProcessThread(), so release it. */
            pTask.release();
        }
    }
    catch(std::bad_alloc &)
    {
        vrc = VERR_NO_MEMORY;
    }

    LogFlowFuncLeaveRC(vrc);
    return vrc;
}

/* static */
DECLCALLBACK(int) GuestProcess::startProcessThread(RTTHREAD Thread, void *pvUser)
{
    LogFlowFunc(("pvUser=%p\n", pvUser));

    std::auto_ptr<GuestProcessStartTask> pTask(static_cast<GuestProcessStartTask*>(pvUser));
    AssertPtr(pTask.get());

    const ComObjPtr<GuestProcess> pProcess(pTask->Process());
    Assert(!pProcess.isNull());

    AutoCaller autoCaller(pProcess);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    int vrc = pProcess->startProcess(30 * 1000 /* 30s timeout */,
                                     NULL /* Guest rc, ignored */);
    /* Nothing to do here anymore. */

    LogFlowFunc(("pProcess=%p returning rc=%Rrc\n", (GuestProcess *)pProcess, vrc));
    return vrc;
}

int GuestProcess::terminateProcess(uint32_t uTimeoutMS, int *pGuestRc)
{
    /* pGuestRc is optional. */
    LogFlowThisFunc(("uTimeoutMS=%RU32\n", uTimeoutMS));

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    int vrc = VINF_SUCCESS;

    if (mData.mStatus != ProcessStatus_Started)
    {
        LogFlowThisFunc(("Process not in started state (state is %RU32), skipping termination\n",
                         mData.mStatus));
    }
    else
    {
        AssertPtr(mSession);
        /* Note: VBox < 4.3 (aka protocol version 1) does not
         *       support this, so just skip. */
        if (mSession->getProtocolVersion() < 2)
            vrc = VERR_NOT_SUPPORTED;

        if (RT_SUCCESS(vrc))
        {
            GuestWaitEvent *pEvent = NULL;
            GuestEventTypes eventTypes;
            try
            {
                eventTypes.push_back(VBoxEventType_OnGuestProcessStateChanged);

                vrc = registerWaitEvent(eventTypes, &pEvent);
            }
            catch (std::bad_alloc)
            {
                vrc = VERR_NO_MEMORY;
            }

            if (RT_FAILURE(vrc))
                return vrc;

            VBOXHGCMSVCPARM paParms[4];
            int i = 0;
            paParms[i++].setUInt32(pEvent->ContextID());
            paParms[i++].setUInt32(mData.mPID);

            alock.release(); /* Drop the write lock before sending. */

            vrc = sendCommand(HOST_EXEC_TERMINATE, i, paParms);
            if (RT_SUCCESS(vrc))
                vrc = waitForStatusChange(pEvent, uTimeoutMS,
                                          NULL /* ProcessStatus */, pGuestRc);
            unregisterWaitEvent(pEvent);
        }
    }

    LogFlowFuncLeaveRC(vrc);
    return vrc;
}

/* static */
ProcessWaitResult_T GuestProcess::waitFlagsToResultEx(uint32_t fWaitFlags,
                                                      ProcessStatus_T oldStatus, ProcessStatus_T newStatus,
                                                      uint32_t uProcFlags, uint32_t uProtocol)
{
    ProcessWaitResult_T waitResult = ProcessWaitResult_None;

    switch (newStatus)
    {
        case ProcessStatus_TerminatedNormally:
        case ProcessStatus_TerminatedSignal:
        case ProcessStatus_TerminatedAbnormally:
        case ProcessStatus_Down:
            /* Nothing to wait for anymore. */
            waitResult = ProcessWaitResult_Terminate;
            break;

        case ProcessStatus_TimedOutKilled:
        case ProcessStatus_TimedOutAbnormally:
            /* Dito. */
            waitResult = ProcessWaitResult_Timeout;
            break;

        case ProcessStatus_Started:
            switch (oldStatus)
            {
                case ProcessStatus_Undefined:
                case ProcessStatus_Starting:
                    /* Also wait for process start. */
                    if (fWaitFlags & ProcessWaitForFlag_Start)
                        waitResult = ProcessWaitResult_Start;
                    else
                    {
                        /*
                             * If ProcessCreateFlag_WaitForProcessStartOnly was specified on process creation the
                             * caller is not interested in getting further process statuses -- so just don't notify
                             * anything here anymore and return.
                             */
                        if (uProcFlags & ProcessCreateFlag_WaitForProcessStartOnly)
                            waitResult = ProcessWaitResult_Start;
                    }
                    break;

                case ProcessStatus_Started:
                    /* Only wait for process start. */
                    if (fWaitFlags == ProcessWaitForFlag_Start)
                        waitResult = ProcessWaitResult_Start;
                    break;

                default:
                    AssertMsgFailed(("Unhandled old status %RU32 before new status 'started'\n",
                                     oldStatus));
                    waitResult = ProcessWaitResult_Start;
                    break;
            }
            break;

        case ProcessStatus_Error:
            /* Nothing to wait for anymore. */
            waitResult = ProcessWaitResult_Error;
            break;

        case ProcessStatus_Undefined:
        case ProcessStatus_Starting:
            /* No result available yet, leave wait
             * flags untouched. */
            break;
    }

    if (newStatus == ProcessStatus_Started)
    {
        /* Filter out waits which are *not* supported using
         * older guest control Guest Additions.
         *
         ** @todo ProcessWaitForFlag_Std* flags are not implemented yet.
         */
        if (uProtocol < 99) /* See @todo above. */
        {
            if (   waitResult == ProcessWaitResult_None
                /* We don't support waiting for stdin, out + err,
                 * just skip waiting then. */
                && (   (fWaitFlags & ProcessWaitForFlag_StdIn)
                    || (fWaitFlags & ProcessWaitForFlag_StdOut)
                    || (fWaitFlags & ProcessWaitForFlag_StdErr)
                   )
               )
            {
                /* Use _WaitFlagNotSupported because we don't know what to tell the caller. */
                waitResult = ProcessWaitResult_WaitFlagNotSupported;
            }
        }
    }

#ifdef DEBUG
    LogFlowFunc(("oldStatus=%RU32, newStatus=%RU32, fWaitFlags=0x%x, waitResult=%RU32\n",
                 oldStatus, newStatus, fWaitFlags, waitResult));
#endif
    return waitResult;
}

ProcessWaitResult_T GuestProcess::waitFlagsToResult(uint32_t fWaitFlags)
{
    AssertPtr(mSession);
    return GuestProcess::waitFlagsToResultEx(fWaitFlags,
                                             mData.mStatus /* curStatus */, mData.mStatus /* newStatus */,
                                             mData.mProcess.mFlags, mSession->getProtocolVersion());
}

int GuestProcess::waitFor(uint32_t fWaitFlags, ULONG uTimeoutMS,
                          ProcessWaitResult_T &waitResult, int *pGuestRc)
{
    AssertReturn(fWaitFlags, VERR_INVALID_PARAMETER);

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    LogFlowThisFunc(("fWaitFlags=0x%x, uTimeoutMS=%RU32, procStatus=%RU32, procRc=%Rrc, pGuestRc=%p\n",
                     fWaitFlags, uTimeoutMS, mData.mStatus, mData.mLastError, pGuestRc));

    /* Did some error occur before? Then skip waiting and return. */
    ProcessStatus_T curStatus = mData.mStatus;
    if (curStatus == ProcessStatus_Error)
    {
        waitResult = ProcessWaitResult_Error;
        AssertMsg(RT_FAILURE(mData.mLastError), ("No error rc (%Rrc) set when guest process indicated an error\n", mData.mLastError));
        if (pGuestRc)
            *pGuestRc = mData.mLastError; /* Return last set error. */
        LogFlowThisFunc(("Process is in error state (guestRc=%Rrc)\n", mData.mLastError));
        return VERR_GSTCTL_GUEST_ERROR;
    }

    waitResult = waitFlagsToResult(fWaitFlags);

    /* No waiting needed? Return immediately using the last set error. */
    if (waitResult != ProcessWaitResult_None)
    {
        if (pGuestRc)
            *pGuestRc = mData.mLastError; /* Return last set error (if any). */
        LogFlowThisFunc(("Nothing to wait for (guestRc=%Rrc)\n", mData.mLastError));
        return RT_SUCCESS(mData.mLastError) ? VINF_SUCCESS : VERR_GSTCTL_GUEST_ERROR;
    }

    /* Adjust timeout. Passing 0 means RT_INDEFINITE_WAIT. */
    if (!uTimeoutMS)
        uTimeoutMS = RT_INDEFINITE_WAIT;

    int vrc;

    GuestWaitEvent *pEvent = NULL;
    GuestEventTypes eventTypes;
    try
    {
        eventTypes.push_back(VBoxEventType_OnGuestProcessStateChanged);

        vrc = registerWaitEvent(eventTypes, &pEvent);
    }
    catch (std::bad_alloc)
    {
        vrc = VERR_NO_MEMORY;
    }

    if (RT_FAILURE(vrc))
        return vrc;

    alock.release(); /* Release lock before waiting. */

    /*
     * Do the actual waiting.
     */
    ProcessStatus_T newStatus = ProcessStatus_Undefined;
    uint64_t u64StartMS = RTTimeMilliTS();
    for (;;)
    {
        uint64_t u64ElapsedMS = RTTimeMilliTS() - u64StartMS;
        if (   uTimeoutMS   != RT_INDEFINITE_WAIT
            && u64ElapsedMS >= uTimeoutMS)
        {
            vrc = VERR_TIMEOUT;
            break;
        }

        vrc = waitForStatusChange(pEvent,
                                    uTimeoutMS == RT_INDEFINITE_WAIT
                                  ? RT_INDEFINITE_WAIT : uTimeoutMS - (uint32_t)u64ElapsedMS,
                                  &newStatus, pGuestRc);
        if (RT_SUCCESS(vrc))
        {
            alock.acquire();

            waitResult = waitFlagsToResultEx(fWaitFlags, curStatus, newStatus,
                                             mData.mProcess.mFlags, mSession->getProtocolVersion());
#ifdef DEBUG
            LogFlowThisFunc(("Got new status change: fWaitFlags=0x%x, newStatus=%RU32, waitResult=%RU32\n",
                             fWaitFlags, newStatus, waitResult));
#endif
            if (ProcessWaitResult_None != waitResult) /* We got a waiting result. */
                break;
        }
        else /* Waiting failed, bail out. */
            break;

        alock.release(); /* Don't hold lock in next waiting round. */
    }

    unregisterWaitEvent(pEvent);

    LogFlowThisFunc(("Returned waitResult=%RU32, newStatus=%RU32, rc=%Rrc\n",
                     waitResult, newStatus, vrc));
    return vrc;
}

int GuestProcess::waitForInputNotify(GuestWaitEvent *pEvent, uint32_t uHandle, uint32_t uTimeoutMS,
                                     ProcessInputStatus_T *pInputStatus, uint32_t *pcbProcessed)
{
    AssertPtrReturn(pEvent, VERR_INVALID_POINTER);

    VBoxEventType_T evtType;
    ComPtr<IEvent> pIEvent;
    int vrc = waitForEvent(pEvent, uTimeoutMS,
                           &evtType, pIEvent.asOutParam());
    if (RT_SUCCESS(vrc))
    {
        if (evtType == VBoxEventType_OnGuestProcessInputNotify)
        {
            ComPtr<IGuestProcessInputNotifyEvent> pProcessEvent = pIEvent;
            Assert(!pProcessEvent.isNull());

            if (pInputStatus)
            {
                HRESULT hr2 = pProcessEvent->COMGETTER(Status)(pInputStatus);
                ComAssertComRC(hr2);
            }
            if (pcbProcessed)
            {
                HRESULT hr2 = pProcessEvent->COMGETTER(Processed)((ULONG*)pcbProcessed);
                ComAssertComRC(hr2);
            }
        }
        else
            vrc = VWRN_GSTCTL_OBJECTSTATE_CHANGED;
    }

    LogFlowThisFunc(("Returning pEvent=%p, uHandle=%RU32, rc=%Rrc\n",
                     pEvent, uHandle, vrc));
    return vrc;
}

int GuestProcess::waitForOutput(GuestWaitEvent *pEvent, uint32_t uHandle, uint32_t uTimeoutMS,
                                void *pvData, size_t cbData, uint32_t *pcbRead)
{
    AssertPtrReturn(pEvent, VERR_INVALID_POINTER);
    /* pvData is optional. */
    /* cbData is optional. */
    /* pcbRead is optional. */

    LogFlowThisFunc(("cEventTypes=%zu, pEvent=%p, uHandle=%RU32, uTimeoutMS=%RU32, pvData=%p, cbData=%zu, pcbRead=%p\n",
                     pEvent->TypeCount(), pEvent, uHandle, uTimeoutMS, pvData, cbData, pcbRead));

    int vrc;

    VBoxEventType_T evtType;
    ComPtr<IEvent> pIEvent;
    do
    {
        vrc = waitForEvent(pEvent, uTimeoutMS,
                           &evtType, pIEvent.asOutParam());
        if (RT_SUCCESS(vrc))
        {
            if (evtType == VBoxEventType_OnGuestProcessOutput)
            {
                ComPtr<IGuestProcessOutputEvent> pProcessEvent = pIEvent;
                Assert(!pProcessEvent.isNull());

                ULONG uHandleEvent;
                HRESULT hr = pProcessEvent->COMGETTER(Handle)(&uHandleEvent);
                if (   SUCCEEDED(hr)
                    && uHandleEvent == uHandle)
                {
                    if (pvData)
                    {
                        com::SafeArray <BYTE> data;
                        hr = pProcessEvent->COMGETTER(Data)(ComSafeArrayAsOutParam(data));
                        ComAssertComRC(hr);
                        size_t cbRead = data.size();
                        if (cbRead)
                        {
                            if (cbRead <= cbData)
                            {
                                /* Copy data from event into our buffer. */
                                memcpy(pvData, data.raw(), data.size());
                            }
                            else
                                vrc = VERR_BUFFER_OVERFLOW;

                            LogFlowThisFunc(("Read %zu bytes (uHandle=%RU32), rc=%Rrc\n",
                                             cbRead, uHandleEvent, vrc));
                        }
                    }

                    if (   RT_SUCCESS(vrc)
                        && pcbRead)
                    {
                        ULONG cbRead;
                        hr = pProcessEvent->COMGETTER(Processed)(&cbRead);
                        ComAssertComRC(hr);
                        *pcbRead = (uint32_t)cbRead;
                    }

                    break;
                }
                else if (FAILED(hr))
                    vrc = VERR_COM_UNEXPECTED;
            }
            else
                vrc = VWRN_GSTCTL_OBJECTSTATE_CHANGED;
        }

    } while (vrc == VINF_SUCCESS);

    if (   vrc != VINF_SUCCESS
        && pcbRead)
    {
        *pcbRead = 0;
    }

    LogFlowFuncLeaveRC(vrc);
    return vrc;
}

int GuestProcess::waitForStatusChange(GuestWaitEvent *pEvent, uint32_t uTimeoutMS,
                                      ProcessStatus_T *pProcessStatus, int *pGuestRc)
{
    AssertPtrReturn(pEvent, VERR_INVALID_POINTER);
    /* pProcessStatus is optional. */
    /* pGuestRc is optional. */

    VBoxEventType_T evtType;
    ComPtr<IEvent> pIEvent;
    int vrc = waitForEvent(pEvent, uTimeoutMS,
                           &evtType, pIEvent.asOutParam());
    if (RT_SUCCESS(vrc))
    {
        Assert(evtType == VBoxEventType_OnGuestProcessStateChanged);
        ComPtr<IGuestProcessStateChangedEvent> pProcessEvent = pIEvent;
        Assert(!pProcessEvent.isNull());

        ProcessStatus_T procStatus;
        HRESULT hr = pProcessEvent->COMGETTER(Status)(&procStatus);
        ComAssertComRC(hr);
        if (pProcessStatus)
            *pProcessStatus = procStatus;

        ComPtr<IVirtualBoxErrorInfo> errorInfo;
        hr = pProcessEvent->COMGETTER(Error)(errorInfo.asOutParam());
        ComAssertComRC(hr);

        LONG lGuestRc;
        hr = errorInfo->COMGETTER(ResultDetail)(&lGuestRc);
        ComAssertComRC(hr);

        LogFlowThisFunc(("Got procStatus=%RU32, guestRc=%RI32 (%Rrc)\n",
                         procStatus, lGuestRc, lGuestRc));

        if (RT_FAILURE((int)lGuestRc))
            vrc = VERR_GSTCTL_GUEST_ERROR;

        if (pGuestRc)
            *pGuestRc = (int)lGuestRc;
    }

    LogFlowFuncLeaveRC(vrc);
    return vrc;
}

/* static */
bool GuestProcess::waitResultImpliesEx(ProcessWaitResult_T waitResult,
                                       ProcessStatus_T procStatus, uint32_t uProcFlags,
                                       uint32_t uProtocol)
{
    bool fImplies;

    switch (waitResult)
    {
        case ProcessWaitResult_Start:
            fImplies = procStatus == ProcessStatus_Started;
            break;

        case ProcessWaitResult_Terminate:
            fImplies = (   procStatus == ProcessStatus_TerminatedNormally
                        || procStatus == ProcessStatus_TerminatedSignal
                        || procStatus == ProcessStatus_TerminatedAbnormally
                        || procStatus == ProcessStatus_TimedOutKilled
                        || procStatus == ProcessStatus_TimedOutAbnormally
                        || procStatus == ProcessStatus_Down
                        || procStatus == ProcessStatus_Error);
            break;

        default:
            fImplies = false;
            break;
    }

    return fImplies;
}

int GuestProcess::writeData(uint32_t uHandle, uint32_t uFlags,
                            void *pvData, size_t cbData, uint32_t uTimeoutMS, uint32_t *puWritten, int *pGuestRc)
{
    LogFlowThisFunc(("uPID=%RU32, uHandle=%RU32, uFlags=%RU32, pvData=%p, cbData=%RU32, uTimeoutMS=%RU32, puWritten=%p, pGuestRc=%p\n",
                     mData.mPID, uHandle, uFlags, pvData, cbData, uTimeoutMS, puWritten, pGuestRc));
    /* All is optional. There can be 0 byte writes. */

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    if (mData.mStatus != ProcessStatus_Started)
    {
        if (puWritten)
            *puWritten = 0;
        if (pGuestRc)
            *pGuestRc = VINF_SUCCESS;
        return VINF_SUCCESS; /* Not available for writing (anymore). */
    }

    int vrc;

    GuestWaitEvent *pEvent = NULL;
    GuestEventTypes eventTypes;
    try
    {
        /*
         * On Guest Additions < 4.3 there is no guarantee that the process status
         * change arrives *after* the input event, e.g. if this was the last input
         * block being written and the process will report status "terminate".
         * So just skip checking for process status change and only wait for the
         * input event.
         */
        if (mSession->getProtocolVersion() >= 2)
            eventTypes.push_back(VBoxEventType_OnGuestProcessStateChanged);
        eventTypes.push_back(VBoxEventType_OnGuestProcessInputNotify);

        vrc = registerWaitEvent(eventTypes, &pEvent);
    }
    catch (std::bad_alloc)
    {
        vrc = VERR_NO_MEMORY;
    }

    if (RT_FAILURE(vrc))
        return vrc;

    VBOXHGCMSVCPARM paParms[5];
    int i = 0;
    paParms[i++].setUInt32(pEvent->ContextID());
    paParms[i++].setUInt32(mData.mPID);
    paParms[i++].setUInt32(uFlags);
    paParms[i++].setPointer(pvData, (uint32_t)cbData);
    paParms[i++].setUInt32((uint32_t)cbData);

    alock.release(); /* Drop the write lock before sending. */

    uint32_t cbProcessed = 0;
    vrc = sendCommand(HOST_EXEC_SET_INPUT, i, paParms);
    if (RT_SUCCESS(vrc))
    {
        ProcessInputStatus_T inputStatus;
        vrc = waitForInputNotify(pEvent, uHandle, uTimeoutMS,
                                 &inputStatus, &cbProcessed);
        if (RT_SUCCESS(vrc))
        {
            /** @todo Set guestRc. */

            if (puWritten)
                *puWritten = cbProcessed;
        }
        /** @todo Error handling. */
    }

    unregisterWaitEvent(pEvent);

    LogFlowThisFunc(("Returning cbProcessed=%RU32, rc=%Rrc\n",
                     cbProcessed, vrc));
    return vrc;
}

// implementation of public methods
/////////////////////////////////////////////////////////////////////////////

STDMETHODIMP GuestProcess::Read(ULONG aHandle, ULONG aToRead, ULONG aTimeoutMS, ComSafeArrayOut(BYTE, aData))
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    if (aToRead == 0)
        return setError(E_INVALIDARG, tr("The size to read is zero"));
    CheckComArgOutSafeArrayPointerValid(aData);

    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    com::SafeArray<BYTE> data((size_t)aToRead);
    Assert(data.size() >= aToRead);

    HRESULT hr = S_OK;

    uint32_t cbRead; int guestRc;
    int vrc = readData(aHandle, aToRead, aTimeoutMS, data.raw(), aToRead, &cbRead, &guestRc);
    if (RT_SUCCESS(vrc))
    {
        if (data.size() != cbRead)
            data.resize(cbRead);
        data.detachTo(ComSafeArrayOutArg(aData));
    }
    else
    {
        switch (vrc)
        {
            case VERR_GSTCTL_GUEST_ERROR:
                hr = GuestProcess::setErrorExternal(this, guestRc);
                break;

            default:
                hr = setError(VBOX_E_IPRT_ERROR,
                              tr("Reading from process \"%s\" (PID %RU32) failed: %Rrc"),
                              mData.mProcess.mCommand.c_str(), mData.mPID, vrc);
                break;
        }
    }

    LogFlowThisFunc(("rc=%Rrc, cbRead=%RU32\n", vrc, cbRead));

    LogFlowFuncLeaveRC(vrc);
    return hr;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

STDMETHODIMP GuestProcess::Terminate(void)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    HRESULT hr = S_OK;

    int guestRc;
    int vrc = terminateProcess(30 * 1000 /* Timeout in ms */,
                               &guestRc);
    if (RT_FAILURE(vrc))
    {
        switch (vrc)
        {
           case VERR_GSTCTL_GUEST_ERROR:
                hr = GuestProcess::setErrorExternal(this, guestRc);
                break;

            case VERR_NOT_SUPPORTED:
                hr = setError(VBOX_E_IPRT_ERROR,
                              tr("Terminating process \"%s\" (PID %RU32) not supported by installed Guest Additions"),
                              mData.mProcess.mCommand.c_str(), mData.mPID);
                break;

            default:
                hr = setError(VBOX_E_IPRT_ERROR,
                              tr("Terminating process \"%s\" (PID %RU32) failed: %Rrc"),
                              mData.mProcess.mCommand.c_str(), mData.mPID, vrc);
                break;
        }
    }

    /* Remove process from guest session list. Now only API clients
     * still can hold references to it. */
    AssertPtr(mSession);
    int rc2 = mSession->processRemoveFromList(this);
    if (RT_SUCCESS(vrc))
        vrc = rc2;

    LogFlowFuncLeaveRC(vrc);
    return hr;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

STDMETHODIMP GuestProcess::WaitFor(ULONG aWaitFlags, ULONG aTimeoutMS, ProcessWaitResult_T *aReason)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    CheckComArgOutPointerValid(aReason);

    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    /*
     * Note: Do not hold any locks here while waiting!
     */
    HRESULT hr = S_OK;

    int guestRc; ProcessWaitResult_T waitResult;
    int vrc = waitFor(aWaitFlags, aTimeoutMS, waitResult, &guestRc);
    if (RT_SUCCESS(vrc))
    {
        *aReason = waitResult;
    }
    else
    {
        switch (vrc)
        {
            case VERR_GSTCTL_GUEST_ERROR:
                hr = GuestProcess::setErrorExternal(this, guestRc);
                break;

            case VERR_TIMEOUT:
                *aReason = ProcessWaitResult_Timeout;
                break;

            default:
                hr = setError(VBOX_E_IPRT_ERROR,
                              tr("Waiting for process \"%s\" (PID %RU32) failed: %Rrc"),
                              mData.mProcess.mCommand.c_str(), mData.mPID, vrc);
                break;
        }
    }

    LogFlowFuncLeaveRC(vrc);
    return hr;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

STDMETHODIMP GuestProcess::WaitForArray(ComSafeArrayIn(ProcessWaitForFlag_T, aFlags), ULONG aTimeoutMS, ProcessWaitResult_T *aReason)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    CheckComArgOutPointerValid(aReason);

    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    /*
     * Note: Do not hold any locks here while waiting!
     */
    uint32_t fWaitFor = ProcessWaitForFlag_None;
    com::SafeArray<ProcessWaitForFlag_T> flags(ComSafeArrayInArg(aFlags));
    for (size_t i = 0; i < flags.size(); i++)
        fWaitFor |= flags[i];

    return WaitFor(fWaitFor, aTimeoutMS, aReason);
#endif /* VBOX_WITH_GUEST_CONTROL */
}

STDMETHODIMP GuestProcess::Write(ULONG aHandle, ULONG aFlags,
                                 ComSafeArrayIn(BYTE, aData), ULONG aTimeoutMS, ULONG *aWritten)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    CheckComArgSafeArrayNotNull(aData);
    CheckComArgOutPointerValid(aWritten);

    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    com::SafeArray<BYTE> data(ComSafeArrayInArg(aData));

    HRESULT hr = S_OK;

    uint32_t cbWritten; int guestRc;
    int vrc = writeData(aHandle, aFlags, data.raw(), data.size(), aTimeoutMS, &cbWritten, &guestRc);
    if (RT_FAILURE(vrc))
    {
        switch (vrc)
        {
            case VERR_GSTCTL_GUEST_ERROR:
                hr = GuestProcess::setErrorExternal(this, guestRc);
                break;

            default:
                hr = setError(VBOX_E_IPRT_ERROR,
                              tr("Writing to process \"%s\" (PID %RU32) failed: %Rrc"),
                              mData.mProcess.mCommand.c_str(), mData.mPID, vrc);
                break;
        }
    }

    LogFlowThisFunc(("rc=%Rrc, aWritten=%RU32\n", vrc, cbWritten));

    *aWritten = (ULONG)cbWritten;

    LogFlowFuncLeaveRC(vrc);
    return hr;
#endif /* VBOX_WITH_GUEST_CONTROL */
}

STDMETHODIMP GuestProcess::WriteArray(ULONG aHandle, ComSafeArrayIn(ProcessInputFlag_T, aFlags),
                                      ComSafeArrayIn(BYTE, aData), ULONG aTimeoutMS, ULONG *aWritten)
{
#ifndef VBOX_WITH_GUEST_CONTROL
    ReturnComNotImplemented();
#else
    LogFlowThisFuncEnter();

    CheckComArgSafeArrayNotNull(aData);
    CheckComArgOutPointerValid(aWritten);

    AutoCaller autoCaller(this);
    if (FAILED(autoCaller.rc())) return autoCaller.rc();

    /*
     * Note: Do not hold any locks here while writing!
     */
    ULONG fWrite = ProcessInputFlag_None;
    com::SafeArray<ProcessInputFlag_T> flags(ComSafeArrayInArg(aFlags));
    for (size_t i = 0; i < flags.size(); i++)
        fWrite |= flags[i];

    return Write(aHandle, fWrite, ComSafeArrayInArg(aData), aTimeoutMS, aWritten);
#endif /* VBOX_WITH_GUEST_CONTROL */
}

///////////////////////////////////////////////////////////////////////////////

GuestProcessTool::GuestProcessTool(void)
    : pSession(NULL),
      pProcess(NULL)
{
}

GuestProcessTool::~GuestProcessTool(void)
{
    Terminate(30 * 1000, NULL /* pGuestRc */);
}

int GuestProcessTool::Init(GuestSession *pGuestSession, const GuestProcessStartupInfo &startupInfo,
                           bool fAsync, int *pGuestRc)
{
    LogFlowThisFunc(("pGuestSession=%p, szCmd=%s, fAsync=%RTbool\n",
                     pGuestSession, startupInfo.mCommand.c_str(), fAsync));

    AssertPtrReturn(pGuestSession, VERR_INVALID_POINTER);

    pSession = pGuestSession;
    mStartupInfo = startupInfo;

    /* Make sure the process is hidden. */
    mStartupInfo.mFlags |= ProcessCreateFlag_Hidden;

    int vrc = pSession->processCreateExInteral(mStartupInfo, pProcess);
    if (RT_SUCCESS(vrc))
        vrc = fAsync
            ? pProcess->startProcessAsync()
            : pProcess->startProcess(30 * 1000 /* 30s timeout */, pGuestRc);

    if (   RT_SUCCESS(vrc)
        && !fAsync
        && (   pGuestRc
            && RT_FAILURE(*pGuestRc)
           )
       )
    {
        vrc = VERR_GSTCTL_GUEST_ERROR;
    }

    LogFlowFuncLeaveRC(vrc);
    return vrc;
}

int GuestProcessTool::GetCurrentBlock(uint32_t uHandle, GuestProcessStreamBlock &strmBlock)
{
    const GuestProcessStream *pStream = NULL;
    if (uHandle == OUTPUT_HANDLE_ID_STDOUT)
        pStream = &mStdOut;
    else if (uHandle == OUTPUT_HANDLE_ID_STDERR)
        pStream = &mStdErr;

    if (!pStream)
        return VERR_INVALID_PARAMETER;

    int vrc;
    do
    {
        /* Try parsing the data to see if the current block is complete. */
        vrc = mStdOut.ParseBlock(strmBlock);
        if (strmBlock.GetCount())
            break;
    } while (RT_SUCCESS(vrc));

    LogFlowThisFunc(("rc=%Rrc, %RU64 pairs\n",
                      vrc, strmBlock.GetCount()));
    return vrc;
}

bool GuestProcessTool::IsRunning(void)
{
    AssertReturn(!pProcess.isNull(), false);

    ProcessStatus_T procStatus = ProcessStatus_Undefined;
    HRESULT hr = pProcess->COMGETTER(Status(&procStatus));
    Assert(SUCCEEDED(hr));

    if (   procStatus == ProcessStatus_Started
        || procStatus == ProcessStatus_Paused
        || procStatus == ProcessStatus_Terminating)
    {
        return true;
    }

    return false;
}

/* static */
int GuestProcessTool::Run(      GuestSession            *pGuestSession,
                          const GuestProcessStartupInfo &startupInfo,
                                int                     *pGuestRc)
{
    return RunEx(pGuestSession, startupInfo,
                 NULL /* pStrmOutObjects */, 0 /* cStrmOutObjects */,
                 pGuestRc);
}

/* static */
int GuestProcessTool::RunEx(      GuestSession            *pGuestSession,
                            const GuestProcessStartupInfo &startupInfo,
                                  GuestCtrlStreamObjects  *pStrmOutObjects,
                                  uint32_t                 cStrmOutObjects,
                                  int                     *pGuestRc)
{
    GuestProcessTool procTool; int guestRc;
    int vrc = procTool.Init(pGuestSession, startupInfo, false /* Async */, &guestRc);
    if (RT_SUCCESS(vrc))
    {
        while (cStrmOutObjects--)
        {
            try
            {
                GuestProcessStreamBlock strmBlk;
                vrc = procTool.WaitEx(  pStrmOutObjects
                                      ? GUESTPROCESSTOOL_FLAG_STDOUT_BLOCK
                                      : GUESTPROCESSTOOL_FLAG_NONE, &strmBlk, &guestRc);
                if (pStrmOutObjects)
                    pStrmOutObjects->push_back(strmBlk);
            }
            catch (std::bad_alloc)
            {
                vrc = VERR_NO_MEMORY;
            }
        }
    }

    if (RT_SUCCESS(vrc))
    {
        /* Make sure the process runs until completion. */
        vrc = procTool.Wait(GUESTPROCESSTOOL_FLAG_NONE, &guestRc);
        if (RT_SUCCESS(vrc))
        {
            guestRc = procTool.TerminatedOk(NULL /* Exit code */);
            if (RT_FAILURE(guestRc))
                vrc = VERR_GSTCTL_GUEST_ERROR;
        }
    }

    if (pGuestRc)
        *pGuestRc = guestRc;

    LogFlowFunc(("Returned rc=%Rrc, guestRc=%Rrc\n", vrc, guestRc));
    return vrc;
}

int GuestProcessTool::TerminatedOk(LONG *pExitCode)
{
    Assert(!pProcess.isNull());
    /* pExitCode is optional. */

    int vrc;
    if (!IsRunning())
    {
        LONG exitCode;
        HRESULT hr = pProcess->COMGETTER(ExitCode(&exitCode));
        Assert(SUCCEEDED(hr));

        if (pExitCode)
            *pExitCode = exitCode;

        vrc = (exitCode != 0)
              /** @todo Special guest control rc needed! */
            ? VERR_NOT_EQUAL : VINF_SUCCESS;
    }
    else
        vrc = VERR_INVALID_STATE; /** @todo Special guest control rc needed! */

    LogFlowFuncLeaveRC(vrc);
    return vrc;
}

int GuestProcessTool::Wait(uint32_t fFlags, int *pGuestRc)
{
    return WaitEx(fFlags, NULL /* pStrmBlkOut */, pGuestRc);
}

int GuestProcessTool::WaitEx(uint32_t fFlags, GuestProcessStreamBlock *pStrmBlkOut, int *pGuestRc)
{
    LogFlowThisFunc(("fFlags=0x%x, pStreamBlock=%p, pGuestRc=%p\n",
                     fFlags, pStrmBlkOut, pGuestRc));

    /* Can we parse the next block without waiting? */
    int vrc;
    if (fFlags & GUESTPROCESSTOOL_FLAG_STDOUT_BLOCK)
    {
        AssertPtr(pStrmBlkOut);
        vrc = GetCurrentBlock(OUTPUT_HANDLE_ID_STDOUT, *pStrmBlkOut);
        if (RT_SUCCESS(vrc))
            return vrc;
        /* else do the the waiting below. */
    }

    /* Do the waiting. */
    uint32_t fWaitFlags = ProcessWaitForFlag_Terminate;
    if (mStartupInfo.mFlags & ProcessCreateFlag_WaitForStdOut)
        fWaitFlags |= ProcessWaitForFlag_StdOut;
    if (mStartupInfo.mFlags & ProcessCreateFlag_WaitForStdErr)
        fWaitFlags |= ProcessWaitForFlag_StdErr;

    /** @todo Decrease timeout while running. */
    uint64_t u64StartMS = RTTimeMilliTS();
    uint32_t uTimeoutMS = mStartupInfo.mTimeoutMS;

    int guestRc;
    bool fDone = false;

    BYTE byBuf[_64K];
    uint32_t cbRead;

    bool fHandleStdOut = false;
    bool fHandleStdErr = false;

    /**
     * Updates the elapsed time and checks if a
     * timeout happened, then breaking out of the loop.
     */
#define UPDATE_AND_CHECK_ELAPSED_TIME()          \
    u64ElapsedMS = RTTimeMilliTS() - u64StartMS; \
    if (   uTimeoutMS   != RT_INDEFINITE_WAIT    \
        && u64ElapsedMS >= uTimeoutMS)           \
    {                                            \
        vrc = VERR_TIMEOUT;                      \
        break;                                   \
    }

    /**
     * Returns the remaining time (in ms).
     */
#define GET_REMAINING_TIME                                     \
      uTimeoutMS == RT_INDEFINITE_WAIT                         \
    ? RT_INDEFINITE_WAIT : uTimeoutMS - (uint32_t)u64ElapsedMS \

    ProcessWaitResult_T waitRes;
    do
    {
        uint64_t u64ElapsedMS;
        UPDATE_AND_CHECK_ELAPSED_TIME();

        vrc = pProcess->waitFor(fWaitFlags, GET_REMAINING_TIME,
                                waitRes, &guestRc);
        if (RT_FAILURE(vrc))
            break;

        switch (waitRes)
        {
            case ProcessWaitResult_StdIn:
                vrc = VERR_NOT_IMPLEMENTED;
                break;

            case ProcessWaitResult_StdOut:
                fHandleStdOut = true;
                break;

            case ProcessWaitResult_StdErr:
                fHandleStdErr = true;
                break;

            case ProcessWaitResult_WaitFlagNotSupported:
                if (fWaitFlags & ProcessWaitForFlag_StdOut)
                    fHandleStdOut = true;
                if (fWaitFlags & ProcessWaitForFlag_StdErr)
                    fHandleStdErr = true;
                /* Since waiting for stdout / stderr is not supported by the guest,
                 * wait a bit to not hog the CPU too much when polling for data. */
                RTThreadSleep(1); /* Optional, don't check rc. */
                break;

            case ProcessWaitResult_Error:
                vrc = VERR_GSTCTL_GUEST_ERROR;
                break;

            case ProcessWaitResult_Terminate:
                fDone = true;
                break;

            case ProcessWaitResult_Timeout:
                vrc = VERR_TIMEOUT;
                break;

            case ProcessWaitResult_Start:
            case ProcessWaitResult_Status:
                /* Not used here, just skip. */
                break;

            default:
                AssertMsgFailed(("Unhandled process wait result %RU32\n", waitRes));
                break;
        }

        if (RT_FAILURE(vrc))
            break;

        if (fHandleStdOut)
        {
            UPDATE_AND_CHECK_ELAPSED_TIME();

            cbRead = 0;
            vrc = pProcess->readData(OUTPUT_HANDLE_ID_STDOUT, sizeof(byBuf),
                                     GET_REMAINING_TIME,
                                     byBuf, sizeof(byBuf),
                                     &cbRead, &guestRc);
            if (   RT_FAILURE(vrc)
                || vrc == VWRN_GSTCTL_OBJECTSTATE_CHANGED)
                break;

            if (cbRead)
            {
                LogFlowThisFunc(("Received %RU32 bytes from stdout\n", cbRead));
                vrc = mStdOut.AddData(byBuf, cbRead);

                if (   RT_SUCCESS(vrc)
                    && (fFlags & GUESTPROCESSTOOL_FLAG_STDOUT_BLOCK))
                {
                    AssertPtr(pStrmBlkOut);
                    vrc = GetCurrentBlock(OUTPUT_HANDLE_ID_STDOUT, *pStrmBlkOut);

                    /* When successful, break out of the loop because we're done
                     * with reading the first stream block. */
                    if (RT_SUCCESS(vrc))
                        fDone = true;
                }
            }

            fHandleStdOut = false;
        }

        if (fHandleStdErr)
        {
            UPDATE_AND_CHECK_ELAPSED_TIME();

            cbRead = 0;
            vrc = pProcess->readData(OUTPUT_HANDLE_ID_STDERR, sizeof(byBuf),
                                     GET_REMAINING_TIME,
                                     byBuf, sizeof(byBuf),
                                     &cbRead, &guestRc);
            if (   RT_FAILURE(vrc)
                || vrc == VWRN_GSTCTL_OBJECTSTATE_CHANGED)
                break;

            if (cbRead)
            {
                LogFlowThisFunc(("Received %RU32 bytes from stderr\n", cbRead));
                vrc = mStdErr.AddData(byBuf, cbRead);
            }

            fHandleStdErr = false;
        }

    } while (!fDone && RT_SUCCESS(vrc));

#undef UPDATE_AND_CHECK_ELAPSED_TIME
#undef GET_REMAINING_TIME

    if (RT_FAILURE(guestRc))
        vrc = VERR_GSTCTL_GUEST_ERROR;

    LogFlowThisFunc(("Loop ended with rc=%Rrc, guestRc=%Rrc, waitRes=%RU32\n",
                     vrc, guestRc, waitRes));
    if (pGuestRc)
        *pGuestRc = guestRc;

    LogFlowFuncLeaveRC(vrc);
    return vrc;
}

int GuestProcessTool::Terminate(uint32_t uTimeoutMS, int *pGuestRc)
{
    LogFlowThisFuncEnter();

    int rc = VINF_SUCCESS;
    if (!pProcess.isNull())
    {
        rc = pProcess->terminateProcess(uTimeoutMS, pGuestRc);
        pProcess.setNull();
    }
    else
        rc = VERR_NOT_FOUND;

    LogFlowFuncLeaveRC(rc);
    return rc;
}

