
/* $Id: GuestSessionImpl.h $ */
/** @file
 * VirtualBox Main - Guest session handling.
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

#ifndef ____H_GUESTSESSIONIMPL
#define ____H_GUESTSESSIONIMPL

#include "VirtualBoxBase.h"
#include "EventImpl.h"

#include "GuestCtrlImplPrivate.h"
#include "GuestProcessImpl.h"
#include "GuestDirectoryImpl.h"
#include "GuestFileImpl.h"
#include "GuestFsObjInfoImpl.h"

#include <iprt/isofs.h> /* For UpdateAdditions. */

class Guest;

/**
 * Abstract base class for a lenghtly per-session operation which
 * runs in a Main worker thread.
 */
class GuestSessionTask
{
public:

    GuestSessionTask(GuestSession *pSession);

    virtual ~GuestSessionTask(void);

public:

    virtual int Run(void) = 0;
    virtual int RunAsync(const Utf8Str &strDesc, ComObjPtr<Progress> &pProgress) = 0;

protected:

    int getGuestProperty(const ComObjPtr<Guest> &pGuest,
                         const Utf8Str &strPath, Utf8Str &strValue);
    int setProgress(ULONG uPercent);
    int setProgressSuccess(void);
    HRESULT setProgressErrorMsg(HRESULT hr, const Utf8Str &strMsg);

protected:

    Utf8Str                 mDesc;
    GuestSession           *mSession;
    /** Progress object for getting updated when running
     *  asynchronously. Optional. */
    ComObjPtr<Progress>     mProgress;
};

/**
 * Task for opening a guest session.
 */
class SessionTaskOpen : public GuestSessionTask
{
public:

    SessionTaskOpen(GuestSession *pSession,
                    uint32_t uFlags,
                    uint32_t uTimeoutMS);

    virtual ~SessionTaskOpen(void);

public:

    int Run(int *pGuestRc);
    int RunAsync(const Utf8Str &strDesc, ComObjPtr<Progress> &pProgress);
    static int taskThread(RTTHREAD Thread, void *pvUser);

protected:

    /** Session creation flags. */
    uint32_t mFlags;
    /** Session creation timeout (in ms). */
    uint32_t mTimeoutMS;
};

/**
 * Task for copying files from host to the guest.
 */
class SessionTaskCopyTo : public GuestSessionTask
{
public:

    SessionTaskCopyTo(GuestSession *pSession,
                      const Utf8Str &strSource, const Utf8Str &strDest, uint32_t uFlags);

    SessionTaskCopyTo(GuestSession *pSession,
                      PRTFILE pSourceFile, size_t cbSourceOffset, uint64_t cbSourceSize,
                      const Utf8Str &strDest, uint32_t uFlags);

    virtual ~SessionTaskCopyTo(void);

public:

    int Run(void);
    int RunAsync(const Utf8Str &strDesc, ComObjPtr<Progress> &pProgress);
    static int taskThread(RTTHREAD Thread, void *pvUser);

protected:

    Utf8Str  mSource;
    PRTFILE  mSourceFile;
    size_t   mSourceOffset;
    uint64_t mSourceSize;
    Utf8Str  mDest;
    uint32_t mCopyFileFlags;
};

/**
 * Task for copying files from guest to the host.
 */
class SessionTaskCopyFrom : public GuestSessionTask
{
public:

    SessionTaskCopyFrom(GuestSession *pSession,
                        const Utf8Str &strSource, const Utf8Str &strDest, uint32_t uFlags);

    virtual ~SessionTaskCopyFrom(void);

public:

    int Run(void);
    int RunAsync(const Utf8Str &strDesc, ComObjPtr<Progress> &pProgress);
    static int taskThread(RTTHREAD Thread, void *pvUser);

protected:

    Utf8Str  mSource;
    Utf8Str  mDest;
    uint32_t mFlags;
};

/**
 * Task for automatically updating the Guest Additions on the guest.
 */
class SessionTaskUpdateAdditions : public GuestSessionTask
{
public:

    SessionTaskUpdateAdditions(GuestSession *pSession,
                               const Utf8Str &strSource, const ProcessArguments &aArguments,
                               uint32_t uFlags);

    virtual ~SessionTaskUpdateAdditions(void);

public:

    int Run(void);
    int RunAsync(const Utf8Str &strDesc, ComObjPtr<Progress> &pProgress);
    static int taskThread(RTTHREAD Thread, void *pvUser);

protected:

