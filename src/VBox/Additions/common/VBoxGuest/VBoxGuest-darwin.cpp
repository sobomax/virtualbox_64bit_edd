/* $Id: VBoxGuest-darwin.cpp $ */
/** @file
 * VBoxGuest - Darwin Specifics.
 */

/*
 * Copyright (C) 2006-2014 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

/*******************************************************************************
*   Header Files                                                               *
*******************************************************************************/
#define LOG_GROUP LOG_GROUP_VBGD
/*
 * Deal with conflicts first.
 * PVM - BSD mess, that FreeBSD has correct a long time ago.
 * iprt/types.h before sys/param.h - prevents UINT32_C and friends.
 */
#include <iprt/types.h>
#include <sys/param.h>
#undef PVM

#include <IOKit/IOLib.h> /* Assert as function */

#include <VBox/version.h>
#include <iprt/asm.h>
#include <iprt/initterm.h>
#include <iprt/assert.h>
#include <iprt/spinlock.h>
#include <iprt/semaphore.h>
#include <iprt/process.h>
#include <iprt/alloc.h>
#include <iprt/power.h>
#include <VBox/err.h>
#include <VBox/log.h>

#include <mach/kmod.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/ioccom.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/kauth.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include "VBoxGuestInternal.h"


/*******************************************************************************
*   Defined Constants And Macros                                               *
*******************************************************************************/

/** The system device node name. */
#define DEVICE_NAME_SYS     "vboxguest"
/** The user device node name. */
#define DEVICE_NAME_USR     "vboxguestu"



/*******************************************************************************
*   Internal Functions                                                         *
*******************************************************************************/
RT_C_DECLS_BEGIN
static kern_return_t    VbgdDarwinStart(struct kmod_info *pKModInfo, void *pvData);
static kern_return_t    VbgdDarwinStop(struct kmod_info *pKModInfo, void *pvData);
static int              VbgdDarwinCharDevRemove(void);

static int              VbgdDarwinOpen(dev_t Dev, int fFlags, int fDevType, struct proc *pProcess);
static int              VbgdDarwinClose(dev_t Dev, int fFlags, int fDevType, struct proc *pProcess);
static int              VbgdDarwinIOCtlSlow(PVBOXGUESTSESSION pSession, u_long iCmd, caddr_t pData, struct proc *pProcess);
static int              VbgdDarwinIOCtl(dev_t Dev, u_long iCmd, caddr_t pData, int fFlags, struct proc *pProcess);

static int              VbgdDarwinErr2DarwinErr(int rc);

static IOReturn         VbgdDarwinSleepHandler(void *pvTarget, void *pvRefCon, UInt32 uMessageType, IOService *pProvider, void *pvMessageArgument, vm_size_t argSize);
RT_C_DECLS_END


/*******************************************************************************
*   Structures and Typedefs                                                    *
*******************************************************************************/
/**
 * The service class for handling the VMMDev PCI device.
 *
 * Instantiated when the module is loaded (and on PCI hotplugging?).
 */
class org_virtualbox_VBoxGuest : public IOService
{
    OSDeclareDefaultStructors(org_virtualbox_VBoxGuest);

private:
    IOPCIDevice                *m_pIOPCIDevice;
    IOMemoryMap                *m_pMap;
    IOFilterInterruptEventSource *m_pInterruptSrc;

    bool setupVmmDevInterrupts(IOService *pProvider);
    bool disableVmmDevInterrupts(void);
    bool isVmmDev(IOPCIDevice *pIOPCIDevice);

public:
    virtual bool start(IOService *pProvider);
    virtual void stop(IOService *pProvider);
    virtual bool terminate(IOOptionBits fOptions);
};

OSDefineMetaClassAndStructors(org_virtualbox_VBoxGuest, IOService);


/**
 * An attempt at getting that clientDied() notification.
 * I don't think it'll work as I cannot figure out where/what creates the correct
 * port right.
 *
 * Instantiated when userland does IOServiceOpen().
 */
class org_virtualbox_VBoxGuestClient : public IOUserClient
{
    OSDeclareDefaultStructors(org_virtualbox_VBoxGuestClient);

private:
    PVBOXGUESTSESSION           m_pSession;     /**< The session. */
    task_t                      m_Task;         /**< The client task. */
    org_virtualbox_VBoxGuest   *m_pProvider;    /**< The service provider. */

public:
    virtual bool initWithTask(task_t OwningTask, void *pvSecurityId, UInt32 u32Type);
    virtual bool start(IOService *pProvider);
    static  void sessionClose(RTPROCESS Process);
    virtual IOReturn clientClose(void);
};

OSDefineMetaClassAndStructors(org_virtualbox_VBoxGuestClient, IOUserClient);



