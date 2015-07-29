/* $Id: MediumAttachmentImpl.cpp $ */
/** @file
 *
 * VirtualBox COM class implementation
 */

/*
 * Copyright (C) 2006-2015 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#include "MediumAttachmentImpl.h"
#include "MachineImpl.h"
#include "MediumImpl.h"
#include "Global.h"

#include "AutoCaller.h"
#include "Logging.h"

#include <iprt/cpp/utils.h>

////////////////////////////////////////////////////////////////////////////////
//
// private member data definition
//
////////////////////////////////////////////////////////////////////////////////

struct BackupableMediumAttachmentData
{
    BackupableMediumAttachmentData()
        : lPort(0),
          lDevice(0),
          type(DeviceType_Null),
          fPassthrough(false),
          fTempEject(false),
          fNonRotational(false),
          fDiscard(false),
          fImplicit(false),
          fHotPluggable(false)
    { }

    ComObjPtr<Medium>   pMedium;
    /* Since MediumAttachment is not a first class citizen when it
     * comes to managing settings, having a reference to the storage
     * controller will not work - when settings are changed it will point
     * to the old, uninitialized instance. Changing this requires
     * substantial changes to MediumImpl.cpp. */
    const Utf8Str       strControllerName;
    /* Same counts for the assigned bandwidth group */
    Utf8Str             strBandwidthGroup;
    const LONG          lPort;
    const LONG          lDevice;
    const DeviceType_T  type;
    bool                fPassthrough;
    bool                fTempEject;
    bool                fNonRotational;
    bool                fDiscard;
    bool                fImplicit;
    bool                fHotPluggable;
};

struct MediumAttachment::Data
{
    Data(Machine * const aMachine = NULL)
        : pMachine(aMachine),
          fIsEjected(false)
    { }

    /** Reference to Machine object, for checking mutable state. */
    Machine * const pMachine;
    /* later: const ComObjPtr<MediumAttachment> mPeer; */

    bool                fIsEjected;

    Backupable<BackupableMediumAttachmentData> bd;
};

// constructor / destructor
/////////////////////////////////////////////////////////////////////////////

DEFINE_EMPTY_CTOR_DTOR(MediumAttachment)

HRESULT MediumAttachment::FinalConstruct()
{
    LogFlowThisFunc(("\n"));
    return BaseFinalConstruct();
}

void MediumAttachment::FinalRelease()
{
    LogFlowThisFuncEnter();
    uninit();
    BaseFinalRelease();
    LogFlowThisFuncLeave();
}

// public initializer/uninitializer for internal purposes only
/////////////////////////////////////////////////////////////////////////////

/**
 * Initializes the medium attachment object.
 *
 * @param aParent           Machine object.
 * @param aMedium           Medium object.
 * @param aController       Controller the hard disk is attached to.
 * @param aPort             Port number.
 * @param aDevice           Device number on the port.
 * @param aPassthrough      Whether accesses are directly passed to the host drive.
 * @param aBandwidthLimit   Bandwidth limit in Mbps
 */
