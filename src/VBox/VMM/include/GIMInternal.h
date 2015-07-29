/* $Id: GIMInternal.h $ */
/** @file
 * GIM - Internal header file.
 */

/*
 * Copyright (C) 2014-2015 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#ifndef ___GIMInternal_h
#define ___GIMInternal_h

#include <VBox/vmm/gim.h>
#include <VBox/vmm/pgm.h>
#include "GIMHvInternal.h"
#include "GIMKvmInternal.h"
#include "GIMMinimalInternal.h"

RT_C_DECLS_BEGIN

/** @defgroup grp_gim_int       Internal
 * @ingroup grp_gim
 * @internal
 * @{
 */

/** The saved state version. */
#define GIM_SAVED_STATE_VERSION         1

/**
 * GIM VM Instance data.
 * Changes to this must checked against the padding of the gim union in VM!
 */
typedef struct GIM
{
    /** The provider that is active for this VM. */
    GIMPROVIDERID                    enmProviderId;
    /** The interface implementation version. */
    uint32_t                         u32Version;

    /** Physical access handler type for semi-read-only MMIO2 memory. Lazy creation. */
    PGMPHYSHANDLERTYPE              hSemiReadOnlyMmio2Handler;
    /** Alignment padding. */
    uint32_t                        u32Padding;

    /** Pointer to the GIM device - ring-3 context. */
    R3PTRTYPE(PPDMDEVINS)            pDevInsR3;
#if 0
    /** Pointer to the provider's ring-3 hypercall handler. */
    R3PTRTYPE(PFNGIMHYPERCALL)       pfnHypercallR3;
    /** Pointer to the provider's ring-0 hypercall handler. */
    R0PTRTYPE(PFNGIMHYPERCALL)       pfnHypercallR0;
    /** Pointer to the provider's raw-mode context hypercall handler. */
    RCPTRTYPE(PFNGIMHYPERCALL)       pfnHypercallRC;

    /** Pointer to the provider's ring-3 MSR-read handler. */
    R3PTRTYPE(PFNGIMRDMSR)           pfnReadMsrR3;
    /** Pointer to the provider's ring-0 MSR-read handler. */
    R0PTRTYPE(PFNGIMRDMSR)           pfnReadMsrR0;
    /** Pointer to the provider's raw-mode context MSR-read handler. */
    RCPTRTYPE(PFNGIMRDMSR)           pfnReadMsrRC;

    /** Pointer to the provider's ring-3 MSR-read handler. */
    R3PTRTYPE(PFNGIMWDMSR)           pfnWriteMsrR3;
    /** Pointer to the provider's ring-0 MSR-read handler. */
    R0PTRTYPE(PFNGIMWDMSR)           pfnWriteMsrRR0;
    /** Pointer to the provider's raw-mode context MSR-read handler. */
    RCPTRTYPE(PFNGIMWDMSR)           pfnWriteMsrRRC;
#endif

    union
    {
        GIMHV  Hv;
        GIMKVM Kvm;
    } u;
} GIM;
/** Pointer to GIM VM instance data. */
typedef GIM *PGIM;


/**
 * GIM VMCPU Instance data.
 */
typedef struct GIMCPU
{
    union
    {
        GIMKVMCPU KvmCpu;
    } u;
} GIMCPU;
/** Pointer to GIM VMCPU instance data. */
typedef GIMCPU *PGIMCPU;

#ifdef IN_RING3
VMMR3_INT_DECL(int)           GIMR3Mmio2Unmap(PVM pVM, PGIMMMIO2REGION pRegion);
VMMR3_INT_DECL(int)           GIMR3Mmio2Map(PVM pVM, PGIMMMIO2REGION pRegion, RTGCPHYS GCPhysRegion);
VMMR3_INT_DECL(int)           GIMR3Mmio2HandlerPhysicalRegister(PVM pVM, PGIMMMIO2REGION pRegion);
VMMR3_INT_DECL(int)           GIMR3Mmio2HandlerPhysicalDeregister(PVM pVM, PGIMMMIO2REGION pRegion);
#endif /* IN_RING3 */

/** @} */

RT_C_DECLS_END

#endif