/*******************************************************************************
*   Global Variables                                                           *
*******************************************************************************/
/**
 * Declare the module stuff.
 */
RT_C_DECLS_BEGIN
extern kern_return_t _start(struct kmod_info *pKModInfo, void *pvData);
extern kern_return_t _stop(struct kmod_info *pKModInfo, void *pvData);

KMOD_EXPLICIT_DECL(VBoxGuest, VBOX_VERSION_STRING, _start, _stop)
DECLHIDDEN(kmod_start_func_t *) _realmain = VbgdDarwinStart;
DECLHIDDEN(kmod_stop_func_t *)  _antimain = VbgdDarwinStop;
DECLHIDDEN(int)                 _kext_apple_cc = __APPLE_CC__;
RT_C_DECLS_END


/**
 * Device extention & session data association structure.
 */
static VBOXGUESTDEVEXT  g_DevExt;

/**
 * The character device switch table for the driver.
 */
static struct cdevsw    g_DevCW =
{
    /*.d_open     = */ VbgdDarwinOpen,
    /*.d_close    = */ VbgdDarwinClose,
    /*.d_read     = */ eno_rdwrt,
    /*.d_write    = */ eno_rdwrt,
    /*.d_ioctl    = */ VbgdDarwinIOCtl,
    /*.d_stop     = */ eno_stop,
    /*.d_reset    = */ eno_reset,
    /*.d_ttys     = */ NULL,
    /*.d_select   = */ eno_select,
    /*.d_mmap     = */ eno_mmap,
    /*.d_strategy = */ eno_strat,
    /*.d_getc     = */ eno_getc,
    /*.d_putc     = */ eno_putc,
    /*.d_type     = */ 0
};

/** Major device number. */
static int                  g_iMajorDeviceNo    = -1;
/** Registered devfs device handle. */
static void                *g_hDevFsDeviceSys   = NULL;
/** Registered devfs device handle for the user device. */
static void                *g_hDevFsDeviceUsr   = NULL; /**< @todo 4 later */

/** Spinlock protecting g_apSessionHashTab. */
static RTSPINLOCK           g_Spinlock          = NIL_RTSPINLOCK;
/** Hash table */
static PVBOXGUESTSESSION    g_apSessionHashTab[19];
/** Calculates the index into g_apSessionHashTab.*/
#define SESSION_HASH(pid)   ((pid) % RT_ELEMENTS(g_apSessionHashTab))
/** The number of open sessions. */
static int32_t volatile     g_cSessions         = 0;
/** The number of IOService class instances. */
static bool volatile        g_fInstantiated     = 0;
/** The notifier handle for the sleep callback handler. */
static IONotifier          *g_pSleepNotifier    = NULL;



/**
 * Start the kernel module.
 */
static kern_return_t    VbgdDarwinStart(struct kmod_info *pKModInfo, void *pvData)
{
#ifdef DEBUG
    printf("VbgdDarwinStart\n");
#endif

    /*
     * Initialize IPRT.
     */
    int rc = RTR0Init(0);
    if (RT_FAILURE(rc))
    {
        printf("VBoxGuest: RTR0Init failed with rc=%d\n", rc);
        return KMOD_RETURN_FAILURE;
    }

    return KMOD_RETURN_SUCCESS;
}


/* Register VBoxGuest char device */
static int VbgdDarwinCharDevInit(void)
{
    int rc = RTSpinlockCreate(&g_Spinlock, RTSPINLOCK_FLAGS_INTERRUPT_SAFE, "VBoxGuestDarwin");
    if (RT_FAILURE(rc))
    {
        return KMOD_RETURN_FAILURE;
    }

    /*
     * Registering ourselves as a character device.
     */
    g_iMajorDeviceNo = cdevsw_add(-1, &g_DevCW);
    if (g_iMajorDeviceNo < 0)
    {
        VbgdDarwinCharDevRemove();
        return KMOD_RETURN_FAILURE;
    }

    g_hDevFsDeviceSys = devfs_make_node(makedev(g_iMajorDeviceNo, 0), DEVFS_CHAR,
                                        UID_ROOT, GID_WHEEL, 0666, DEVICE_NAME_SYS);
    if (!g_hDevFsDeviceSys)
    {
        VbgdDarwinCharDevRemove();
        return KMOD_RETURN_FAILURE;
    }

    /* Register a sleep/wakeup notification callback */
    g_pSleepNotifier = registerPrioritySleepWakeInterest(&VbgdDarwinSleepHandler, &g_DevExt, NULL);
    if (g_pSleepNotifier == NULL)
    {
        VbgdDarwinCharDevRemove();
        return KMOD_RETURN_FAILURE;
    }

    return KMOD_RETURN_SUCCESS;
}