HRESULT MediumAttachment::init(Machine *aParent,
                               Medium *aMedium,
                               const Utf8Str &aControllerName,
                               LONG aPort,
                               LONG aDevice,
                               DeviceType_T aType,
                               bool aImplicit,
                               bool aPassthrough,
                               bool aTempEject,
                               bool aNonRotational,
                               bool aDiscard,
                               bool aHotPluggable,
                               const Utf8Str &strBandwidthGroup)
{
    LogFlowThisFuncEnter();
    LogFlowThisFunc(("aParent=%p aMedium=%p aControllerName=%s aPort=%d aDevice=%d aType=%d aImplicit=%d aPassthrough=%d aTempEject=%d aNonRotational=%d aDiscard=%d aHotPluggable=%d strBandwithGroup=%s\n", aParent, aMedium, aControllerName.c_str(), aPort, aDevice, aType, aImplicit, aPassthrough, aTempEject, aNonRotational, aDiscard, aHotPluggable, strBandwidthGroup.c_str()));

    if (aType == DeviceType_HardDisk)
        AssertReturn(aMedium, E_INVALIDARG);

    /* Enclose the state transition NotReady->InInit->Ready */
    AutoInitSpan autoInitSpan(this);
    AssertReturn(autoInitSpan.isOk(), E_FAIL);

    m = new Data();

    unconst(m->pMachine) = aParent;

    m->bd.allocate();
    m->bd->pMedium = aMedium;
    unconst(m->bd->strBandwidthGroup) = strBandwidthGroup;
    unconst(m->bd->strControllerName) = aControllerName;
    unconst(m->bd->lPort)   = aPort;
    unconst(m->bd->lDevice) = aDevice;
    unconst(m->bd->type)    = aType;

    m->bd->fPassthrough = aPassthrough;
    m->bd->fTempEject = aTempEject;
    m->bd->fNonRotational = aNonRotational;
    m->bd->fDiscard = aDiscard;
    m->bd->fImplicit = aImplicit;
    m->bd->fHotPluggable = aHotPluggable;

    /* Confirm a successful initialization when it's the case */
    autoInitSpan.setSucceeded();

    /* Construct a short log name for this attachment. */
    Utf8Str ctlName(aControllerName);
    const char *psz = strpbrk(ctlName.c_str(), " \t:-");
    mLogName = Utf8StrFmt("MA%p[%.*s:%u:%u:%s%s]",
                          this,
                          psz ? psz - ctlName.c_str() : 4, ctlName.c_str(),
                          aPort, aDevice, Global::stringifyDeviceType(aType),
                          m->bd->fImplicit ? ":I" : "");

    LogFlowThisFunc(("LEAVE - %s\n", i_getLogName()));
    return S_OK;
}

/**
 *  Initializes the medium attachment object given another guest object
 *  (a kind of copy constructor). This object makes a private copy of data
 *  of the original object passed as an argument.
 */
HRESULT MediumAttachment::initCopy(Machine *aParent, MediumAttachment *aThat)
{
    LogFlowThisFunc(("aParent=%p, aThat=%p\n", aParent, aThat));

    ComAssertRet(aParent && aThat, E_INVALIDARG);

    /* Enclose the state transition NotReady->InInit->Ready */
    AutoInitSpan autoInitSpan(this);
    AssertReturn(autoInitSpan.isOk(), E_FAIL);

    m = new Data(aParent);
    /* m->pPeer is left null */

    AutoCaller thatCaller(aThat);
    AssertComRCReturnRC(thatCaller.rc());

    AutoReadLock thatlock(aThat COMMA_LOCKVAL_SRC_POS);
    m->bd.attachCopy(aThat->m->bd);

    /* Confirm a successful initialization */
    autoInitSpan.setSucceeded();

    return S_OK;
}

/**
 * Uninitializes the instance.
 * Called from FinalRelease().
 */
void MediumAttachment::uninit()
{
    LogFlowThisFunc(("ENTER - %s\n", i_getLogName()));

    /* Enclose the state transition Ready->InUninit->NotReady */
    AutoUninitSpan autoUninitSpan(this);
    if (autoUninitSpan.uninitDone())
        return;

    m->bd.free();

    unconst(m->pMachine) = NULL;

    delete m;
    m = NULL;

    LogFlowThisFuncLeave();
}

// IHardDiskAttachment properties
/////////////////////////////////////////////////////////////////////////////


HRESULT MediumAttachment::getMedium(ComPtr<IMedium> &aHardDisk)
{
    LogFlowThisFuncEnter();

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    aHardDisk = m->bd->pMedium;

    LogFlowThisFuncLeave();
    return S_OK;
}


HRESULT MediumAttachment::getController(com::Utf8Str &aController)
{
    LogFlowThisFuncEnter();

    /* m->controller is constant during life time, no need to lock */
    aController = Utf8Str(m->bd->strControllerName);

    LogFlowThisFuncLeave();
    return S_OK;
}


HRESULT MediumAttachment::getPort(LONG *aPort)
{
    LogFlowThisFuncEnter();

    /* m->bd->port is constant during life time, no need to lock */
    *aPort = m->bd->lPort;

    LogFlowThisFuncLeave();
    return S_OK;
}

HRESULT  MediumAttachment::getDevice(LONG *aDevice)
{
    LogFlowThisFuncEnter();

    /* m->bd->device is constant during life time, no need to lock */
    *aDevice = m->bd->lDevice;

    LogFlowThisFuncLeave();
    return S_OK;
}

HRESULT MediumAttachment::getType(DeviceType_T *aType)
{
    LogFlowThisFuncEnter();

    /* m->bd->type is constant during life time, no need to lock */
    *aType = m->bd->type;

    LogFlowThisFuncLeave();
    return S_OK;
}