    /**
     * Suported OS types for automatic updating.
     */
    enum eOSType
    {
        eOSType_Unknown = 0,
        eOSType_Windows = 1,
        eOSType_Linux   = 2,
        eOSType_Solaris = 3
    };

    /**
     * Structure representing a file to
     * get off the .ISO, copied to the guest.
     */
    struct InstallerFile
    {
        InstallerFile(const Utf8Str &aSource,
                      const Utf8Str &aDest,
                      uint32_t       aFlags = 0)
            : strSource(aSource),
              strDest(aDest),
              fFlags(aFlags) { }

        InstallerFile(const Utf8Str          &aSource,
                      const Utf8Str          &aDest,
                      uint32_t                aFlags,
                      GuestProcessStartupInfo startupInfo)
            : strSource(aSource),
              strDest(aDest),
              fFlags(aFlags),
              mProcInfo(startupInfo)
        {
            mProcInfo.mCommand = strDest;
            if (mProcInfo.mName.isEmpty())
                mProcInfo.mName = strDest;
        }

        /** Source file on .ISO. */
        Utf8Str                 strSource;
        /** Destination file on the guest. */
        Utf8Str                 strDest;
        /** File flags. */
        uint32_t                fFlags;
        /** Optional arguments if this file needs to be
         *  executed. */
        GuestProcessStartupInfo mProcInfo;
    };

    int addProcessArguments(ProcessArguments &aArgumentsDest,
                            const ProcessArguments &aArgumentsSource);
    int copyFileToGuest(GuestSession *pSession, PRTISOFSFILE pISO,
                        Utf8Str const &strFileSource, const Utf8Str &strFileDest,
                        bool fOptional, uint32_t *pcbSize);
    int runFileOnGuest(GuestSession *pSession, GuestProcessStartupInfo &procInfo);

    /** Files to handle. */
    std::vector<InstallerFile> mFiles;
    /** The (optionally) specified Guest Additions .ISO on the host
     *  which will be used for the updating process. */
    Utf8Str                    mSource;
    /** (Optional) installer command line arguments. */
    ProcessArguments           mArguments;
    /** Update flags. */
    uint32_t                   mFlags;
};

/**
 * Guest session implementation.
 */