/**
 * Stop the kernel module.
 */
static kern_return_t VbgdDarwinStop(struct kmod_info *pKModInfo, void *pvData)
{
    RTR0TermForced();
#ifdef DEBUG
    printf("VbgdDarwinStop - done\n");
#endif
    return KMOD_RETURN_SUCCESS;
}


/* Unregister VBoxGuest char device */
static int
VbgdDarwinCharDevRemove(void)
{
    int rc = KMOD_RETURN_SUCCESS;

    if (g_pSleepNotifier)
    {
        g_pSleepNotifier->remove();
        g_pSleepNotifier = NULL;
    }

    if (g_hDevFsDeviceSys)
    {
        devfs_remove(g_hDevFsDeviceSys);
        g_hDevFsDeviceSys = NULL;
    }

    if (g_hDevFsDeviceUsr)
    {
        devfs_remove(g_hDevFsDeviceUsr);
        g_hDevFsDeviceUsr = NULL;
    }

    if (g_iMajorDeviceNo != -1)
    {
        int rc2 = cdevsw_remove(g_iMajorDeviceNo, &g_DevCW);
        Assert(rc2 == g_iMajorDeviceNo);
        g_iMajorDeviceNo = -1;
    }

    if (g_Spinlock != NIL_RTSPINLOCK)
    {
        int rc2 = RTSpinlockDestroy(g_Spinlock); AssertRC(rc2);
        g_Spinlock = NIL_RTSPINLOCK;
    }

    return rc;
}


/**
 * Device open. Called on open /dev/vboxguest and (later) /dev/vboxguestu.
 *
 * @param   Dev         The device number.
 * @param   fFlags      ???.
 * @param   fDevType    ???.
 * @param   pProcess    The process issuing this request.
 */
static int VbgdDarwinOpen(dev_t Dev, int fFlags, int fDevType, struct proc *pProcess)
{
    /*
     * Only two minor devices numbers are allowed.
     */
    if (minor(Dev) != 0 && minor(Dev) != 1)
        return EACCES;

    /*
     * Find the session created by org_virtualbox_VBoxGuestClient, fail
     * if no such session, and mark it as opened. We set the uid & gid
     * here too, since that is more straight forward at this point.
     */
    //const bool          fUnrestricted = minor(Dev) == 0;
    int                 rc = VINF_SUCCESS;
    PVBOXGUESTSESSION   pSession = NULL;
    kauth_cred_t        pCred = kauth_cred_proc_ref(pProcess);
    if (pCred)
    {
        RTPROCESS       Process = RTProcSelf();
        unsigned        iHash = SESSION_HASH(Process);
        RTSpinlockAcquire(g_Spinlock);

        pSession = g_apSessionHashTab[iHash];
        while (pSession && pSession->Process != Process)
            pSession = pSession->pNextHash;
        if (pSession)
        {
            if (!pSession->fOpened)
            {
                pSession->fOpened = true;
                /*pSession->fUnrestricted = fUnrestricted; - later */
            }
            else
                rc = VERR_ALREADY_LOADED;
        }
        else
            rc = VERR_GENERAL_FAILURE;

        RTSpinlockRelease(g_Spinlock);
#if MAC_OS_X_VERSION_MIN_REQUIRED >= 1050
        kauth_cred_unref(&pCred);
#else  /* 10.4 */
        /* The 10.4u SDK headers and 10.4.11 kernel source have inconsistent definitions
           of kauth_cred_unref(), so use the other (now deprecated) API for releasing it. */
        kauth_cred_rele(pCred);
#endif /* 10.4 */
    }
    else
        rc = VERR_INVALID_PARAMETER;

    Log(("VbgdDarwinOpen: g_DevExt=%p pSession=%p rc=%d pid=%d\n", &g_DevExt, pSession, rc, proc_pid(pProcess)));
    return VbgdDarwinErr2DarwinErr(rc);
}


/**
 * Close device.
 */
static int VbgdDarwinClose(dev_t Dev, int fFlags, int fDevType, struct proc *pProcess)
{
    Log(("VbgdDarwinClose: pid=%d\n", (int)RTProcSelf()));
    Assert(proc_pid(pProcess) == (int)RTProcSelf());

    /*
     * Hand the session closing to org_virtualbox_VBoxGuestClient.
     */
    org_virtualbox_VBoxGuestClient::sessionClose(RTProcSelf());
    return 0;
}


/**
 * Device I/O Control entry point.
 *
 * @returns Darwin for slow IOCtls and VBox status code for the fast ones.
 * @param   Dev         The device number (major+minor).
 * @param   iCmd        The IOCtl command.
 * @param   pData       Pointer to the data (if any it's a VBOXGUESTIOCTLDATA (kernel copy)).
 * @param   fFlags      Flag saying we're a character device (like we didn't know already).
 * @param   pProcess    The process issuing this request.
 */