HRESULT MediumAttachment::getPassthrough(BOOL *aPassthrough)
{
    LogFlowThisFuncEnter();

    AutoReadLock lock(this COMMA_LOCKVAL_SRC_POS);

    *aPassthrough = m->bd->fPassthrough;

    LogFlowThisFuncLeave();
    return S_OK;
}


HRESULT MediumAttachment::getTemporaryEject(BOOL *aTemporaryEject)
{
    LogFlowThisFuncEnter();

    AutoReadLock lock(this COMMA_LOCKVAL_SRC_POS);

    *aTemporaryEject = m->bd->fTempEject;

    LogFlowThisFuncLeave();
    return S_OK;
}


HRESULT MediumAttachment::getIsEjected(BOOL *aEjected)
{
    LogFlowThisFuncEnter();

    AutoReadLock lock(this COMMA_LOCKVAL_SRC_POS);

    *aEjected = m->fIsEjected;

    LogFlowThisFuncLeave();
    return S_OK;
}


HRESULT MediumAttachment::getNonRotational(BOOL *aNonRotational)
{
    LogFlowThisFuncEnter();

    AutoReadLock lock(this COMMA_LOCKVAL_SRC_POS);

    *aNonRotational = m->bd->fNonRotational;

    LogFlowThisFuncLeave();
    return S_OK;
}

HRESULT MediumAttachment::getDiscard(BOOL *aDiscard)
{
    LogFlowThisFuncEnter();

    AutoReadLock lock(this COMMA_LOCKVAL_SRC_POS);

    *aDiscard = m->bd->fDiscard;

    LogFlowThisFuncLeave();
    return S_OK;
}


HRESULT MediumAttachment::getBandwidthGroup(ComPtr<IBandwidthGroup> &aBandwidthGroup)
{
    LogFlowThisFuncEnter();

    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    HRESULT hrc = S_OK;
    if (m->bd->strBandwidthGroup.isNotEmpty())
    {
        ComObjPtr<BandwidthGroup> pBwGroup;
        hrc = m->pMachine->i_getBandwidthGroup(m->bd->strBandwidthGroup, pBwGroup, true /* fSetError */);

        Assert(SUCCEEDED(hrc)); /* This is not allowed to fail because the existence of the
                                   group was checked when it was attached. */

        if (SUCCEEDED(hrc))
            pBwGroup.queryInterfaceTo(aBandwidthGroup.asOutParam());
    }

    LogFlowThisFuncLeave();
    return hrc;
}

HRESULT MediumAttachment::getHotPluggable(BOOL *aHotPluggable)
{
    LogFlowThisFuncEnter();

    AutoReadLock lock(this COMMA_LOCKVAL_SRC_POS);

    *aHotPluggable = m->bd->fHotPluggable;

    LogFlowThisFuncLeave();
    return S_OK;
}

/**
 *  @note Locks this object for writing.
 */
void MediumAttachment::i_rollback()
{
    LogFlowThisFunc(("ENTER - %s\n", i_getLogName()));

    /* sanity */
    AutoCaller autoCaller(this);
    AssertComRCReturnVoid(autoCaller.rc());

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    m->bd.rollback();

    LogFlowThisFunc(("LEAVE - %s\n", i_getLogName()));
}

/**
 *  @note Locks this object for writing.
 */
void MediumAttachment::i_commit()
{
    LogFlowThisFunc(("ENTER - %s\n", i_getLogName()));

    /* sanity */
    AutoCaller autoCaller(this);
    AssertComRCReturnVoid (autoCaller.rc());

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    if (m->bd.isBackedUp())
        m->bd.commit();

    LogFlowThisFunc(("LEAVE - %s\n", i_getLogName()));
}

bool MediumAttachment::i_isImplicit() const
{
    return m->bd->fImplicit;
}

void MediumAttachment::i_setImplicit(bool aImplicit)
{
    m->bd->fImplicit = aImplicit;
}

const ComObjPtr<Medium>& MediumAttachment::i_getMedium() const
{
    return m->bd->pMedium;
}

const Utf8Str &MediumAttachment::i_getControllerName() const
{
    return m->bd->strControllerName;
}

LONG MediumAttachment::i_getPort() const
{
    return m->bd->lPort;
}