class ATL_NO_VTABLE GuestSession :
    public VirtualBoxBase,
    public GuestBase,
    VBOX_SCRIPTABLE_IMPL(IGuestSession)
{
public:
    /** @name COM and internal init/term/mapping cruft.
     * @{ */
    VIRTUALBOXBASE_ADD_ERRORINFO_SUPPORT(GuestSession, IGuestSession)
    DECLARE_NOT_AGGREGATABLE(GuestSession)
    DECLARE_PROTECT_FINAL_CONSTRUCT()
    BEGIN_COM_MAP(GuestSession)
        VBOX_DEFAULT_INTERFACE_ENTRIES(IGuestSession)
    END_COM_MAP()
    DECLARE_EMPTY_CTOR_DTOR(GuestSession)

    int     init(Guest *pGuest, const GuestSessionStartupInfo &ssInfo, const GuestCredentials &guestCreds);
    void    uninit(void);
    HRESULT FinalConstruct(void);
    void    FinalRelease(void);
    /** @}  */

    /** @name IGuestSession properties.
     * @{ */
    STDMETHOD(COMGETTER(User))(BSTR *aName);
    STDMETHOD(COMGETTER(Domain))(BSTR *aDomain);
    STDMETHOD(COMGETTER(Name))(BSTR *aName);
    STDMETHOD(COMGETTER(Id))(ULONG *aId);
    STDMETHOD(COMGETTER(Status))(GuestSessionStatus_T *aStatus);
    STDMETHOD(COMGETTER(Timeout))(ULONG *aTimeout);
    STDMETHOD(COMSETTER(Timeout))(ULONG aTimeout);
    STDMETHOD(COMGETTER(ProtocolVersion))(ULONG *aVersion);
    STDMETHOD(COMGETTER(Environment))(ComSafeArrayOut(BSTR, aEnvironment));
    STDMETHOD(COMSETTER(Environment))(ComSafeArrayIn(IN_BSTR, aEnvironment));
    STDMETHOD(COMGETTER(Processes))(ComSafeArrayOut(IGuestProcess *, aProcesses));
    STDMETHOD(COMGETTER(Directories))(ComSafeArrayOut(IGuestDirectory *, aDirectories));
    STDMETHOD(COMGETTER(Files))(ComSafeArrayOut(IGuestFile *, aFiles));
    STDMETHOD(COMGETTER(EventSource))(IEventSource ** aEventSource);
    /** @}  */

    /** @name IGuestSession methods.
     * @{ */
    STDMETHOD(Close)(void);
    STDMETHOD(CopyFrom)(IN_BSTR aSource, IN_BSTR aDest, ComSafeArrayIn(CopyFileFlag_T, aFlags), IProgress **aProgress);
    STDMETHOD(CopyTo)(IN_BSTR aSource, IN_BSTR aDest, ComSafeArrayIn(CopyFileFlag_T, aFlags), IProgress **aProgress);
    STDMETHOD(DirectoryCreate)(IN_BSTR aPath, ULONG aMode, ComSafeArrayIn(DirectoryCreateFlag_T, aFlags));
    STDMETHOD(DirectoryCreateTemp)(IN_BSTR aTemplate, ULONG aMode, IN_BSTR aPath, BOOL aSecure, BSTR *aDirectory);
    STDMETHOD(DirectoryExists)(IN_BSTR aPath, BOOL *aExists);
    STDMETHOD(DirectoryOpen)(IN_BSTR aPath, IN_BSTR aFilter, ComSafeArrayIn(DirectoryOpenFlag_T, aFlags), IGuestDirectory **aDirectory);
    STDMETHOD(DirectoryQueryInfo)(IN_BSTR aPath, IGuestFsObjInfo **aInfo);
    STDMETHOD(DirectoryRemove)(IN_BSTR aPath);
    STDMETHOD(DirectoryRemoveRecursive)(IN_BSTR aPath, ComSafeArrayIn(DirectoryRemoveRecFlag_T, aFlags), IProgress **aProgress);
    STDMETHOD(DirectoryRename)(IN_BSTR aSource, IN_BSTR aDest, ComSafeArrayIn(PathRenameFlag_T, aFlags));
    STDMETHOD(DirectorySetACL)(IN_BSTR aPath, IN_BSTR aACL);
    STDMETHOD(EnvironmentClear)(void);
    STDMETHOD(EnvironmentGet)(IN_BSTR aName, BSTR *aValue);
    STDMETHOD(EnvironmentSet)(IN_BSTR aName, IN_BSTR aValue);
    STDMETHOD(EnvironmentUnset)(IN_BSTR aName);
    STDMETHOD(FileCreateTemp)(IN_BSTR aTemplate, ULONG aMode, IN_BSTR aPath, BOOL aSecure, IGuestFile **aFile);
    STDMETHOD(FileExists)(IN_BSTR aPath, BOOL *aExists);
    STDMETHOD(FileRemove)(IN_BSTR aPath);
    STDMETHOD(FileOpen)(IN_BSTR aPath, IN_BSTR aOpenMode, IN_BSTR aDisposition, ULONG aCreationMode, IGuestFile **aFile);
    STDMETHOD(FileOpenEx)(IN_BSTR aPath, IN_BSTR aOpenMode, IN_BSTR aDisposition, IN_BSTR aSharingMode, ULONG aCreationMode, LONG64 aOffset, IGuestFile **aFile);
    STDMETHOD(FileQueryInfo)(IN_BSTR aPath, IGuestFsObjInfo **aInfo);
    STDMETHOD(FileQuerySize)(IN_BSTR aPath, LONG64 *aSize);
    STDMETHOD(FileRename)(IN_BSTR aSource, IN_BSTR aDest, ComSafeArrayIn(PathRenameFlag_T, aFlags));
    STDMETHOD(FileSetACL)(IN_BSTR aPath, IN_BSTR aACL);
    STDMETHOD(ProcessCreate)(IN_BSTR aCommand, ComSafeArrayIn(IN_BSTR, aArguments), ComSafeArrayIn(IN_BSTR, aEnvironment),
                             ComSafeArrayIn(ProcessCreateFlag_T, aFlags), ULONG aTimeoutMS, IGuestProcess **aProcess);
    STDMETHOD(ProcessCreateEx)(IN_BSTR aCommand, ComSafeArrayIn(IN_BSTR, aArguments), ComSafeArrayIn(IN_BSTR, aEnvironment),
                               ComSafeArrayIn(ProcessCreateFlag_T, aFlags), ULONG aTimeoutMS,
                               ProcessPriority_T aPriority, ComSafeArrayIn(LONG, aAffinity),
                               IGuestProcess **aProcess);
    STDMETHOD(ProcessGet)(ULONG aPID, IGuestProcess **aProcess);
    STDMETHOD(SymlinkCreate)(IN_BSTR aSource, IN_BSTR aTarget, SymlinkType_T aType);
    STDMETHOD(SymlinkExists)(IN_BSTR aSymlink, BOOL *aExists);
    STDMETHOD(SymlinkRead)(IN_BSTR aSymlink, ComSafeArrayIn(SymlinkReadFlag_T, aFlags), BSTR *aTarget);
    STDMETHOD(SymlinkRemoveDirectory)(IN_BSTR aPath);
    STDMETHOD(SymlinkRemoveFile)(IN_BSTR aFile);
    STDMETHOD(WaitFor)(ULONG aWaitFlags, ULONG aTimeoutMS, GuestSessionWaitResult_T *aReason);
    STDMETHOD(WaitForArray)(ComSafeArrayIn(GuestSessionWaitForFlag_T, aFlags), ULONG aTimeoutMS, GuestSessionWaitResult_T *aReason);
    /** @}  */

private:

    /** Map of guest directories. The key specifies the internal directory ID. */
    typedef std::map <uint32_t, ComObjPtr<GuestDirectory> > SessionDirectories;
    /** Map of guest files. The key specifies the internal file ID. */
    typedef std::map <uint32_t, ComObjPtr<GuestFile> > SessionFiles;
    /** Map of guest processes. The key specifies the internal process number.
     *  To retrieve the process' guest PID use the Id() method of the IProcess interface. */
    typedef std::map <uint32_t, ComObjPtr<GuestProcess> > SessionProcesses;

public:
    /** @name Public internal methods.
     * @{ */
    int                     closeSession(uint32_t uFlags, uint32_t uTimeoutMS, int *pGuestRc);
    inline bool             directoryExists(uint32_t uDirID, ComObjPtr<GuestDirectory> *pDir);
    int                     directoryRemoveFromList(GuestDirectory *pDirectory);
    int                     directoryRemoveInternal(const Utf8Str &strPath, uint32_t uFlags, int *pGuestRc);
    int                     directoryCreateInternal(const Utf8Str &strPath, uint32_t uMode, uint32_t uFlags, int *pGuestRc);
    int                     objectCreateTempInternal(const Utf8Str &strTemplate, const Utf8Str &strPath, bool fDirectory, Utf8Str &strName, int *pGuestRc);
    int                     directoryOpenInternal(const GuestDirectoryOpenInfo &openInfo, ComObjPtr<GuestDirectory> &pDirectory, int *pGuestRc);
    int                     directoryQueryInfoInternal(const Utf8Str &strPath, GuestFsObjData &objData, int *pGuestRc);
    int                     dispatchToDirectory(PVBOXGUESTCTRLHOSTCBCTX pCtxCb, PVBOXGUESTCTRLHOSTCALLBACK pSvcCb);
    int                     dispatchToFile(PVBOXGUESTCTRLHOSTCBCTX pCtxCb, PVBOXGUESTCTRLHOSTCALLBACK pSvcCb);
    int                     dispatchToObject(PVBOXGUESTCTRLHOSTCBCTX pCtxCb, PVBOXGUESTCTRLHOSTCALLBACK pSvcCb);
    int                     dispatchToProcess(PVBOXGUESTCTRLHOSTCBCTX pCtxCb, PVBOXGUESTCTRLHOSTCALLBACK pSvcCb);
    int                     dispatchToThis(PVBOXGUESTCTRLHOSTCBCTX pCtxCb, PVBOXGUESTCTRLHOSTCALLBACK pSvcCb);
    inline bool             fileExists(uint32_t uFileID, ComObjPtr<GuestFile> *pFile);
    int                     fileRemoveFromList(GuestFile *pFile);
    int                     fileRemoveInternal(const Utf8Str &strPath, int *pGuestRc);
    int                     fileOpenInternal(const GuestFileOpenInfo &openInfo, ComObjPtr<GuestFile> &pFile, int *pGuestRc);
    int                     fileQueryInfoInternal(const Utf8Str &strPath, GuestFsObjData &objData, int *pGuestRc);
    int                     fileQuerySizeInternal(const Utf8Str &strPath, int64_t *pllSize, int *pGuestRc);
    int                     fsQueryInfoInternal(const Utf8Str &strPath, GuestFsObjData &objData, int *pGuestRc);
    const GuestCredentials &getCredentials(void);
    const GuestEnvironment &getEnvironment(void);
    EventSource            *getEventSource(void) { return mEventSource; }
    Utf8Str                 getName(void);
    ULONG                   getId(void) { return mData.mSession.mID; }
    static Utf8Str          guestErrorToString(int guestRc);
    HRESULT                 isReadyExternal(void);
    int                     onRemove(void);
    int                     onSessionStatusChange(PVBOXGUESTCTRLHOSTCBCTX pCbCtx, PVBOXGUESTCTRLHOSTCALLBACK pSvcCbData);
    int                     startSessionInternal(int *pGuestRc);
    int                     startSessionAsync(void);
    static DECLCALLBACK(int)
                            startSessionThread(RTTHREAD Thread, void *pvUser);
    Guest                  *getParent(void) { return mParent; }
    uint32_t                getProtocolVersion(void) { return mData.mProtocolVersion; }
    int                     pathRenameInternal(const Utf8Str &strSource, const Utf8Str &strDest, uint32_t uFlags, int *pGuestRc);
    int                     processRemoveFromList(GuestProcess *pProcess);
    int                     processCreateExInteral(GuestProcessStartupInfo &procInfo, ComObjPtr<GuestProcess> &pProgress);
    inline bool             processExists(uint32_t uProcessID, ComObjPtr<GuestProcess> *pProcess);
    inline int              processGetByPID(ULONG uPID, ComObjPtr<GuestProcess> *pProcess);
    int                     sendCommand(uint32_t uFunction, uint32_t uParms, PVBOXHGCMSVCPARM paParms);
    static HRESULT          setErrorExternal(VirtualBoxBase *pInterface, int guestRc);
    int                     setSessionStatus(GuestSessionStatus_T sessionStatus, int sessionRc);
    int                     signalWaiters(GuestSessionWaitResult_T enmWaitResult, int rc /*= VINF_SUCCESS */);
    int                     startTaskAsync(const Utf8Str &strTaskDesc, GuestSessionTask *pTask, ComObjPtr<Progress> &pProgress);
    int                     queryInfo(void);
    int                     waitFor(uint32_t fWaitFlags, ULONG uTimeoutMS, GuestSessionWaitResult_T &waitResult, int *pGuestRc);
    int                     waitForStatusChange(GuestWaitEvent *pEvent, uint32_t fWaitFlags, uint32_t uTimeoutMS, GuestSessionStatus_T *pSessionStatus, int *pGuestRc);
    /** @}  */

private:

    /** Pointer to the parent (Guest). */
    Guest                          *mParent;
    /**
     * The session's event source. This source is used for
     * serving the internal listener as well as all other
     * external listeners that may register to it.
     *
     * Note: This can safely be used without holding any locks.
     * An AutoCaller suffices to prevent it being destroy while in use and
     * internally there is a lock providing the necessary serialization.
     */
    const ComObjPtr<EventSource>    mEventSource;

    struct Data
    {
        /** The session credentials. */
        GuestCredentials            mCredentials;
        /** The session's startup info. */
        GuestSessionStartupInfo     mSession;
        /** The session's current status. */
        GuestSessionStatus_T        mStatus;
        /** The session's environment block. Can be
         *  overwritten/extended by ProcessCreate(Ex). */
        GuestEnvironment            mEnvironment;
        /** Directory objects bound to this session. */
        SessionDirectories          mDirectories;
        /** File objects bound to this session. */
        SessionFiles                mFiles;
        /** Process objects bound to this session. */
        SessionProcesses            mProcesses;
        /** Guest control protocol version to be used.
         *  Guest Additions < VBox 4.3 have version 1,
         *  any newer version will have version 2. */
        uint32_t                    mProtocolVersion;
        /** Session timeout (in ms). */
        uint32_t                    mTimeout;
        /** Total number of session objects (processes,
         *  files, ...). */
        uint32_t                    mNumObjects;
        /** The last returned session status
         *  returned from the guest side. */
        int                         mRC;
    } mData;
};

#endif /* !____H_GUESTSESSIONIMPL */