static int VbgdDarwinIOCtl(dev_t Dev, u_long iCmd, caddr_t pData, int fFlags, struct proc *pProcess)
{
    //const bool          fUnrestricted = minor(Dev) == 0;
    const RTPROCESS     Process = proc_pid(pProcess);
    const unsigned      iHash = SESSION_HASH(Process);
    PVBOXGUESTSESSION   pSession;

    /*
     * Find the session.
     */
    RTSpinlockAcquire(g_Spinlock);
    pSession = g_apSessionHashTab[iHash];
    while (pSession && pSession->Process != Process /*later: && pSession->fUnrestricted == fUnrestricted*/ && pSession->fOpened)
        pSession = pSession->pNextHash;
    RTSpinlockRelease(g_Spinlock);
    if (!pSession)
    {
        Log(("VBoxDrvDarwinIOCtl: WHAT?!? pSession == NULL! This must be a mistake... pid=%d iCmd=%#lx\n",
             (int)Process, iCmd));
        return EINVAL;
    }

    /*
     * No high speed IOCtls here yet.
     */

    return VbgdDarwinIOCtlSlow(pSession, iCmd, pData, pProcess);
}


/**
 * Worker for VbgdDarwinIOCtl that takes the slow IOCtl functions.
 *
 * @returns Darwin errno.
 *
 * @param pSession  The session.
 * @param iCmd      The IOCtl command.
 * @param pData     Pointer to the kernel copy of the data buffer.
 * @param pProcess  The calling process.
 */
static int VbgdDarwinIOCtlSlow(PVBOXGUESTSESSION pSession, u_long iCmd, caddr_t pData, struct proc *pProcess)
{
    LogFlow(("VbgdDarwinIOCtlSlow: pSession=%p iCmd=%p pData=%p pProcess=%p\n", pSession, iCmd, pData, pProcess));


    /*
     * Buffered or unbuffered?
     */
    void *pvReqData;
    user_addr_t pUser = 0;
    void *pvPageBuf = NULL;
    uint32_t cbReq = IOCPARM_LEN(iCmd);
    if ((IOC_DIRMASK & iCmd) == IOC_INOUT)
    {
        /*
         * Raw buffered request data, common code validates it.
         */
        pvReqData = pData;
    }
    else if ((IOC_DIRMASK & iCmd) == IOC_VOID && !cbReq)
    {
        /*
         * Get the header and figure out how much we're gonna have to read.
         */
        VBGLBIGREQ Hdr;
        pUser = (user_addr_t)*(void **)pData;
        int rc = copyin(pUser, &Hdr, sizeof(Hdr));
        if (RT_UNLIKELY(rc))
        {
            Log(("VbgdDarwinIOCtlSlow: copyin(%llx,Hdr,) -> %#x; iCmd=%#lx\n", (unsigned long long)pUser, rc, iCmd));
            return rc;
        }
        if (RT_UNLIKELY(Hdr.u32Magic != VBGLBIGREQ_MAGIC))
        {
            Log(("VbgdDarwinIOCtlSlow: bad magic u32Magic=%#x; iCmd=%#lx\n", Hdr.u32Magic, iCmd));
            return EINVAL;
        }
        cbReq = Hdr.cbData;
        if (RT_UNLIKELY(cbReq > _1M*16))
        {
            Log(("VbgdDarwinIOCtlSlow: %#x; iCmd=%#lx\n", Hdr.cbData, iCmd));
            return EINVAL;
        }
        pUser = Hdr.pvDataR3;

        /*
         * Allocate buffer and copy in the data.
         */
        pvReqData = RTMemTmpAlloc(cbReq);
        if (!pvReqData)
            pvPageBuf = pvReqData = IOMallocAligned(RT_ALIGN_Z(cbReq, PAGE_SIZE), 8);
        if (RT_UNLIKELY(!pvReqData))
        {
            Log(("VbgdDarwinIOCtlSlow: failed to allocate buffer of %d bytes; iCmd=%#lx\n", cbReq, iCmd));
            return ENOMEM;
        }
        rc = copyin(pUser, pvReqData, Hdr.cbData);
        if (RT_UNLIKELY(rc))
        {
            Log(("VbgdDarwinIOCtlSlow: copyin(%llx,%p,%#x) -> %#x; iCmd=%#lx\n",
                 (unsigned long long)pUser, pvReqData, Hdr.cbData, rc, iCmd));
            if (pvPageBuf)
                IOFreeAligned(pvPageBuf, RT_ALIGN_Z(cbReq, PAGE_SIZE));
            else
                RTMemTmpFree(pvReqData);
            return rc;
        }
    }
    else
    {
        Log(("VbgdDarwinIOCtlSlow: huh? cbReq=%#x iCmd=%#lx\n", cbReq, iCmd));
        return EINVAL;
    }

    /*
     * Process the IOCtl.
     */
    size_t cbReqRet = 0;
    int rc = VBoxGuestCommonIOCtl(iCmd, &g_DevExt, pSession, pvReqData, cbReq, &cbReqRet);
    if (RT_SUCCESS(rc))
    {
        /*
         * If not buffered, copy back the buffer before returning.
         */
        if (pUser)
        {
            if (cbReqRet > cbReq)
            {
                Log(("VbgdDarwinIOCtlSlow: too much output! %#x > %#x; uCmd=%#lx!\n", cbReqRet, cbReq, iCmd));
                cbReqRet = cbReq;
            }
            rc = copyout(pvReqData, pUser, cbReqRet);
            if (RT_UNLIKELY(rc))
                Log(("VbgdDarwinIOCtlSlow: copyout(%p,%llx,%#x) -> %d; uCmd=%#lx!\n",
                     pvReqData, (unsigned long long)pUser, cbReqRet, rc, iCmd));

            /* cleanup */
            if (pvPageBuf)
                IOFreeAligned(pvPageBuf, RT_ALIGN_Z(cbReq, PAGE_SIZE));
            else
                RTMemTmpFree(pvReqData);
        }
        else
            rc = 0;
    }
    else
    {
        /*
         * The request failed, just clean up.
         */
        if (pUser)
        {
            if (pvPageBuf)
                IOFreeAligned(pvPageBuf, RT_ALIGN_Z(cbReq, PAGE_SIZE));
            else
                RTMemTmpFree(pvReqData);
        }

        Log(("VbgdDarwinIOCtlSlow: pid=%d iCmd=%lx pData=%p failed, rc=%d\n", proc_pid(pProcess), iCmd, (void *)pData, rc));
        rc = EINVAL;
    }

    Log2(("VbgdDarwinIOCtlSlow: returns %d\n", rc));
    return rc;
}