LONG MediumAttachment::i_getDevice() const
{
    return m->bd->lDevice;
}

DeviceType_T MediumAttachment::i_getType() const
{
    return m->bd->type;
}

bool MediumAttachment::i_getPassthrough() const
{
    AutoReadLock lock(this COMMA_LOCKVAL_SRC_POS);
    return m->bd->fPassthrough;
}

bool MediumAttachment::i_getTempEject() const
{
    AutoReadLock lock(this COMMA_LOCKVAL_SRC_POS);
    return m->bd->fTempEject;
}

bool MediumAttachment::i_getNonRotational() const
{
    AutoReadLock lock(this COMMA_LOCKVAL_SRC_POS);
    return m->bd->fNonRotational;
}

bool MediumAttachment::i_getDiscard() const
{
    AutoReadLock lock(this COMMA_LOCKVAL_SRC_POS);
    return m->bd->fDiscard;
}

bool MediumAttachment::i_getHotPluggable() const
{
    AutoReadLock lock(this COMMA_LOCKVAL_SRC_POS);
    return m->bd->fHotPluggable;
}

Utf8Str& MediumAttachment::i_getBandwidthGroup() const
{
    return m->bd->strBandwidthGroup;
}

bool MediumAttachment::i_matches(const Utf8Str &aControllerName, LONG aPort, LONG aDevice)
{
    return (    aControllerName == m->bd->strControllerName
             && aPort == m->bd->lPort
             && aDevice == m->bd->lDevice);
}

/** Must be called from under this object's write lock. */
void MediumAttachment::i_updateName(const Utf8Str &aName)
{
    Assert(isWriteLockOnCurrentThread());

    m->bd.backup();
    unconst(m->bd->strControllerName) = aName;
}

/**
 * Sets the medium of this attachment and unsets the "implicit" flag.
 * @param aMedium
 */
void MediumAttachment::i_updateMedium(const ComObjPtr<Medium> &aMedium)
{
    Assert(isWriteLockOnCurrentThread());

    m->bd.backup();
    m->bd->pMedium = aMedium;
    m->bd->fImplicit = false;
    m->fIsEjected = false;
}

/** Must be called from under this object's write lock. */
void MediumAttachment::i_updatePassthrough(bool aPassthrough)
{
    Assert(isWriteLockOnCurrentThread());

    m->bd.backup();
    m->bd->fPassthrough = aPassthrough;
}

/** Must be called from under this object's write lock. */
void MediumAttachment::i_updateTempEject(bool aTempEject)
{
    Assert(isWriteLockOnCurrentThread());

    m->bd.backup();
    m->bd->fTempEject = aTempEject;
}

/** Must be called from under this object's write lock. */
void MediumAttachment::i_updateEjected()
{
    Assert(isWriteLockOnCurrentThread());

    m->fIsEjected = true;
}

/** Must be called from under this object's write lock. */
void MediumAttachment::i_updateNonRotational(bool aNonRotational)
{
    Assert(isWriteLockOnCurrentThread());

    m->bd.backup();
    m->bd->fNonRotational = aNonRotational;
}

/** Must be called from under this object's write lock. */
void MediumAttachment::i_updateDiscard(bool aDiscard)
{
    Assert(isWriteLockOnCurrentThread());

    m->bd.backup();
    m->bd->fDiscard = aDiscard;
}

/** Must be called from under this object's write lock. */
void MediumAttachment::i_updateHotPluggable(bool aHotPluggable)
{
    Assert(isWriteLockOnCurrentThread());

    m->bd.backup();
    m->bd->fHotPluggable = aHotPluggable;
}

void MediumAttachment::i_updateBandwidthGroup(const Utf8Str &aBandwidthGroup)
{
    LogFlowThisFuncEnter();
    Assert(isWriteLockOnCurrentThread());

    m->bd.backup();
    m->bd->strBandwidthGroup = aBandwidthGroup;

    LogFlowThisFuncLeave();
}

void MediumAttachment::i_updateParentMachine(Machine * const pMachine)
{
    LogFlowThisFunc(("ENTER - %s\n", i_getLogName()));
    /* sanity */
    AutoCaller autoCaller(this);
    AssertComRCReturnVoid (autoCaller.rc());

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    unconst(m->pMachine) = pMachine;

    LogFlowThisFunc(("LEAVE - %s\n", i_getLogName()));
}