/*
 * The VBoxGuest IDC entry points.
 *
 * This code is shared with the other unixy OSes.
 */
#include "VBoxGuestIDC-unix.c.h"


void VBoxGuestNativeISRMousePollEvent(PVBOXGUESTDEVEXT pDevExt)
{
    NOREF(pDevExt);
}


/**
 * Callback for blah blah blah.
 */
IOReturn VbgdDarwinSleepHandler(void * /* pvTarget */, void *pvRefCon, UInt32 uMessageType, IOService * /* pProvider */, void * /* pvMessageArgument */, vm_size_t /* argSize */)
{
    LogFlow(("VBoxGuest: Got sleep/wake notice. Message type was %X\n", (uint)uMessageType));

    if (uMessageType == kIOMessageSystemWillSleep)
        RTPowerSignalEvent(RTPOWEREVENT_SUSPEND);
    else if (uMessageType == kIOMessageSystemHasPoweredOn)
        RTPowerSignalEvent(RTPOWEREVENT_RESUME);

    acknowledgeSleepWakeNotification(pvRefCon);

    return 0;
}


/**
 * Converts an IPRT error code to a darwin error code.
 *
 * @returns corresponding darwin error code.
 * @param   rc      IPRT status code.
 */
static int VbgdDarwinErr2DarwinErr(int rc)
{
    switch (rc)
    {
        case VINF_SUCCESS:              return 0;
        case VERR_GENERAL_FAILURE:      return EACCES;
        case VERR_INVALID_PARAMETER:    return EINVAL;
        case VERR_INVALID_MAGIC:        return EILSEQ;
        case VERR_INVALID_HANDLE:       return ENXIO;
        case VERR_INVALID_POINTER:      return EFAULT;
        case VERR_LOCK_FAILED:          return ENOLCK;
        case VERR_ALREADY_LOADED:       return EEXIST;
        case VERR_PERMISSION_DENIED:    return EPERM;
        case VERR_VERSION_MISMATCH:     return ENOSYS;
    }

    return EPERM;
}


/*
 *
 * org_virtualbox_VBoxGuest
 *
 */

/**
 * Just a plug
 */
static void
interruptHandler(OSObject *pOwner, IOInterruptEventSource *pSrc, int cInts)
{
    NOREF(pOwner);
    NOREF(pSrc);
    NOREF(cInts);
}

/**
 * Callback triggered when interrupt occurs.
 */
static bool
checkForInterrupt(OSObject *pOwner, IOFilterInterruptEventSource *pSrc)
{
    if (!pSrc)
        return false;

    bool fTaken = VBoxGuestCommonISR(&g_DevExt);
    if (!fTaken)
        printf("VBoxGuestCommonISR error\n");

    return fTaken;
}

bool
org_virtualbox_VBoxGuest::setupVmmDevInterrupts(IOService *pProvider)
{
    IOWorkLoop *pWorkLoop = (IOWorkLoop *)getWorkLoop();

    if (!pWorkLoop)
        return false;

    m_pInterruptSrc = IOFilterInterruptEventSource::filterInterruptEventSource(this,
                                                                               &interruptHandler,
                                                                               &checkForInterrupt,
                                                                               pProvider);

    if (kIOReturnSuccess != pWorkLoop->addEventSource(m_pInterruptSrc))
    {
        m_pInterruptSrc->disable();
        m_pInterruptSrc->release();
        m_pInterruptSrc = 0;
        return false;
    }

    m_pInterruptSrc->enable();

    return true;
}

bool
org_virtualbox_VBoxGuest::disableVmmDevInterrupts(void)
{
    IOWorkLoop *pWorkLoop = (IOWorkLoop *)getWorkLoop();

    if (!pWorkLoop)
        return false;

    if (!m_pInterruptSrc)
        return false;

    m_pInterruptSrc->disable();
    pWorkLoop->removeEventSource(m_pInterruptSrc);
    m_pInterruptSrc->release();
    m_pInterruptSrc = 0;

    return true;
}

bool org_virtualbox_VBoxGuest::isVmmDev(IOPCIDevice *pIOPCIDevice)
{
    UInt16 uVendorId, uDeviceId;

    if (!pIOPCIDevice)
        return false;

    uVendorId = m_pIOPCIDevice->configRead16(kIOPCIConfigVendorID);
    uDeviceId = m_pIOPCIDevice->configRead16(kIOPCIConfigDeviceID);

    if (uVendorId == VMMDEV_VENDORID && uDeviceId == VMMDEV_DEVICEID)
        return true;

    return true;
}


/**
 * Start this service.
 */
bool org_virtualbox_VBoxGuest::start(IOService *pProvider)
{
    if (!IOService::start(pProvider))
        return false;

    /* Low level initialization should be performed only once */
    if (!ASMAtomicCmpXchgBool(&g_fInstantiated, true, false))
    {
        IOService::stop(pProvider);
        return false;
    }

    m_pIOPCIDevice = OSDynamicCast(IOPCIDevice, pProvider);
    if (m_pIOPCIDevice)
    {
        if (isVmmDev(m_pIOPCIDevice))
        {
            /* Enable memory response from VMM device */
            m_pIOPCIDevice->setMemoryEnable(true);
            m_pIOPCIDevice->setIOEnable(true);

            IOMemoryDescriptor *pMem = m_pIOPCIDevice->getDeviceMemoryWithIndex(0);
            if (pMem)
            {
                IOPhysicalAddress IOPortBasePhys = pMem->getPhysicalAddress();
                /* Check that returned value is from I/O port range (at least it is 16-bit lenght) */
                if((IOPortBasePhys >> 16) == 0)
                {

                    RTIOPORT IOPortBase = (RTIOPORT)IOPortBasePhys;
                    void    *pvMMIOBase = NULL;
                    uint32_t cbMMIO     = 0;
                    m_pMap = m_pIOPCIDevice->mapDeviceMemoryWithIndex(1);
                    if (m_pMap)
                    {
                        pvMMIOBase = (void *)m_pMap->getVirtualAddress();
                        cbMMIO     = m_pMap->getLength();
                    }

                    int rc = VBoxGuestInitDevExt(&g_DevExt,
                                                 IOPortBase,
                                                 pvMMIOBase,
                                                 cbMMIO,
#if ARCH_BITS == 64
                                                 VBOXOSTYPE_MacOS_x64,
#else
                                                 VBOXOSTYPE_MacOS,
#endif
                                                 0);
                    if (RT_SUCCESS(rc))
                    {
                        rc = VbgdDarwinCharDevInit();
                        if (rc == KMOD_RETURN_SUCCESS)
                        {
                            if (setupVmmDevInterrupts(pProvider))
                            {
                                /* register the service. */
                                registerService();
                                LogRel(("VBoxGuest: Successfully started I/O kit class instance.\n"));
                                return true;
                            }

                            LogRel(("VBoxGuest: Failed to set up interrupts\n"));
                            VbgdDarwinCharDevRemove();
                        }
                        else
                            LogRel(("VBoxGuest: Failed to initialize character device (rc=%d).\n", rc));

                        VBoxGuestDeleteDevExt(&g_DevExt);
                    }
                    else
                        LogRel(("VBoxGuest: Failed to initialize common code (rc=%d).\n", rc));

                    if (m_pMap)
                    {
                        m_pMap->release();
                        m_pMap = NULL;
                    }
                }
            }
            else
                LogRel(("VBoxGuest: The device missing is the I/O port range (#0).\n"));
        }
        else
            LogRel(("VBoxGuest: Not the VMMDev (%#x:%#x).\n",
                   m_pIOPCIDevice->configRead16(kIOPCIConfigVendorID), m_pIOPCIDevice->configRead16(kIOPCIConfigDeviceID)));
    }
    else
        LogRel(("VBoxGuest: Provider is not an instance of IOPCIDevice.\n"));

    ASMAtomicXchgBool(&g_fInstantiated, false);

    IOService::stop(pProvider);

    return false;
}


/**
 * Stop this service.
 */
void org_virtualbox_VBoxGuest::stop(IOService *pProvider)
{
    LogFlow(("org_virtualbox_VBoxGuest::stop([%p], %p)\n", this, pProvider));

    AssertReturnVoid(ASMAtomicReadBool(&g_fInstantiated));

    /* Low level termination should be performed only once */
    if (!disableVmmDevInterrupts())
        LogRel(("vboxguest: unable to unregister interrupt handler\n"));

    VbgdDarwinCharDevRemove();
    VBoxGuestDeleteDevExt(&g_DevExt);

    if (m_pMap)
    {
        m_pMap->release();
        m_pMap = NULL;
    }

    IOService::stop(pProvider);

    ASMAtomicWriteBool(&g_fInstantiated, false);

    LogRel(("vboxguest module unloaded\n"));
}


/**
 * Termination request.
 *
 * @return  true if we're ok with shutting down now, false if we're not.
 * @param   fOptions        Flags.
 */
bool org_virtualbox_VBoxGuest::terminate(IOOptionBits fOptions)
{
    bool fRc;
    LogFlow(("org_virtualbox_VBoxGuest::terminate: reference_count=%d g_cSessions=%d (fOptions=%#x)\n",
             KMOD_INFO_NAME.reference_count, ASMAtomicUoReadS32(&g_cSessions), fOptions));
    if (    KMOD_INFO_NAME.reference_count != 0
        ||  ASMAtomicUoReadS32(&g_cSessions))
        fRc = false;
    else
        fRc = IOService::terminate(fOptions);
    LogFlow(("org_virtualbox_SupDrv::terminate: returns %d\n", fRc));
    return fRc;
}


/*
 *
 * org_virtualbox_VBoxGuestClient
 *
 */


/**
 * Initializer called when the client opens the service.
 */
bool org_virtualbox_VBoxGuestClient::initWithTask(task_t OwningTask, void *pvSecurityId, UInt32 u32Type)
{
    LogFlow(("org_virtualbox_VBoxGuestClient::initWithTask([%p], %#x, %p, %#x) (cur pid=%d proc=%p)\n",
             this, OwningTask, pvSecurityId, u32Type, RTProcSelf(), RTR0ProcHandleSelf()));
    AssertMsg((RTR0PROCESS)OwningTask == RTR0ProcHandleSelf(), ("%p %p\n", OwningTask, RTR0ProcHandleSelf()));

    if (!OwningTask)
        return false;

    if (u32Type != VBOXGUEST_DARWIN_IOSERVICE_COOKIE)
    {
        Log(("org_virtualbox_VBoxGuestClient::initWithTask: Bad cookie %#x\n", u32Type));
        return false;
    }

    if (IOUserClient::initWithTask(OwningTask, pvSecurityId , u32Type))
    {
        m_Task = OwningTask;
        m_pSession = NULL;
        m_pProvider = NULL;
        return true;
    }
    return false;
}


/**
 * Start the client service.
 */
bool org_virtualbox_VBoxGuestClient::start(IOService *pProvider)
{
    LogFlow(("org_virtualbox_VBoxGuestClient::start([%p], %p) (cur pid=%d proc=%p)\n",
             this, pProvider, RTProcSelf(), RTR0ProcHandleSelf() ));
    AssertMsgReturn((RTR0PROCESS)m_Task == RTR0ProcHandleSelf(),
                    ("%p %p\n", m_Task, RTR0ProcHandleSelf()),
                    false);

    if (IOUserClient::start(pProvider))
    {
        m_pProvider = OSDynamicCast(org_virtualbox_VBoxGuest, pProvider);
        if (m_pProvider)
        {
            Assert(!m_pSession);

            /*
             * Create a new session.
             */
            int rc = VBoxGuestCreateUserSession(&g_DevExt, &m_pSession);
            if (RT_SUCCESS(rc))
            {
                m_pSession->fOpened = false;
                /* The fUnrestricted field is set on open. */

                /*
                 * Insert it into the hash table, checking that there isn't
                 * already one for this process first. (One session per proc!)
                 */
                unsigned iHash = SESSION_HASH(m_pSession->Process);
                RTSpinlockAcquire(g_Spinlock);

                PVBOXGUESTSESSION pCur = g_apSessionHashTab[iHash];
                if (pCur && pCur->Process != m_pSession->Process)
                {
                    do pCur = pCur->pNextHash;
                    while (pCur && pCur->Process != m_pSession->Process);
                }
                if (!pCur)
                {
                    m_pSession->pNextHash = g_apSessionHashTab[iHash];
                    g_apSessionHashTab[iHash] = m_pSession;
                    m_pSession->pvVBoxGuestClient = this;
                    ASMAtomicIncS32(&g_cSessions);
                    rc = VINF_SUCCESS;
                }
                else
                    rc = VERR_ALREADY_LOADED;

                RTSpinlockRelease(g_Spinlock);
                if (RT_SUCCESS(rc))
                {
                    Log(("org_virtualbox_VBoxGuestClient::start: created session %p for pid %d\n", m_pSession, (int)RTProcSelf()));
                    return true;
                }

                LogFlow(("org_virtualbox_VBoxGuestClient::start: already got a session for this process (%p)\n", pCur));
                VBoxGuestCloseSession(&g_DevExt, m_pSession);
            }

            m_pSession = NULL;
            LogFlow(("org_virtualbox_VBoxGuestClient::start: rc=%Rrc from supdrvCreateSession\n", rc));
        }
        else
            LogFlow(("org_virtualbox_VBoxGuestClient::start: %p isn't org_virtualbox_VBoxGuest\n", pProvider));
    }
    return false;
}


/**
 * Common worker for clientClose and VBoxDrvDarwinClose.
 */
/* static */ void org_virtualbox_VBoxGuestClient::sessionClose(RTPROCESS Process)
{
    /*
     * Find the session and remove it from the hash table.
     *
     * Note! Only one session per process. (Both start() and
     * VbgdDarwinOpen makes sure this is so.)
     */
    const unsigned  iHash = SESSION_HASH(Process);
    RTSpinlockAcquire(g_Spinlock);
    PVBOXGUESTSESSION  pSession = g_apSessionHashTab[iHash];
    if (pSession)
    {
        if (pSession->Process == Process)
        {
            g_apSessionHashTab[iHash] = pSession->pNextHash;
            pSession->pNextHash = NULL;
            ASMAtomicDecS32(&g_cSessions);
        }
        else
        {
            PVBOXGUESTSESSION pPrev = pSession;
            pSession = pSession->pNextHash;
            while (pSession)
            {
                if (pSession->Process == Process)
                {
                    pPrev->pNextHash = pSession->pNextHash;
                    pSession->pNextHash = NULL;
                    ASMAtomicDecS32(&g_cSessions);
                    break;
                }

                /* next */
                pPrev = pSession;
                pSession = pSession->pNextHash;
            }
        }
    }
    RTSpinlockRelease(g_Spinlock);
    if (!pSession)
    {
        Log(("VBoxGuestClient::sessionClose: pSession == NULL, pid=%d; freed already?\n", (int)Process));
        return;
    }

    /*
     * Remove it from the client object.
     */
    org_virtualbox_VBoxGuestClient *pThis = (org_virtualbox_VBoxGuestClient *)pSession->pvVBoxGuestClient;
    pSession->pvVBoxGuestClient = NULL;
    if (pThis)
    {
        Assert(pThis->m_pSession == pSession);
        pThis->m_pSession = NULL;
    }

    /*
     * Close the session.
     */
    VBoxGuestCloseSession(&g_DevExt, pSession);
}


/**
 * Client exits normally.
 */
IOReturn org_virtualbox_VBoxGuestClient::clientClose(void)
{
    LogFlow(("org_virtualbox_VBoxGuestClient::clientClose([%p]) (cur pid=%d proc=%p)\n", this, RTProcSelf(), RTR0ProcHandleSelf()));
    AssertMsg((RTR0PROCESS)m_Task == RTR0ProcHandleSelf(), ("%p %p\n", m_Task, RTR0ProcHandleSelf()));

    /*
     * Clean up the session if it's still around.
     *
     * We cannot rely 100% on close, and in the case of a dead client
     * we'll end up hanging inside vm_map_remove() if we postpone it.
     */
    if (m_pSession)
    {
        sessionClose(RTProcSelf());
        Assert(!m_pSession);
    }

    m_pProvider = NULL;
    terminate();

    return kIOReturnSuccess;
}

