/* $Id: DevVGA-SVGA.cpp $ */
/** @file
 * VMWare SVGA device.
 *
 * Logging levels guidelines for this and related files:
 *  - Log() for normal bits.
 *  - LogFlow() for more info.
 *  - Log2 for hex dump of cursor data.
 *  - Log3 for hex dump of shader code.
 *  - Log4 for hex dumps of 3D data.
 */

/*
 * Copyright (C) 2013-2015 Oracle Corporation
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
#define LOG_GROUP LOG_GROUP_DEV_VMSVGA
#define VMSVGA_USE_EMT_HALT_CODE
#include <VBox/vmm/pdmdev.h>
#include <VBox/version.h>
#include <VBox/err.h>
#include <VBox/log.h>
#include <VBox/vmm/pgm.h>
#ifdef VMSVGA_USE_EMT_HALT_CODE
# include <VBox/vmm/vmapi.h>
# include <VBox/vmm/vmcpuset.h>
#endif
#include <VBox/sup.h>

#include <iprt/assert.h>
#include <iprt/semaphore.h>
#include <iprt/uuid.h>
#ifdef IN_RING3
# include <iprt/mem.h>
#endif

#include <VBox/VMMDev.h>
#include <VBox/VBoxVideo.h>
#include <VBox/bioslogo.h>

/* should go BEFORE any other DevVGA include to make all DevVGA.h config defines be visible */
#include "DevVGA.h"

#ifdef DEBUG
/* Enable to log FIFO register accesses. */
//# define DEBUG_FIFO_ACCESS
/* Enable to log GMR page accesses. */
//# define DEBUG_GMR_ACCESS
#endif

#include "DevVGA-SVGA.h"
#include "vmsvga/svga_reg.h"
#include "vmsvga/svga_escape.h"
#include "vmsvga/svga_overlay.h"
#include "vmsvga/svga3d_reg.h"
#include "vmsvga/svga3d_caps.h"
#ifdef VBOX_WITH_VMSVGA3D
# include "DevVGA-SVGA3d.h"
# ifdef RT_OS_DARWIN
#  include "DevVGA-SVGA3d-cocoa.h"
# endif
#endif


/*******************************************************************************
*   Defined Constants And Macros                                               *
*******************************************************************************/
/**
 * Macro for checking if a fixed FIFO register is valid according to the
 * current FIFO configuration.
 *
 * @returns true / false.
 * @param   a_iIndex        The fifo register index (like SVGA_FIFO_CAPABILITIES).
 * @param   a_offFifoMin    A valid SVGA_FIFO_MIN value.
 */
#define VMSVGA_IS_VALID_FIFO_REG(a_iIndex, a_offFifoMin) ( ((a_iIndex) + 1) * sizeof(uint32_t) <= (a_offFifoMin) )


/*******************************************************************************
*   Structures and Typedefs                                                    *
*******************************************************************************/
/* 64-bit GMR descriptor */
typedef struct
{
   RTGCPHYS GCPhys;
   uint64_t numPages;
} VMSVGAGMRDESCRIPTOR, *PVMSVGAGMRDESCRIPTOR;

/* GMR slot */
typedef struct
{
    uint32_t                    cMaxPages;
    uint32_t                    cbTotal;
    uint32_t                    numDescriptors;
    PVMSVGAGMRDESCRIPTOR        paDesc;
} GMR, *PGMR;

/* Internal SVGA state. */
typedef struct
{
    GMR                     aGMR[VMSVGA_MAX_GMR_IDS];
    struct
    {
        SVGAGuestPtr        ptr;
        uint32_t            bytesPerLine;
        SVGAGMRImageFormat  format;
    } GMRFB;
    struct
    {
        bool                fActive;
        uint32_t            xHotspot;
        uint32_t            yHotspot;
        uint32_t            width;
        uint32_t            height;
        uint32_t            cbData;
        void               *pData;
    } Cursor;
    SVGAColorBGRX           colorAnnotation;

#ifdef VMSVGA_USE_EMT_HALT_CODE
    /** Number of EMTs in BusyDelayedEmts (quicker than scanning the set). */
    uint32_t volatile       cBusyDelayedEmts;
    /** Set of EMTs that are   */
    VMCPUSET                BusyDelayedEmts;
#else
    /** Number of EMTs waiting on hBusyDelayedEmts. */
    uint32_t volatile       cBusyDelayedEmts;
    /** Semaphore that EMTs wait on when reading SVGA_REG_BUSY and the FIFO is
     *  busy (ugly).  */
    RTSEMEVENTMULTI         hBusyDelayedEmts;
#endif
    /** Tracks how much time we waste reading SVGA_REG_BUSY with a busy FIFO. */
    STAMPROFILE             StatBusyDelayEmts;

    STAMPROFILE             StatR3CmdPresent;
    STAMPROFILE             StatR3CmdDrawPrimitive;
    STAMPROFILE             StatR3CmdSurfaceDMA;

    STAMCOUNTER             StatFifoCommands;
    STAMCOUNTER             StatFifoErrors;
    STAMCOUNTER             StatFifoUnkCmds;
    STAMCOUNTER             StatFifoTodoTimeout;
    STAMCOUNTER             StatFifoTodoWoken;
    STAMPROFILE             StatFifoStalls;

} VMSVGASTATE, *PVMSVGASTATE;


/*******************************************************************************
*   Internal Functions                                                         *
*******************************************************************************/
#ifdef IN_RING3
# ifdef DEBUG_FIFO_ACCESS
static FNPGMPHYSHANDLER vmsvgaR3FIFOAccessHandler;
# endif
# ifdef DEBUG_GMR_ACCESS
static FNPGMPHYSHANDLER vmsvgaR3GMRAccessHandler;
# endif
#endif


/*******************************************************************************
*   Global Variables                                                           *
*******************************************************************************/
#ifdef IN_RING3

/**
 * SSM descriptor table for the VMSVGAGMRDESCRIPTOR structure.
 */
static SSMFIELD const g_aVMSVGAGMRDESCRIPTORFields[] =
{
    SSMFIELD_ENTRY_GCPHYS(      VMSVGAGMRDESCRIPTOR,     GCPhys),
    SSMFIELD_ENTRY(             VMSVGAGMRDESCRIPTOR,     numPages),
    SSMFIELD_ENTRY_TERM()
};

/**
 * SSM descriptor table for the GMR structure.
 */
static SSMFIELD const g_aGMRFields[] =
{
    SSMFIELD_ENTRY(             GMR, cMaxPages),
    SSMFIELD_ENTRY(             GMR, cbTotal),
    SSMFIELD_ENTRY(             GMR, numDescriptors),
    SSMFIELD_ENTRY_IGN_HCPTR(   GMR, paDesc),
    SSMFIELD_ENTRY_TERM()
};

/**
 * SSM descriptor table for the VMSVGASTATE structure.
 */
static SSMFIELD const g_aVMSVGASTATEFields[] =
{
    SSMFIELD_ENTRY_IGNORE(      VMSVGASTATE, aGMR),
    SSMFIELD_ENTRY(             VMSVGASTATE, GMRFB),
    SSMFIELD_ENTRY(             VMSVGASTATE, Cursor.fActive),
    SSMFIELD_ENTRY(             VMSVGASTATE, Cursor.xHotspot),
    SSMFIELD_ENTRY(             VMSVGASTATE, Cursor.yHotspot),
    SSMFIELD_ENTRY(             VMSVGASTATE, Cursor.width),
    SSMFIELD_ENTRY(             VMSVGASTATE, Cursor.height),
    SSMFIELD_ENTRY(             VMSVGASTATE, Cursor.cbData),
    SSMFIELD_ENTRY_IGN_HCPTR(   VMSVGASTATE, Cursor.pData),
    SSMFIELD_ENTRY(             VMSVGASTATE, colorAnnotation),
    SSMFIELD_ENTRY_IGNORE(      VMSVGASTATE, cBusyDelayedEmts),
#ifdef VMSVGA_USE_EMT_HALT_CODE
    SSMFIELD_ENTRY_IGNORE(      VMSVGASTATE, BusyDelayedEmts),
#else
    SSMFIELD_ENTRY_IGNORE(      VMSVGASTATE, hBusyDelayedEmts),
#endif
    SSMFIELD_ENTRY_IGNORE(      VMSVGASTATE, StatBusyDelayEmts),
    SSMFIELD_ENTRY_IGNORE(      VMSVGASTATE, StatR3CmdPresent),
    SSMFIELD_ENTRY_IGNORE(      VMSVGASTATE, StatR3CmdDrawPrimitive),
    SSMFIELD_ENTRY_IGNORE(      VMSVGASTATE, StatR3CmdSurfaceDMA),
    SSMFIELD_ENTRY_IGNORE(      VMSVGASTATE, StatFifoCommands),
    SSMFIELD_ENTRY_IGNORE(      VMSVGASTATE, StatFifoErrors),
    SSMFIELD_ENTRY_IGNORE(      VMSVGASTATE, StatFifoUnkCmds),
    SSMFIELD_ENTRY_IGNORE(      VMSVGASTATE, StatFifoTodoTimeout),
    SSMFIELD_ENTRY_IGNORE(      VMSVGASTATE, StatFifoTodoWoken),
    SSMFIELD_ENTRY_IGNORE(      VMSVGASTATE, StatFifoStalls),
    SSMFIELD_ENTRY_TERM()
};

/**
 * SSM descriptor table for the VGAState.svga structure.
 */
static SSMFIELD const g_aVGAStateSVGAFields[] =
{
    SSMFIELD_ENTRY_IGNORE(          VMSVGAState, u64HostWindowId),
    SSMFIELD_ENTRY_IGN_HCPTR(       VMSVGAState, pFIFOR3),
    SSMFIELD_ENTRY_IGN_HCPTR(       VMSVGAState, pFIFOR0),
    SSMFIELD_ENTRY_IGN_HCPTR(       VMSVGAState, pSVGAState),
    SSMFIELD_ENTRY_IGN_HCPTR(       VMSVGAState, p3dState),
    SSMFIELD_ENTRY_IGN_HCPTR(       VMSVGAState, pFrameBufferBackup),
    SSMFIELD_ENTRY_IGN_HCPTR(       VMSVGAState, pFIFOExtCmdParam),
    SSMFIELD_ENTRY_IGN_GCPHYS(      VMSVGAState, GCPhysFIFO),
    SSMFIELD_ENTRY_IGNORE(          VMSVGAState, cbFIFO),
    SSMFIELD_ENTRY(                 VMSVGAState, u32SVGAId),
    SSMFIELD_ENTRY(                 VMSVGAState, fEnabled),
    SSMFIELD_ENTRY(                 VMSVGAState, fConfigured),
    SSMFIELD_ENTRY(                 VMSVGAState, fBusy),
    SSMFIELD_ENTRY(                 VMSVGAState, fTraces),
    SSMFIELD_ENTRY(                 VMSVGAState, u32GuestId),
    SSMFIELD_ENTRY(                 VMSVGAState, cScratchRegion),
    SSMFIELD_ENTRY(                 VMSVGAState, au32ScratchRegion),
    SSMFIELD_ENTRY(                 VMSVGAState, u32IrqStatus),
    SSMFIELD_ENTRY(                 VMSVGAState, u32IrqMask),
    SSMFIELD_ENTRY(                 VMSVGAState, u32PitchLock),
    SSMFIELD_ENTRY(                 VMSVGAState, u32CurrentGMRId),
    SSMFIELD_ENTRY(                 VMSVGAState, u32RegCaps),
    SSMFIELD_ENTRY_IGNORE(          VMSVGAState, BasePort),
    SSMFIELD_ENTRY(                 VMSVGAState, u32IndexReg),
    SSMFIELD_ENTRY_IGNORE(          VMSVGAState, pSupDrvSession),
    SSMFIELD_ENTRY_IGNORE(          VMSVGAState, FIFORequestSem),
    SSMFIELD_ENTRY_IGNORE(          VMSVGAState, FIFOExtCmdSem),
    SSMFIELD_ENTRY_IGN_HCPTR(       VMSVGAState, pFIFOIOThread),
    SSMFIELD_ENTRY(                 VMSVGAState, uWidth),
    SSMFIELD_ENTRY(                 VMSVGAState, uHeight),
    SSMFIELD_ENTRY(                 VMSVGAState, uBpp),
    SSMFIELD_ENTRY(                 VMSVGAState, cbScanline),
    SSMFIELD_ENTRY(                 VMSVGAState, u32MaxWidth),
    SSMFIELD_ENTRY(                 VMSVGAState, u32MaxHeight),
    SSMFIELD_ENTRY(                 VMSVGAState, u32ActionFlags),
    SSMFIELD_ENTRY(                 VMSVGAState, f3DEnabled),
    SSMFIELD_ENTRY(                 VMSVGAState, fVRAMTracking),
    SSMFIELD_ENTRY_IGNORE(          VMSVGAState, u8FIFOExtCommand),
    SSMFIELD_ENTRY_TERM()
};

static void vmsvgaSetTraces(PVGASTATE pThis, bool fTraces);

#endif /* IN_RING3 */


#ifdef LOG_ENABLED
/**
 * Index register string name lookup
 *
 * @returns Index register string or "UNKNOWN"
 * @param   pThis       VMSVGA State
 */
static const char *vmsvgaIndexToString(PVGASTATE pThis)
{
    switch (pThis->svga.u32IndexReg)
    {
    case SVGA_REG_ID:
        return "SVGA_REG_ID";
    case SVGA_REG_ENABLE:
        return "SVGA_REG_ENABLE";
    case SVGA_REG_WIDTH:
        return "SVGA_REG_WIDTH";
    case SVGA_REG_HEIGHT:
        return "SVGA_REG_HEIGHT";
    case SVGA_REG_MAX_WIDTH:
        return "SVGA_REG_MAX_WIDTH";
    case SVGA_REG_MAX_HEIGHT:
        return "SVGA_REG_MAX_HEIGHT";
    case SVGA_REG_DEPTH:
        return "SVGA_REG_DEPTH";
    case SVGA_REG_BITS_PER_PIXEL:      /* Current bpp in the guest */
        return "SVGA_REG_BITS_PER_PIXEL";
    case SVGA_REG_HOST_BITS_PER_PIXEL: /* (Deprecated) */
        return "SVGA_REG_HOST_BITS_PER_PIXEL";
    case SVGA_REG_PSEUDOCOLOR:
        return "SVGA_REG_PSEUDOCOLOR";
    case SVGA_REG_RED_MASK:
        return "SVGA_REG_RED_MASK";
    case SVGA_REG_GREEN_MASK:
        return "SVGA_REG_GREEN_MASK";
    case SVGA_REG_BLUE_MASK:
        return "SVGA_REG_BLUE_MASK";
    case SVGA_REG_BYTES_PER_LINE:
        return "SVGA_REG_BYTES_PER_LINE";
    case SVGA_REG_VRAM_SIZE:            /* VRAM size */
        return "SVGA_REG_VRAM_SIZE";
    case SVGA_REG_FB_START:             /* Frame buffer physical address. */
        return "SVGA_REG_FB_START";
    case SVGA_REG_FB_OFFSET:            /* Offset of the frame buffer in VRAM */
        return "SVGA_REG_FB_OFFSET";
    case SVGA_REG_FB_SIZE:              /* Frame buffer size */
        return "SVGA_REG_FB_SIZE";
    case SVGA_REG_CAPABILITIES:
        return "SVGA_REG_CAPABILITIES";
    case SVGA_REG_MEM_START:           /* FIFO start */
        return "SVGA_REG_MEM_START";
    case SVGA_REG_MEM_SIZE:            /* FIFO size */
        return "SVGA_REG_MEM_SIZE";
    case SVGA_REG_CONFIG_DONE:         /* Set when memory area configured */
        return "SVGA_REG_CONFIG_DONE";
    case SVGA_REG_SYNC:                /* See "FIFO Synchronization Registers" */
        return "SVGA_REG_SYNC";
    case SVGA_REG_BUSY:                /* See "FIFO Synchronization Registers" */
        return "SVGA_REG_BUSY";
    case SVGA_REG_GUEST_ID:            /* Set guest OS identifier */
        return "SVGA_REG_GUEST_ID";
    case SVGA_REG_SCRATCH_SIZE:        /* Number of scratch registers */
        return "SVGA_REG_SCRATCH_SIZE";
    case SVGA_REG_MEM_REGS:            /* Number of FIFO registers */
        return "SVGA_REG_MEM_REGS";
    case SVGA_REG_PITCHLOCK:           /* Fixed pitch for all modes */
        return "SVGA_REG_PITCHLOCK";
    case SVGA_REG_IRQMASK:             /* Interrupt mask */
        return "SVGA_REG_IRQMASK";
    case SVGA_REG_GMR_ID:
        return "SVGA_REG_GMR_ID";
    case SVGA_REG_GMR_DESCRIPTOR:
        return "SVGA_REG_GMR_DESCRIPTOR";
    case SVGA_REG_GMR_MAX_IDS:
        return "SVGA_REG_GMR_MAX_IDS";
    case SVGA_REG_GMR_MAX_DESCRIPTOR_LENGTH:
        return "SVGA_REG_GMR_MAX_DESCRIPTOR_LENGTH";
    case SVGA_REG_TRACES:            /* Enable trace-based updates even when FIFO is on */
        return "SVGA_REG_TRACES";
    case SVGA_REG_GMRS_MAX_PAGES:    /* Maximum number of 4KB pages for all GMRs */
        return "SVGA_REG_GMRS_MAX_PAGES";
    case SVGA_REG_MEMORY_SIZE:       /* Total dedicated device memory excluding FIFO */
        return "SVGA_REG_MEMORY_SIZE";
    case SVGA_REG_TOP:               /* Must be 1 more than the last register */
        return "SVGA_REG_TOP";
    case SVGA_PALETTE_BASE:         /* Base of SVGA color map */
        return "SVGA_PALETTE_BASE";
    case SVGA_REG_CURSOR_ID:
        return "SVGA_REG_CURSOR_ID";
    case SVGA_REG_CURSOR_X:
        return "SVGA_REG_CURSOR_X";
    case SVGA_REG_CURSOR_Y:
        return "SVGA_REG_CURSOR_Y";
    case SVGA_REG_CURSOR_ON:
        return "SVGA_REG_CURSOR_ON";
    case SVGA_REG_NUM_GUEST_DISPLAYS:/* Number of guest displays in X/Y direction */
        return "SVGA_REG_NUM_GUEST_DISPLAYS";
    case SVGA_REG_DISPLAY_ID:        /* Display ID for the following display attributes */
        return "SVGA_REG_DISPLAY_ID";
    case SVGA_REG_DISPLAY_IS_PRIMARY:/* Whether this is a primary display */
        return "SVGA_REG_DISPLAY_IS_PRIMARY";
    case SVGA_REG_DISPLAY_POSITION_X:/* The display position x */
        return "SVGA_REG_DISPLAY_POSITION_X";
    case SVGA_REG_DISPLAY_POSITION_Y:/* The display position y */
        return "SVGA_REG_DISPLAY_POSITION_Y";
    case SVGA_REG_DISPLAY_WIDTH:     /* The display's width */
        return "SVGA_REG_DISPLAY_WIDTH";
    case SVGA_REG_DISPLAY_HEIGHT:    /* The display's height */
        return "SVGA_REG_DISPLAY_HEIGHT";
    case SVGA_REG_NUM_DISPLAYS:        /* (Deprecated) */
        return "SVGA_REG_NUM_DISPLAYS";

    default:
        if (pThis->svga.u32IndexReg - (uint32_t)SVGA_SCRATCH_BASE < pThis->svga.cScratchRegion)
            return "SVGA_SCRATCH_BASE reg";
        if (pThis->svga.u32IndexReg - (uint32_t)SVGA_PALETTE_BASE < (uint32_t)SVGA_NUM_PALETTE_REGS)
            return "SVGA_PALETTE_BASE reg";
        return "UNKNOWN";
    }
}

/**
 * FIFO command name lookup
 *
 * @returns FIFO command string or "UNKNOWN"
 * @param   u32Cmd      FIFO command
 */
static const char *vmsvgaFIFOCmdToString(uint32_t u32Cmd)
{
    switch (u32Cmd)
    {
    case SVGA_CMD_INVALID_CMD:
        return "SVGA_CMD_INVALID_CMD";
    case SVGA_CMD_UPDATE:
        return "SVGA_CMD_UPDATE";
    case SVGA_CMD_RECT_COPY:
        return "SVGA_CMD_RECT_COPY";
    case SVGA_CMD_DEFINE_CURSOR:
        return "SVGA_CMD_DEFINE_CURSOR";
    case SVGA_CMD_DEFINE_ALPHA_CURSOR:
        return "SVGA_CMD_DEFINE_ALPHA_CURSOR";
    case SVGA_CMD_UPDATE_VERBOSE:
        return "SVGA_CMD_UPDATE_VERBOSE";
    case SVGA_CMD_FRONT_ROP_FILL:
        return "SVGA_CMD_FRONT_ROP_FILL";
    case SVGA_CMD_FENCE:
        return "SVGA_CMD_FENCE";
    case SVGA_CMD_ESCAPE:
        return "SVGA_CMD_ESCAPE";
    case SVGA_CMD_DEFINE_SCREEN:
        return "SVGA_CMD_DEFINE_SCREEN";
    case SVGA_CMD_DESTROY_SCREEN:
        return "SVGA_CMD_DESTROY_SCREEN";
    case SVGA_CMD_DEFINE_GMRFB:
        return "SVGA_CMD_DEFINE_GMRFB";
    case SVGA_CMD_BLIT_GMRFB_TO_SCREEN:
        return "SVGA_CMD_BLIT_GMRFB_TO_SCREEN";
    case SVGA_CMD_BLIT_SCREEN_TO_GMRFB:
        return "SVGA_CMD_BLIT_SCREEN_TO_GMRFB";
    case SVGA_CMD_ANNOTATION_FILL:
        return "SVGA_CMD_ANNOTATION_FILL";
    case SVGA_CMD_ANNOTATION_COPY:
        return "SVGA_CMD_ANNOTATION_COPY";
    case SVGA_CMD_DEFINE_GMR2:
        return "SVGA_CMD_DEFINE_GMR2";
    case SVGA_CMD_REMAP_GMR2:
        return "SVGA_CMD_REMAP_GMR2";
    case SVGA_3D_CMD_SURFACE_DEFINE:
        return "SVGA_3D_CMD_SURFACE_DEFINE";
    case SVGA_3D_CMD_SURFACE_DESTROY:
        return "SVGA_3D_CMD_SURFACE_DESTROY";
    case SVGA_3D_CMD_SURFACE_COPY:
        return "SVGA_3D_CMD_SURFACE_COPY";
    case SVGA_3D_CMD_SURFACE_STRETCHBLT:
        return "SVGA_3D_CMD_SURFACE_STRETCHBLT";
    case SVGA_3D_CMD_SURFACE_DMA:
        return "SVGA_3D_CMD_SURFACE_DMA";
    case SVGA_3D_CMD_CONTEXT_DEFINE:
        return "SVGA_3D_CMD_CONTEXT_DEFINE";
    case SVGA_3D_CMD_CONTEXT_DESTROY:
        return "SVGA_3D_CMD_CONTEXT_DESTROY";
    case SVGA_3D_CMD_SETTRANSFORM:
        return "SVGA_3D_CMD_SETTRANSFORM";
    case SVGA_3D_CMD_SETZRANGE:
        return "SVGA_3D_CMD_SETZRANGE";
    case SVGA_3D_CMD_SETRENDERSTATE:
        return "SVGA_3D_CMD_SETRENDERSTATE";
    case SVGA_3D_CMD_SETRENDERTARGET:
        return "SVGA_3D_CMD_SETRENDERTARGET";
    case SVGA_3D_CMD_SETTEXTURESTATE:
        return "SVGA_3D_CMD_SETTEXTURESTATE";
    case SVGA_3D_CMD_SETMATERIAL:
        return "SVGA_3D_CMD_SETMATERIAL";
    case SVGA_3D_CMD_SETLIGHTDATA:
        return "SVGA_3D_CMD_SETLIGHTDATA";
    case SVGA_3D_CMD_SETLIGHTENABLED:
        return "SVGA_3D_CMD_SETLIGHTENABLED";
    case SVGA_3D_CMD_SETVIEWPORT:
        return "SVGA_3D_CMD_SETVIEWPORT";
    case SVGA_3D_CMD_SETCLIPPLANE:
        return "SVGA_3D_CMD_SETCLIPPLANE";
    case SVGA_3D_CMD_CLEAR:
        return "SVGA_3D_CMD_CLEAR";
    case SVGA_3D_CMD_PRESENT:
        return "SVGA_3D_CMD_PRESENT";
    case SVGA_3D_CMD_SHADER_DEFINE:
        return "SVGA_3D_CMD_SHADER_DEFINE";
    case SVGA_3D_CMD_SHADER_DESTROY:
        return "SVGA_3D_CMD_SHADER_DESTROY";
    case SVGA_3D_CMD_SET_SHADER:
        return "SVGA_3D_CMD_SET_SHADER";
    case SVGA_3D_CMD_SET_SHADER_CONST:
        return "SVGA_3D_CMD_SET_SHADER_CONST";
    case SVGA_3D_CMD_DRAW_PRIMITIVES:
        return "SVGA_3D_CMD_DRAW_PRIMITIVES";
    case SVGA_3D_CMD_SETSCISSORRECT:
        return "SVGA_3D_CMD_SETSCISSORRECT";
    case SVGA_3D_CMD_BEGIN_QUERY:
        return "SVGA_3D_CMD_BEGIN_QUERY";
    case SVGA_3D_CMD_END_QUERY:
        return "SVGA_3D_CMD_END_QUERY";
    case SVGA_3D_CMD_WAIT_FOR_QUERY:
        return "SVGA_3D_CMD_WAIT_FOR_QUERY";
    case SVGA_3D_CMD_PRESENT_READBACK:
        return "SVGA_3D_CMD_PRESENT_READBACK";
    case SVGA_3D_CMD_BLIT_SURFACE_TO_SCREEN:
        return "SVGA_3D_CMD_BLIT_SURFACE_TO_SCREEN";
    case SVGA_3D_CMD_SURFACE_DEFINE_V2:
        return "SVGA_3D_CMD_SURFACE_DEFINE_V2";
    case SVGA_3D_CMD_GENERATE_MIPMAPS:
        return "SVGA_3D_CMD_GENERATE_MIPMAPS";
    case SVGA_3D_CMD_ACTIVATE_SURFACE:
        return "SVGA_3D_CMD_ACTIVATE_SURFACE";
    case SVGA_3D_CMD_DEACTIVATE_SURFACE:
        return "SVGA_3D_CMD_DEACTIVATE_SURFACE";
    default:
        return "UNKNOWN";
    }
}
#endif

/**
 * Inform the VGA device of viewport changes (as a result of e.g. scrolling)
 *
 * @param   pInterface          Pointer to this interface.
 * @param
 * @param   uScreenId           The screen updates are for.
 * @param   x                   The upper left corner x coordinate of the new viewport rectangle
 * @param   y                   The upper left corner y coordinate of the new viewport rectangle
 * @param   cx                  The width of the new viewport rectangle
 * @param   cy                  The height of the new viewport rectangle
 * @thread  The emulation thread.
 */
DECLCALLBACK(void) vmsvgaPortSetViewPort(PPDMIDISPLAYPORT pInterface, uint32_t uScreenId, uint32_t x, uint32_t y, uint32_t cx, uint32_t cy)
{
    PVGASTATE pThis = RT_FROM_MEMBER(pInterface, VGASTATE, IPort);

    Log(("vmsvgaPortSetViewPort: screen %d (%d,%d)(%d,%d)\n", uScreenId, x, y, cx, cy));

    pThis->svga.viewport.x  = x;
    pThis->svga.viewport.y  = y;
    pThis->svga.viewport.cx = RT_MIN(cx, (uint32_t)pThis->svga.uWidth);
    pThis->svga.viewport.cy = RT_MIN(cy, (uint32_t)pThis->svga.uHeight);
    return;
}

/**
 * Read port register
 *
 * @returns VBox status code.
 * @param   pThis       VMSVGA State
 * @param   pu32        Where to store the read value
 */
PDMBOTHCBDECL(int) vmsvgaReadPort(PVGASTATE pThis, uint32_t *pu32)
{
    int rc = VINF_SUCCESS;

    *pu32 = 0;
    switch (pThis->svga.u32IndexReg)
    {
    case SVGA_REG_ID:
        *pu32 = pThis->svga.u32SVGAId;
        break;

    case SVGA_REG_ENABLE:
        *pu32 = pThis->svga.fEnabled;
        break;

    case SVGA_REG_WIDTH:
    {
        if (    pThis->svga.fEnabled
            &&  pThis->svga.uWidth != VMSVGA_VAL_UNINITIALIZED)
        {
            *pu32 = pThis->svga.uWidth;
        }
        else
        {
#ifndef IN_RING3
            rc = VINF_IOM_R3_IOPORT_READ;
#else
            *pu32 = pThis->pDrv->cx;
#endif
        }
        break;
    }

    case SVGA_REG_HEIGHT:
    {
        if (    pThis->svga.fEnabled
            &&  pThis->svga.uHeight != VMSVGA_VAL_UNINITIALIZED)
        {
            *pu32 = pThis->svga.uHeight;
        }
        else
        {
#ifndef IN_RING3
            rc = VINF_IOM_R3_IOPORT_READ;
#else
            *pu32 = pThis->pDrv->cy;
#endif
        }
        break;
    }

    case SVGA_REG_MAX_WIDTH:
        *pu32 = pThis->svga.u32MaxWidth;
        break;

    case SVGA_REG_MAX_HEIGHT:
        *pu32 = pThis->svga.u32MaxHeight;
        break;

    case SVGA_REG_DEPTH:
        /* This returns the color depth of the current mode. */
        switch (pThis->svga.uBpp)
        {
        case 15:
        case 16:
        case 24:
            *pu32 = pThis->svga.uBpp;
            break;

        default:
        case 32:
            *pu32 = 24; /* The upper 8 bits are either alpha bits or not used. */
            break;
        }
        break;

    case SVGA_REG_HOST_BITS_PER_PIXEL: /* (Deprecated) */
        if (    pThis->svga.fEnabled
            &&  pThis->svga.uBpp != VMSVGA_VAL_UNINITIALIZED)
        {
            *pu32 = pThis->svga.uBpp;
        }
        else
        {
#ifndef IN_RING3
            rc = VINF_IOM_R3_IOPORT_READ;
#else
            *pu32 = pThis->pDrv->cBits;
#endif
        }
        break;

    case SVGA_REG_BITS_PER_PIXEL:      /* Current bpp in the guest */
        if (    pThis->svga.fEnabled
            &&  pThis->svga.uBpp != VMSVGA_VAL_UNINITIALIZED)
        {
            *pu32 = (pThis->svga.uBpp + 7) & ~7;
        }
        else
        {
#ifndef IN_RING3
            rc = VINF_IOM_R3_IOPORT_READ;
#else
            *pu32 = (pThis->pDrv->cBits + 7) & ~7;
#endif
        }
        break;

    case SVGA_REG_PSEUDOCOLOR:
        *pu32 = 0;
        break;

    case SVGA_REG_RED_MASK:
    case SVGA_REG_GREEN_MASK:
    case SVGA_REG_BLUE_MASK:
    {
        uint32_t uBpp;

        if (    pThis->svga.fEnabled
            &&  pThis->svga.uBpp != VMSVGA_VAL_UNINITIALIZED)
        {
            uBpp = pThis->svga.uBpp;
        }
        else
        {
#ifndef IN_RING3
            rc = VINF_IOM_R3_IOPORT_READ;
            break;
#else
            uBpp = pThis->pDrv->cBits;
#endif
        }
        uint32_t u32RedMask, u32GreenMask, u32BlueMask;
        switch (uBpp)
        {
        case 8:
            u32RedMask   = 0x07;
            u32GreenMask = 0x38;
            u32BlueMask  = 0xc0;
            break;

        case 15:
            u32RedMask   = 0x0000001f;
            u32GreenMask = 0x000003e0;
            u32BlueMask  = 0x00007c00;
            break;

        case 16:
            u32RedMask   = 0x0000001f;
            u32GreenMask = 0x000007e0;
            u32BlueMask  = 0x0000f800;
            break;

        case 24:
        case 32:
        default:
            u32RedMask   = 0x00ff0000;
            u32GreenMask = 0x0000ff00;
            u32BlueMask  = 0x000000ff;
            break;
        }
        switch (pThis->svga.u32IndexReg)
        {
        case SVGA_REG_RED_MASK:
            *pu32 = u32RedMask;
            break;

        case SVGA_REG_GREEN_MASK:
            *pu32 = u32GreenMask;
            break;

        case SVGA_REG_BLUE_MASK:
            *pu32 = u32BlueMask;
            break;
        }
        break;
    }

    case SVGA_REG_BYTES_PER_LINE:
    {
        if (    pThis->svga.fEnabled
            &&  pThis->svga.cbScanline)
        {
            *pu32 = pThis->svga.cbScanline;
        }
        else
        {
#ifndef IN_RING3
            rc = VINF_IOM_R3_IOPORT_READ;
#else
            *pu32 = pThis->pDrv->cbScanline;
#endif
        }
        break;
    }

    case SVGA_REG_VRAM_SIZE:            /* VRAM size */
        *pu32 = pThis->vram_size;
        break;

    case SVGA_REG_FB_START:             /* Frame buffer physical address. */
        Assert(pThis->GCPhysVRAM <= 0xffffffff);
        *pu32 = pThis->GCPhysVRAM;
        break;

    case SVGA_REG_FB_OFFSET:            /* Offset of the frame buffer in VRAM */
        /* Always zero in our case. */
        *pu32 = 0;
        break;

    case SVGA_REG_FB_SIZE:              /* Frame buffer size */
    {
#ifndef IN_RING3
        rc = VINF_IOM_R3_IOPORT_READ;
#else
        /* VMWare testcases want at least 4 MB in case the hardware is disabled. */
        if (    pThis->svga.fEnabled
            &&  pThis->svga.uHeight != VMSVGA_VAL_UNINITIALIZED)
        {
            /* Hardware enabled; return real framebuffer size .*/
            *pu32 = (uint32_t)pThis->svga.uHeight * pThis->svga.cbScanline;
        }
        else
            *pu32 = RT_MAX(0x100000, (uint32_t)pThis->pDrv->cy * pThis->pDrv->cbScanline);

        *pu32 = RT_MIN(pThis->vram_size, *pu32);
        Log(("h=%d w=%d bpp=%d\n", pThis->pDrv->cy, pThis->pDrv->cx, pThis->pDrv->cBits));
#endif
        break;
    }

    case SVGA_REG_CAPABILITIES:
        *pu32 = pThis->svga.u32RegCaps;
        break;

    case SVGA_REG_MEM_START:           /* FIFO start */
        Assert(pThis->svga.GCPhysFIFO <= 0xffffffff);
        *pu32 = pThis->svga.GCPhysFIFO;
        break;

    case SVGA_REG_MEM_SIZE:            /* FIFO size */
        *pu32 = pThis->svga.cbFIFO;
        break;

    case SVGA_REG_CONFIG_DONE:         /* Set when memory area configured */
        *pu32 = pThis->svga.fConfigured;
        break;

    case SVGA_REG_SYNC:                /* See "FIFO Synchronization Registers" */
        *pu32 = 0;
        break;

    case SVGA_REG_BUSY:                /* See "FIFO Synchronization Registers" */
        if (pThis->svga.fBusy)
        {
#ifndef IN_RING3
            /* Go to ring-3 and halt the CPU. */
            rc = VINF_IOM_R3_IOPORT_READ;
            break;
#elif defined(VMSVGA_USE_EMT_HALT_CODE)
            /* The guest is basically doing a HLT via the device here, but with
               a special wake up condition on FIFO completion. */
            PVMSVGASTATE pSVGAState = (PVMSVGASTATE)pThis->svga.pSVGAState;
            STAM_REL_PROFILE_START(&pSVGAState->StatBusyDelayEmts, EmtDelay);
            PVM         pVM   = PDMDevHlpGetVM(pThis->pDevInsR3);
            VMCPUID     idCpu = PDMDevHlpGetCurrentCpuId(pThis->pDevInsR3);
            VMCPUSET_ATOMIC_ADD(&pSVGAState->BusyDelayedEmts, idCpu);
            ASMAtomicIncU32(&pSVGAState->cBusyDelayedEmts);
            if (pThis->svga.fBusy)
                rc = VMR3WaitForDeviceReady(pVM, idCpu);
            ASMAtomicDecU32(&pSVGAState->cBusyDelayedEmts);
            VMCPUSET_ATOMIC_DEL(&pSVGAState->BusyDelayedEmts, idCpu);
#else

            /* Delay the EMT a bit so the FIFO and others can get some work done.
               This used to be a crude 50 ms sleep. The current code tries to be
               more efficient, but the consept is still very crude. */
            PVMSVGASTATE pSVGAState = (PVMSVGASTATE)pThis->svga.pSVGAState;
            STAM_REL_PROFILE_START(&pSVGAState->StatBusyDelayEmts, EmtDelay);
            RTThreadYield();
            if (pThis->svga.fBusy)
            {
                uint32_t cRefs = ASMAtomicIncU32(&pSVGAState->cBusyDelayedEmts);

                if (pThis->svga.fBusy && cRefs == 1)
                    RTSemEventMultiReset(pSVGAState->hBusyDelayedEmts);
                if (pThis->svga.fBusy)
                {
                    /** @todo If this code is going to stay, we need to call into the halt/wait
                     *        code in VMEmt.cpp here, otherwise all kind of EMT interaction will
                     *        suffer when the guest is polling on a busy FIFO. */
                    uint64_t cNsMaxWait = TMVirtualSyncGetNsToDeadline(PDMDevHlpGetVM(pThis->pDevInsR3));
                    if (cNsMaxWait >= RT_NS_100US)
                        RTSemEventMultiWaitEx(pSVGAState->hBusyDelayedEmts,
                                              RTSEMWAIT_FLAGS_NANOSECS | RTSEMWAIT_FLAGS_RELATIVE | RTSEMWAIT_FLAGS_NORESUME,
                                              RT_MIN(cNsMaxWait, RT_NS_10MS));
                }

                ASMAtomicDecU32(&pSVGAState->cBusyDelayedEmts);
            }
            STAM_REL_PROFILE_STOP(&pSVGAState->StatBusyDelayEmts, EmtDelay);
#endif
            *pu32 = pThis->svga.fBusy != 0;
        }
        else
            *pu32 = false;
        break;

    case SVGA_REG_GUEST_ID:            /* Set guest OS identifier */
        *pu32 = pThis->svga.u32GuestId;
        break;

    case SVGA_REG_SCRATCH_SIZE:        /* Number of scratch registers */
        *pu32 = pThis->svga.cScratchRegion;
        break;

    case SVGA_REG_MEM_REGS:            /* Number of FIFO registers */
        *pu32 = SVGA_FIFO_NUM_REGS;
        break;

    case SVGA_REG_PITCHLOCK:           /* Fixed pitch for all modes */
        *pu32 = pThis->svga.u32PitchLock;
        break;

    case SVGA_REG_IRQMASK:             /* Interrupt mask */
        *pu32 = pThis->svga.u32IrqMask;
        break;

    /* See "Guest memory regions" below. */
    case SVGA_REG_GMR_ID:
        *pu32 = pThis->svga.u32CurrentGMRId;
        break;

    case SVGA_REG_GMR_DESCRIPTOR:
        /* Write only */
        *pu32 = 0;
        break;

    case SVGA_REG_GMR_MAX_IDS:
        *pu32 = VMSVGA_MAX_GMR_IDS;
        break;

    case SVGA_REG_GMR_MAX_DESCRIPTOR_LENGTH:
        *pu32 = VMSVGA_MAX_GMR_PAGES;
        break;

    case SVGA_REG_TRACES:            /* Enable trace-based updates even when FIFO is on */
        *pu32 = pThis->svga.fTraces;
        break;

    case SVGA_REG_GMRS_MAX_PAGES:    /* Maximum number of 4KB pages for all GMRs */
        *pu32 = VMSVGA_MAX_GMR_PAGES;
        break;

    case SVGA_REG_MEMORY_SIZE:       /* Total dedicated device memory excluding FIFO */
        *pu32 = VMSVGA_SURFACE_SIZE;
        break;

    case SVGA_REG_TOP:               /* Must be 1 more than the last register */
        break;

    case SVGA_PALETTE_BASE:         /* Base of SVGA color map */
        break;
        /* Next 768 (== 256*3) registers exist for colormap */

    /* Mouse cursor support. */
    case SVGA_REG_CURSOR_ID:
    case SVGA_REG_CURSOR_X:
    case SVGA_REG_CURSOR_Y:
    case SVGA_REG_CURSOR_ON:
        break;

    /* Legacy multi-monitor support */
    case SVGA_REG_NUM_GUEST_DISPLAYS:/* Number of guest displays in X/Y direction */
        *pu32 = 1;
        break;

    case SVGA_REG_DISPLAY_ID:        /* Display ID for the following display attributes */
    case SVGA_REG_DISPLAY_IS_PRIMARY:/* Whether this is a primary display */
    case SVGA_REG_DISPLAY_POSITION_X:/* The display position x */
    case SVGA_REG_DISPLAY_POSITION_Y:/* The display position y */
        *pu32 = 0;
        break;

    case SVGA_REG_DISPLAY_WIDTH:     /* The display's width */
        *pu32 = pThis->svga.uWidth;
        break;

    case SVGA_REG_DISPLAY_HEIGHT:    /* The display's height */
        *pu32 = pThis->svga.uHeight;
        break;

    case SVGA_REG_NUM_DISPLAYS:        /* (Deprecated) */
        *pu32 = 1;  /* Must return something sensible here otherwise the Linux driver will take a legacy code path without 3d support. */
        break;

    default:
        if (    pThis->svga.u32IndexReg >= SVGA_SCRATCH_BASE
            &&  pThis->svga.u32IndexReg < SVGA_SCRATCH_BASE + pThis->svga.cScratchRegion)
        {
            *pu32 = pThis->svga.au32ScratchRegion[pThis->svga.u32IndexReg - SVGA_SCRATCH_BASE];
        }
        break;
    }
    Log(("vmsvgaReadPort index=%s (%d) val=%#x rc=%x\n", vmsvgaIndexToString(pThis), pThis->svga.u32IndexReg, *pu32, rc));
    return rc;
}

#ifdef IN_RING3
/**
 * Apply the current resolution settings to change the video mode.
 *
 * @returns VBox status code.
 * @param   pThis       VMSVGA State
 */
int vmsvgaChangeMode(PVGASTATE pThis)
{
    int rc;

    if (    pThis->svga.uWidth  == VMSVGA_VAL_UNINITIALIZED
        ||  pThis->svga.uHeight == VMSVGA_VAL_UNINITIALIZED
        ||  pThis->svga.uBpp    == VMSVGA_VAL_UNINITIALIZED)
    {
        /* Mode change in progress; wait for all values to be set. */
        Log(("vmsvgaChangeMode: BOGUS sEnable LFB mode and resize to (%d,%d) bpp=%d\n", pThis->svga.uWidth, pThis->svga.uHeight, pThis->svga.uBpp));
        return VINF_SUCCESS;
    }

    if (    pThis->svga.uWidth == 0
        ||  pThis->svga.uHeight == 0
        ||  pThis->svga.uBpp == 0)
    {
        /* Invalid mode change. */
        Log(("vmsvgaChangeMode: BOGUS sEnable LFB mode and resize to (%d,%d) bpp=%d\n", pThis->svga.uWidth, pThis->svga.uHeight, pThis->svga.uBpp));
        return VINF_SUCCESS;
    }

    if (    pThis->last_bpp         == (unsigned)pThis->svga.uBpp
        &&  pThis->last_scr_width   == (unsigned)pThis->svga.uWidth
        &&  pThis->last_scr_height  == (unsigned)pThis->svga.uHeight
        &&  pThis->last_width       == (unsigned)pThis->svga.uWidth
        &&  pThis->last_height      == (unsigned)pThis->svga.uHeight
        )
    {
        /* Nothing to do. */
        Log(("vmsvgaChangeMode: nothing changed; ignore\n"));
        return VINF_SUCCESS;
    }

    Log(("vmsvgaChangeMode: sEnable LFB mode and resize to (%d,%d) bpp=%d\n", pThis->svga.uWidth, pThis->svga.uHeight, pThis->svga.uBpp));
    pThis->svga.cbScanline = ((pThis->svga.uWidth * pThis->svga.uBpp + 7) & ~7) / 8;

    pThis->pDrv->pfnLFBModeChange(pThis->pDrv, true);
    rc = pThis->pDrv->pfnResize(pThis->pDrv, pThis->svga.uBpp, pThis->CTX_SUFF(vram_ptr), pThis->svga.cbScanline, pThis->svga.uWidth, pThis->svga.uHeight);
    AssertRC(rc);
    AssertReturn(rc == VINF_SUCCESS || rc == VINF_VGA_RESIZE_IN_PROGRESS, rc);

    /* last stuff */
    pThis->last_bpp         = pThis->svga.uBpp;
    pThis->last_scr_width   = pThis->svga.uWidth;
    pThis->last_scr_height  = pThis->svga.uHeight;
    pThis->last_width       = pThis->svga.uWidth;
    pThis->last_height      = pThis->svga.uHeight;

    ASMAtomicOrU32(&pThis->svga.u32ActionFlags, VMSVGA_ACTION_CHANGEMODE);

    /* vmsvgaPortSetViewPort not called after state load; set sensible defaults. */
    if (    pThis->svga.viewport.cx == 0
        &&  pThis->svga.viewport.cy == 0)
    {
        pThis->svga.viewport.cx = pThis->svga.uWidth;
        pThis->svga.viewport.cy = pThis->svga.uHeight;
    }
    return VINF_SUCCESS;
}
#endif /* IN_RING3 */

#if defined(IN_RING0) || defined(IN_RING3)
/**
 * Safely updates the SVGA_FIFO_BUSY register (in shared memory).
 *
 * @param   pThis               The VMSVGA state.
 * @param   fState              The busy state.
 */
DECLINLINE(void) vmsvgaSafeFifoBusyRegUpdate(PVGASTATE pThis, bool fState)
{
    ASMAtomicWriteU32(&pThis->svga.CTX_SUFF(pFIFO)[SVGA_FIFO_BUSY], fState);

    if (RT_UNLIKELY(fState != (pThis->svga.fBusy != 0)))
    {
        /* Race / unfortunately scheduling. Highly unlikly. */
        uint32_t cLoops = 64;
        do
        {
            ASMNopPause();
            fState = (pThis->svga.fBusy != 0);
            ASMAtomicWriteU32(&pThis->svga.CTX_SUFF(pFIFO)[SVGA_FIFO_BUSY], fState != 0);
        } while (cLoops-- > 0 && fState != (pThis->svga.fBusy != 0));
    }
}
#endif

/**
 * Write port register
 *
 * @returns VBox status code.
 * @param   pThis       VMSVGA State
 * @param   u32         Value to write
 */
PDMBOTHCBDECL(int) vmsvgaWritePort(PVGASTATE pThis, uint32_t u32)
{
    PVMSVGASTATE pSVGAState = (PVMSVGASTATE)pThis->svga.pSVGAState;
    int          rc = VINF_SUCCESS;

    Log(("vmsvgaWritePort index=%s (%d) val=%#x\n", vmsvgaIndexToString(pThis), pThis->svga.u32IndexReg, u32));
    switch (pThis->svga.u32IndexReg)
    {
    case SVGA_REG_ID:
        if (    u32 == SVGA_ID_0
            ||  u32 == SVGA_ID_1
            ||  u32 == SVGA_ID_2)
            pThis->svga.u32SVGAId = u32;
        break;

    case SVGA_REG_ENABLE:
        if (    pThis->svga.fEnabled    == u32
            &&  pThis->last_bpp         == (unsigned)pThis->svga.uBpp
            &&  pThis->last_scr_width   == (unsigned)pThis->svga.uWidth
            &&  pThis->last_scr_height  == (unsigned)pThis->svga.uHeight
            &&  pThis->last_width       == (unsigned)pThis->svga.uWidth
            &&  pThis->last_height      == (unsigned)pThis->svga.uHeight
            )
            /* Nothing to do. */
            break;

#ifdef IN_RING3
        if (    u32 == 1
            &&  pThis->svga.fEnabled == false)
        {
            /* Make a backup copy of the first 32k in order to save font data etc. */
            memcpy(pThis->svga.pFrameBufferBackup, pThis->vram_ptrR3, VMSVGA_FRAMEBUFFER_BACKUP_SIZE);
        }

        pThis->svga.fEnabled = u32;
        if (pThis->svga.fEnabled)
        {
            if (    pThis->svga.uWidth  == VMSVGA_VAL_UNINITIALIZED
                &&  pThis->svga.uHeight == VMSVGA_VAL_UNINITIALIZED
                &&  pThis->svga.uBpp    == VMSVGA_VAL_UNINITIALIZED)
            {
                /* Keep the current mode. */
                pThis->svga.uWidth  = pThis->pDrv->cx;
                pThis->svga.uHeight = pThis->pDrv->cy;
                pThis->svga.uBpp    = (pThis->pDrv->cBits + 7) & ~7;
            }

            if (    pThis->svga.uWidth  != VMSVGA_VAL_UNINITIALIZED
                &&  pThis->svga.uHeight != VMSVGA_VAL_UNINITIALIZED
                &&  pThis->svga.uBpp    != VMSVGA_VAL_UNINITIALIZED)
            {
                rc = vmsvgaChangeMode(pThis);
                AssertRCReturn(rc, rc);
            }
            Log(("configured=%d busy=%d\n", pThis->svga.fConfigured, pThis->svga.pFIFOR3[SVGA_FIFO_BUSY]));
            uint32_t *pFIFO = pThis->svga.pFIFOR3;
            Log(("next %x stop %x\n", pFIFO[SVGA_FIFO_NEXT_CMD], pFIFO[SVGA_FIFO_STOP]));

            /* Disable or enable dirty page tracking according to the current fTraces value. */
            vmsvgaSetTraces(pThis, !!pThis->svga.fTraces);
        }
        else
        {
            /* Restore the text mode backup. */
            memcpy(pThis->vram_ptrR3, pThis->svga.pFrameBufferBackup, VMSVGA_FRAMEBUFFER_BACKUP_SIZE);

/*            pThis->svga.uHeight    = -1;
            pThis->svga.uWidth     = -1;
            pThis->svga.uBpp       = -1;
            pThis->svga.cbScanline = 0; */
            pThis->pDrv->pfnLFBModeChange(pThis->pDrv, false);

            /* Enable dirty page tracking again when going into legacy mode. */
            vmsvgaSetTraces(pThis, true);
        }
#else
        rc = VINF_IOM_R3_IOPORT_WRITE;
#endif
        break;

    case SVGA_REG_WIDTH:
        if (pThis->svga.uWidth != u32)
        {
            if (pThis->svga.fEnabled)
            {
#ifdef IN_RING3
                pThis->svga.uWidth = u32;
                rc = vmsvgaChangeMode(pThis);
                AssertRCReturn(rc, rc);
#else
                rc = VINF_IOM_R3_IOPORT_WRITE;
#endif
            }
            else
                pThis->svga.uWidth = u32;
        }
        /* else: nop */
        break;

    case SVGA_REG_HEIGHT:
        if (pThis->svga.uHeight != u32)
        {
            if (pThis->svga.fEnabled)
            {
#ifdef IN_RING3
                pThis->svga.uHeight = u32;
                rc = vmsvgaChangeMode(pThis);
                AssertRCReturn(rc, rc);
#else
                rc = VINF_IOM_R3_IOPORT_WRITE;
#endif
            }
            else
                pThis->svga.uHeight = u32;
        }
        /* else: nop */
        break;

    case SVGA_REG_DEPTH:
        /** @todo read-only?? */
        break;

    case SVGA_REG_BITS_PER_PIXEL:      /* Current bpp in the guest */
        if (pThis->svga.uBpp != u32)
        {
            if (pThis->svga.fEnabled)
            {
#ifdef IN_RING3
                pThis->svga.uBpp = u32;
                rc = vmsvgaChangeMode(pThis);
                AssertRCReturn(rc, rc);
#else
                rc = VINF_IOM_R3_IOPORT_WRITE;
#endif
            }
            else
                pThis->svga.uBpp = u32;
        }
        /* else: nop */
        break;

    case SVGA_REG_PSEUDOCOLOR:
        break;

    case SVGA_REG_CONFIG_DONE:         /* Set when memory area configured */
#ifdef IN_RING3
        pThis->svga.fConfigured = u32;
        /* Disabling the FIFO enables tracing (dirty page detection) by default. */
        if (!pThis->svga.fConfigured)
        {
            pThis->svga.fTraces = true;
        }
        vmsvgaSetTraces(pThis, !!pThis->svga.fTraces);
#else
        rc = VINF_IOM_R3_IOPORT_WRITE;
#endif
        break;

    case SVGA_REG_SYNC:                /* See "FIFO Synchronization Registers" */
        if (    pThis->svga.fEnabled
            &&  pThis->svga.fConfigured)
        {
#if defined(IN_RING3) || defined(IN_RING0)
            Log(("SVGA_REG_SYNC: SVGA_FIFO_BUSY=%d\n", pThis->svga.CTX_SUFF(pFIFO)[SVGA_FIFO_BUSY]));
            ASMAtomicWriteU32(&pThis->svga.fBusy, VMSVGA_BUSY_F_EMT_FORCE | VMSVGA_BUSY_F_FIFO);
            if (VMSVGA_IS_VALID_FIFO_REG(SVGA_FIFO_BUSY, pThis->svga.CTX_SUFF(pFIFO)[SVGA_FIFO_MIN]))
                vmsvgaSafeFifoBusyRegUpdate(pThis, true);

            /* Kick the FIFO thread to start processing commands again. */
            SUPSemEventSignal(pThis->svga.pSupDrvSession, pThis->svga.FIFORequestSem);
#else
            rc = VINF_IOM_R3_IOPORT_WRITE;
#endif
        }
        /* else nothing to do. */
        else
            Log(("Sync ignored enabled=%d configured=%d\n", pThis->svga.fEnabled, pThis->svga.fConfigured));

        break;

    case SVGA_REG_BUSY:                /* See "FIFO Synchronization Registers" (read-only) */
        break;

    case SVGA_REG_GUEST_ID:            /* Set guest OS identifier */
        pThis->svga.u32GuestId = u32;
        break;

    case SVGA_REG_PITCHLOCK:           /* Fixed pitch for all modes */
        pThis->svga.u32PitchLock = u32;
        break;

    case SVGA_REG_IRQMASK:             /* Interrupt mask */
        pThis->svga.u32IrqMask = u32;

        /* Irq pending after the above change? */
        if (pThis->svga.u32IrqStatus & u32)
        {
            Log(("SVGA_REG_IRQMASK: Trigger interrupt with status %x\n", pThis->svga.u32IrqStatus));
            PDMDevHlpPCISetIrqNoWait(pThis->CTX_SUFF(pDevIns), 0, 1);
        }
        else
            PDMDevHlpPCISetIrqNoWait(pThis->CTX_SUFF(pDevIns), 0, 0);
        break;

    /* Mouse cursor support */
    case SVGA_REG_CURSOR_ID:
    case SVGA_REG_CURSOR_X:
    case SVGA_REG_CURSOR_Y:
    case SVGA_REG_CURSOR_ON:
        break;

    /* Legacy multi-monitor support */
    case SVGA_REG_NUM_GUEST_DISPLAYS:/* Number of guest displays in X/Y direction */
        break;
    case SVGA_REG_DISPLAY_ID:        /* Display ID for the following display attributes */
        break;
    case SVGA_REG_DISPLAY_IS_PRIMARY:/* Whether this is a primary display */
        break;
    case SVGA_REG_DISPLAY_POSITION_X:/* The display position x */
        break;
    case SVGA_REG_DISPLAY_POSITION_Y:/* The display position y */
        break;
    case SVGA_REG_DISPLAY_WIDTH:     /* The display's width */
        break;
    case SVGA_REG_DISPLAY_HEIGHT:    /* The display's height */
        break;
#ifdef VBOX_WITH_VMSVGA3D
    /* See "Guest memory regions" below. */
    case SVGA_REG_GMR_ID:
        pThis->svga.u32CurrentGMRId = u32;
        break;

    case SVGA_REG_GMR_DESCRIPTOR:
# ifndef IN_RING3
        rc = VINF_IOM_R3_IOPORT_WRITE;
        break;
# else /* IN_RING3 */
    {
        SVGAGuestMemDescriptor desc;
        RTGCPHYS               GCPhys = (RTGCPHYS)u32 << PAGE_SHIFT;
        RTGCPHYS               GCPhysBase = GCPhys;
        uint32_t               idGMR = pThis->svga.u32CurrentGMRId;
        uint32_t               cDescriptorsAllocated = 16;
        uint32_t               iDescriptor = 0;

        /* Validate current GMR id. */
        AssertBreak(idGMR < VMSVGA_MAX_GMR_IDS);

        /* Free the old GMR if present. */
        vmsvgaGMRFree(pThis, idGMR);

        /* Just undefine the GMR? */
        if (GCPhys == 0)
            break;

        pSVGAState->aGMR[idGMR].paDesc = (PVMSVGAGMRDESCRIPTOR)RTMemAllocZ(cDescriptorsAllocated * sizeof(VMSVGAGMRDESCRIPTOR));
        AssertReturn(pSVGAState->aGMR[idGMR].paDesc, VERR_NO_MEMORY);

        /* Never cross a page boundary automatically. */
        while (PHYS_PAGE_ADDRESS(GCPhys) == PHYS_PAGE_ADDRESS(GCPhysBase))
        {
            /* Read descriptor. */
            rc = PDMDevHlpPhysRead(pThis->CTX_SUFF(pDevIns), GCPhys, &desc, sizeof(desc));
            AssertRCBreak(rc);

            if (    desc.ppn == 0
                &&  desc.numPages == 0)
                break;  /* terminator */

            if (    desc.ppn != 0
                &&  desc.numPages == 0)
            {
                /* Pointer to the next physical page of descriptors. */
                GCPhys = GCPhysBase = desc.ppn << PAGE_SHIFT;
            }
            else
            {
                if (iDescriptor == cDescriptorsAllocated)
                {
                    cDescriptorsAllocated += 16;
                    pSVGAState->aGMR[idGMR].paDesc = (PVMSVGAGMRDESCRIPTOR)RTMemRealloc(pSVGAState->aGMR[idGMR].paDesc, cDescriptorsAllocated * sizeof(VMSVGAGMRDESCRIPTOR));
                    AssertReturn(pSVGAState->aGMR[idGMR].paDesc, VERR_NO_MEMORY);
                }

                pSVGAState->aGMR[idGMR].paDesc[iDescriptor].GCPhys     = desc.ppn << PAGE_SHIFT;
                pSVGAState->aGMR[idGMR].paDesc[iDescriptor++].numPages = desc.numPages;
                pSVGAState->aGMR[idGMR].cbTotal += desc.numPages * PAGE_SIZE;

                /* Continue with the next descriptor. */
                GCPhys += sizeof(desc);
            }
        }
        pSVGAState->aGMR[idGMR].numDescriptors = iDescriptor;
        Log(("Defined new gmr %x numDescriptors=%d cbTotal=%x\n", idGMR, iDescriptor, pSVGAState->aGMR[idGMR].cbTotal));

        if (!pSVGAState->aGMR[idGMR].numDescriptors)
        {
            AssertFailed();
            RTMemFree(pSVGAState->aGMR[idGMR].paDesc);
            pSVGAState->aGMR[idGMR].paDesc = NULL;
        }
        AssertRC(rc);
        break;
    }
# endif /* IN_RING3 */
#endif // VBOX_WITH_VMSVGA3D

    case SVGA_REG_TRACES:            /* Enable trace-based updates even when FIFO is on */
        if (pThis->svga.fTraces == u32)
            break; /* nothing to do */

#ifdef IN_RING3
        vmsvgaSetTraces(pThis, !!u32);
#else
        rc = VINF_IOM_R3_IOPORT_WRITE;
#endif
        break;

    case SVGA_REG_TOP:               /* Must be 1 more than the last register */
        break;

    case SVGA_PALETTE_BASE:         /* Base of SVGA color map */
        break;
        /* Next 768 (== 256*3) registers exist for colormap */

    case SVGA_REG_NUM_DISPLAYS:        /* (Deprecated) */
        Log(("Write to deprecated register %x - val %x ignored\n", pThis->svga.u32IndexReg, u32));
        break;

    case SVGA_REG_FB_START:
    case SVGA_REG_MEM_START:
    case SVGA_REG_HOST_BITS_PER_PIXEL:
    case SVGA_REG_MAX_WIDTH:
    case SVGA_REG_MAX_HEIGHT:
    case SVGA_REG_VRAM_SIZE:
    case SVGA_REG_FB_SIZE:
    case SVGA_REG_CAPABILITIES:
    case SVGA_REG_MEM_SIZE:
    case SVGA_REG_SCRATCH_SIZE:        /* Number of scratch registers */
    case SVGA_REG_MEM_REGS:            /* Number of FIFO registers */
    case SVGA_REG_BYTES_PER_LINE:
    case SVGA_REG_FB_OFFSET:
    case SVGA_REG_RED_MASK:
    case SVGA_REG_GREEN_MASK:
    case SVGA_REG_BLUE_MASK:
    case SVGA_REG_GMRS_MAX_PAGES:    /* Maximum number of 4KB pages for all GMRs */
    case SVGA_REG_MEMORY_SIZE:       /* Total dedicated device memory excluding FIFO */
    case SVGA_REG_GMR_MAX_IDS:
    case SVGA_REG_GMR_MAX_DESCRIPTOR_LENGTH:
        /* Read only - ignore. */
        Log(("Write to R/O register %x - val %x ignored\n", pThis->svga.u32IndexReg, u32));
        break;

    default:
        if (    pThis->svga.u32IndexReg >= SVGA_SCRATCH_BASE
            &&  pThis->svga.u32IndexReg < SVGA_SCRATCH_BASE + pThis->svga.cScratchRegion)
        {
            pThis->svga.au32ScratchRegion[pThis->svga.u32IndexReg - SVGA_SCRATCH_BASE] = u32;
        }
        break;
    }
    return rc;
}

/**
 * Port I/O Handler for IN operations.
 *
 * @returns VINF_SUCCESS or VINF_EM_*.
 * @returns VERR_IOM_IOPORT_UNUSED if the port is really unused and a ~0 value should be returned.
 *
 * @param   pDevIns     The device instance.
 * @param   pvUser      User argument.
 * @param   uPort       Port number used for the IN operation.
 * @param   pu32        Where to store the result.  This is always a 32-bit
 *                      variable regardless of what @a cb might say.
 * @param   cb          Number of bytes read.
 */
PDMBOTHCBDECL(int) vmsvgaIORead(PPDMDEVINS pDevIns, void *pvUser, RTIOPORT Port, uint32_t *pu32, unsigned cb)
{
    PVGASTATE   pThis = PDMINS_2_DATA(pDevIns, PVGASTATE);
    int rc = VINF_SUCCESS;

    /* Ignore non-dword accesses. */
    if (cb != 4)
    {
        Log(("Ignoring non-dword read at %x cb=%d\n", Port, cb));
        *pu32 = ~0;
        return VINF_SUCCESS;
    }

    switch (Port - pThis->svga.BasePort)
    {
    case SVGA_INDEX_PORT:
        *pu32 = pThis->svga.u32IndexReg;
        break;

    case SVGA_VALUE_PORT:
        return vmsvgaReadPort(pThis, pu32);

    case SVGA_BIOS_PORT:
        Log(("Ignoring BIOS port read\n"));
        *pu32 = 0;
        break;

    case SVGA_IRQSTATUS_PORT:
        LogFlow(("vmsvgaIORead: SVGA_IRQSTATUS_PORT %x\n", pThis->svga.u32IrqStatus));
        *pu32 = pThis->svga.u32IrqStatus;
        break;
    }
    return rc;
}

/**
 * Port I/O Handler for OUT operations.
 *
 * @returns VINF_SUCCESS or VINF_EM_*.
 *
 * @param   pDevIns     The device instance.
 * @param   pvUser      User argument.
 * @param   uPort       Port number used for the OUT operation.
 * @param   u32         The value to output.
 * @param   cb          The value size in bytes.
 */
PDMBOTHCBDECL(int) vmsvgaIOWrite(PPDMDEVINS pDevIns, void *pvUser, RTIOPORT Port, uint32_t u32, unsigned cb)
{
    PVGASTATE   pThis = PDMINS_2_DATA(pDevIns, PVGASTATE);
    int rc = VINF_SUCCESS;

    /* Ignore non-dword accesses. */
    if (cb != 4)
    {
        Log(("Ignoring non-dword write at %x val=%x cb=%d\n", Port, u32, cb));
        return VINF_SUCCESS;
    }

    switch (Port - pThis->svga.BasePort)
    {
    case SVGA_INDEX_PORT:
        pThis->svga.u32IndexReg = u32;
        break;

    case SVGA_VALUE_PORT:
        return vmsvgaWritePort(pThis, u32);

    case SVGA_BIOS_PORT:
        Log(("Ignoring BIOS port write (val=%x)\n", u32));
        break;

    case SVGA_IRQSTATUS_PORT:
        Log(("vmsvgaIOWrite SVGA_IRQSTATUS_PORT %x: status %x -> %x\n", u32, pThis->svga.u32IrqStatus, pThis->svga.u32IrqStatus & ~u32));
        ASMAtomicAndU32(&pThis->svga.u32IrqStatus, ~u32);
        /* Clear the irq in case all events have been cleared. */
        if (!(pThis->svga.u32IrqStatus & pThis->svga.u32IrqMask))
        {
            Log(("vmsvgaIOWrite SVGA_IRQSTATUS_PORT: clearing IRQ\n"));
            PDMDevHlpPCISetIrqNoWait(pDevIns, 0, 0);
        }
        break;
    }
    return rc;
}

#ifdef DEBUG_FIFO_ACCESS

# ifdef IN_RING3
/**
 * Handle LFB access.
 * @returns VBox status code.
 * @param   pVM             VM handle.
 * @param   pThis           VGA device instance data.
 * @param   GCPhys          The access physical address.
 * @param   fWriteAccess    Read or write access
 */
static int vmsvgaFIFOAccess(PVM pVM, PVGASTATE pThis, RTGCPHYS GCPhys, bool fWriteAccess)
{
    RTGCPHYS GCPhysOffset = GCPhys - pThis->svga.GCPhysFIFO;
    uint32_t *pFIFO = pThis->svga.pFIFOR3;

    switch (GCPhysOffset >> 2)
    {
    case SVGA_FIFO_MIN:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_MIN = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_MAX:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_MAX = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_NEXT_CMD:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_NEXT_CMD = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_STOP:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_STOP = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_CAPABILITIES:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_CAPABILITIES = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_FLAGS:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_FLAGS = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_FENCE:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_FENCE = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_HWVERSION:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_HWVERSION = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_PITCHLOCK:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_PITCHLOCK = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_CURSOR_ON:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_CURSOR_ON = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_CURSOR_X:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_CURSOR_X = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_CURSOR_Y:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_CURSOR_Y = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_CURSOR_COUNT:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_CURSOR_COUNT = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_CURSOR_LAST_UPDATED:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_CURSOR_LAST_UPDATED = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_RESERVED:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_RESERVED = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_CURSOR_SCREEN_ID:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_CURSOR_SCREEN_ID = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_DEAD:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_DEAD = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_HWVERSION_REVISED:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_HWVERSION_REVISED = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_3D:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_3D = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_MAX_LIGHTS:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_MAX_LIGHTS = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_MAX_TEXTURES:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_MAX_TEXTURES = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_MAX_CLIP_PLANES:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_MAX_CLIP_PLANES = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_VERTEX_SHADER_VERSION:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_VERTEX_SHADER_VERSION = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_VERTEX_SHADER:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_VERTEX_SHADER = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_FRAGMENT_SHADER_VERSION:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_FRAGMENT_SHADER_VERSION = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_FRAGMENT_SHADER:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_FRAGMENT_SHADER = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_MAX_RENDER_TARGETS:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_MAX_RENDER_TARGETS = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_S23E8_TEXTURES:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_S23E8_TEXTURES = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_S10E5_TEXTURES:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_S10E5_TEXTURES = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_MAX_FIXED_VERTEXBLEND:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_MAX_FIXED_VERTEXBLEND = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_D16_BUFFER_FORMAT:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_D16_BUFFER_FORMAT = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_D24S8_BUFFER_FORMAT:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_D24S8_BUFFER_FORMAT = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_D24X8_BUFFER_FORMAT:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_D24X8_BUFFER_FORMAT = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_QUERY_TYPES:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_QUERY_TYPES = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_TEXTURE_GRADIENT_SAMPLING:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_TEXTURE_GRADIENT_SAMPLING = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_MAX_POINT_SIZE:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_MAX_POINT_SIZE = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_MAX_SHADER_TEXTURES:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_MAX_SHADER_TEXTURES = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_MAX_TEXTURE_WIDTH:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_MAX_TEXTURE_WIDTH = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_MAX_TEXTURE_HEIGHT:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_MAX_TEXTURE_HEIGHT = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_MAX_VOLUME_EXTENT:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_MAX_VOLUME_EXTENT = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_MAX_TEXTURE_REPEAT:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_MAX_TEXTURE_REPEAT = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_MAX_TEXTURE_ASPECT_RATIO:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_MAX_TEXTURE_ASPECT_RATIO = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_MAX_TEXTURE_ANISOTROPY:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_MAX_TEXTURE_ANISOTROPY = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_MAX_PRIMITIVE_COUNT:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_MAX_PRIMITIVE_COUNT = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_MAX_VERTEX_INDEX:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_MAX_VERTEX_INDEX = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_MAX_VERTEX_SHADER_INSTRUCTIONS:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_MAX_VERTEX_SHADER_INSTRUCTIONS = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_MAX_FRAGMENT_SHADER_INSTRUCTIONS:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_MAX_FRAGMENT_SHADER_INSTRUCTIONS = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_MAX_VERTEX_SHADER_TEMPS:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_MAX_VERTEX_SHADER_TEMPS = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_MAX_FRAGMENT_SHADER_TEMPS:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_MAX_FRAGMENT_SHADER_TEMPS = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_TEXTURE_OPS:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_TEXTURE_OPS = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_X8R8G8B8:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_X8R8G8B8 = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_A8R8G8B8:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_A8R8G8B8 = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_A2R10G10B10:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_A2R10G10B10 = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_X1R5G5B5:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_X1R5G5B5 = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_A1R5G5B5:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_A1R5G5B5 = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_A4R4G4B4:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_A4R4G4B4 = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_R5G6B5:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_R5G6B5 = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_LUMINANCE16:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_LUMINANCE16 = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_LUMINANCE8_ALPHA8:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_LUMINANCE8_ALPHA8 = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_ALPHA8:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_ALPHA8 = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_LUMINANCE8:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_LUMINANCE8 = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_Z_D16:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_Z_D16 = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_Z_D24S8:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_Z_D24S8 = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_Z_D24X8:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_Z_D24X8 = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_DXT1:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_DXT1 = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_DXT2:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_DXT2 = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_DXT3:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_DXT3 = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_DXT4:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_DXT4 = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_DXT5:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_DXT5 = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_BUMPX8L8V8U8:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_BUMPX8L8V8U8 = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_A2W10V10U10:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_A2W10V10U10 = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_BUMPU8V8:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_BUMPU8V8 = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_Q8W8V8U8:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_Q8W8V8U8 = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_CxV8U8:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_CxV8U8 = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_R_S10E5:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_R_S10E5 = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_R_S23E8:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_R_S23E8 = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_RG_S10E5:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_RG_S10E5 = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_RG_S23E8:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_RG_S23E8 = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_ARGB_S10E5:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_ARGB_S10E5 = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_ARGB_S23E8:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_ARGB_S23E8 = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_MAX_VERTEX_SHADER_TEXTURES:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_MAX_VERTEX_SHADER_TEXTURES = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_MAX_SIMULTANEOUS_RENDER_TARGETS:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_MAX_SIMULTANEOUS_RENDER_TARGETS = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_V16U16:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_V16U16 = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_G16R16:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_G16R16 = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_A16B16G16R16:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_A16B16G16R16 = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_UYVY:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_UYVY = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_YUY2:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_YUY2 = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_MULTISAMPLE_NONMASKABLESAMPLES:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_MULTISAMPLE_NONMASKABLESAMPLES = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_MULTISAMPLE_MASKABLESAMPLES:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_MULTISAMPLE_MASKABLESAMPLES = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_ALPHATOCOVERAGE:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_ALPHATOCOVERAGE = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SUPERSAMPLE:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SUPERSAMPLE = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_AUTOGENMIPMAPS:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_AUTOGENMIPMAPS = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_NV12:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_NV12 = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_AYUV:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_AYUV = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_MAX_CONTEXT_IDS:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_MAX_CONTEXT_IDS = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_MAX_SURFACE_IDS:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_MAX_SURFACE_IDS = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_Z_DF16:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_Z_DF16 = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_Z_DF24:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_Z_DF24 = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_Z_D24S8_INT:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_Z_D24S8_INT = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_BC4_UNORM:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_BC4_UNORM = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS + SVGA3D_DEVCAP_SURFACEFMT_BC5_UNORM:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS SVGA3D_DEVCAP_SURFACEFMT_BC5_UNORM = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_3D_CAPS_LAST:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_3D_CAPS_LAST = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_GUEST_3D_HWVERSION:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_GUEST_3D_HWVERSION = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_FENCE_GOAL:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_FENCE_GOAL = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    case SVGA_FIFO_BUSY:
        Log(("vmsvgaFIFOAccess [0x%x]: %s SVGA_FIFO_BUSY = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", pFIFO[GCPhysOffset >> 2]));
        break;
    default:
        Log(("vmsvgaFIFOAccess [0x%x]: %s access at offset %x = %x\n", GCPhysOffset >> 2, (fWriteAccess) ? "WRITE" : "READ", GCPhysOffset, pFIFO[GCPhysOffset >> 2]));
        break;
    }

    return VINF_EM_RAW_EMULATE_INSTR;
}

/**
 * HC access handler for the FIFO.
 *
 * @returns VINF_SUCCESS if the handler have carried out the operation.
 * @returns VINF_PGM_HANDLER_DO_DEFAULT if the caller should carry out the access operation.
 * @param   pVM             VM Handle.
 * @param   pVCpu           The cross context CPU structure for the calling EMT.
 * @param   GCPhys          The physical address the guest is writing to.
 * @param   pvPhys          The HC mapping of that address.
 * @param   pvBuf           What the guest is reading/writing.
 * @param   cbBuf           How much it's reading/writing.
 * @param   enmAccessType   The access type.
 * @param   enmOrigin       Who is making the access.
 * @param   pvUser          User argument.
 */
static DECLCALLBACK(VBOXSTRICTRC)
vmsvgaR3FIFOAccessHandler(PVM pVM, PVMCPU pVCpu RTGCPHYS GCPhys, void *pvPhys, void *pvBuf, size_t cbBuf,
                          PGMACCESSTYPE enmAccessType, PGMACCESSORIGIN enmOrigin, void *pvUser)
{
    PVGASTATE   pThis = (PVGASTATE)pvUser;
    int         rc;
    Assert(pThis);
    Assert(GCPhys >= pThis->GCPhysVRAM);
    NOREF(pVCpu); NOREF(pvPhys); NOREF(pvBuf); NOREF(cbBuf); NOREF(enmOrigin);

    rc = vmsvgaFIFOAccess(pVM, pThis, GCPhys, enmAccessType == PGMACCESSTYPE_WRITE);
    if (RT_SUCCESS(rc))
        return VINF_PGM_HANDLER_DO_DEFAULT;
    AssertMsg(rc <= VINF_SUCCESS, ("rc=%Rrc\n", rc));
    return rc;
}

# endif /* IN_RING3 */
#endif /* DEBUG_FIFO_ACCESS */

#ifdef DEBUG_GMR_ACCESS
/**
 * HC access handler for the FIFO.
 *
 * @returns VINF_SUCCESS if the handler have carried out the operation.
 * @returns VINF_PGM_HANDLER_DO_DEFAULT if the caller should carry out the access operation.
 * @param   pVM             VM Handle.
 * @param   pVCpu           The cross context CPU structure for the calling EMT.
 * @param   GCPhys          The physical address the guest is writing to.
 * @param   pvPhys          The HC mapping of that address.
 * @param   pvBuf           What the guest is reading/writing.
 * @param   cbBuf           How much it's reading/writing.
 * @param   enmAccessType   The access type.
 * @param   enmOrigin       Who is making the access.
 * @param   pvUser          User argument.
 */
static DECLCALLBACK(VBOXSTRICTRC)
vmsvgaR3GMRAccessHandler(PVM pVM, PVMCPU pVCpu, RTGCPHYS GCPhys, void *pvPhys, void *pvBuf, size_t cbBuf,
                         PGMACCESSTYPE enmAccessType, PGMACCESSORIGIN enmOrigin, void *pvUser)
{
    PVGASTATE   pThis = (PVGASTATE)pvUser;
    Assert(pThis);
    PVMSVGASTATE pSVGAState = (PVMSVGASTATE)pThis->svga.pSVGAState;
    NOREF(pVCpu); NOREF(pvPhys); NOREF(pvBuf); NOREF(cbBuf); NOREF(enmOrigin);

    Log(("vmsvgaR3GMRAccessHandler: GMR access to page %RGp\n", GCPhys));

    for (uint32_t i = 0; i < RT_ELEMENTS(pSVGAState->aGMR); i++)
    {
        PGMR pGMR = &pSVGAState->aGMR[i];

        if (pGMR->numDescriptors)
        {
            for (uint32_t j = 0; j < pGMR->numDescriptors; j++)
            {
                if (    GCPhys >= pGMR->paDesc[j].GCPhys
                    &&  GCPhys < pGMR->paDesc[j].GCPhys + pGMR->paDesc[j].numPages * PAGE_SIZE)
                {
                    /*
                     * Turn off the write handler for this particular page and make it R/W.
                     * Then return telling the caller to restart the guest instruction.
                     */
                    int rc = PGMHandlerPhysicalPageTempOff(pVM, pGMR->paDesc[j].GCPhys, GCPhys);
                    goto end;
                }
            }
        }
    }
end:
    return VINF_PGM_HANDLER_DO_DEFAULT;
}

# ifdef IN_RING3

/* Callback handler for VMR3ReqCallWait */
static DECLCALLBACK(int) vmsvgaRegisterGMR(PPDMDEVINS pDevIns, uint32_t gmrId)
{
    PVGASTATE    pThis = PDMINS_2_DATA(pDevIns, PVGASTATE);
    PVMSVGASTATE pSVGAState = (PVMSVGASTATE)pThis->svga.pSVGAState;
    PGMR pGMR = &pSVGAState->aGMR[gmrId];
    int rc;

    for (uint32_t i = 0; i < pGMR->numDescriptors; i++)
    {
        rc = PGMHandlerPhysicalRegister(PDMDevHlpGetVM(pThis->pDevInsR3),
                                        pGMR->paDesc[i].GCPhys, pGMR->paDesc[i].GCPhys + pGMR->paDesc[i].numPages * PAGE_SIZE - 1,
                                        pThis->svga.hGmrAccessHandlerType, pThis, NIL_RTR0PTR, NIL_RTRCPTR, "VMSVGA GMR");
        AssertRC(rc);
    }
    return VINF_SUCCESS;
}

/* Callback handler for VMR3ReqCallWait */
static DECLCALLBACK(int) vmsvgaDeregisterGMR(PPDMDEVINS pDevIns, uint32_t gmrId)
{
    PVGASTATE    pThis = PDMINS_2_DATA(pDevIns, PVGASTATE);
    PVMSVGASTATE pSVGAState = (PVMSVGASTATE)pThis->svga.pSVGAState;
    PGMR pGMR = &pSVGAState->aGMR[gmrId];

    for (uint32_t i = 0; i < pGMR->numDescriptors; i++)
    {
        int rc = PGMHandlerPhysicalDeregister(PDMDevHlpGetVM(pThis->pDevInsR3), pGMR->paDesc[i].GCPhys);
        AssertRC(rc);
    }
    return VINF_SUCCESS;
}

/* Callback handler for VMR3ReqCallWait */
static DECLCALLBACK(int) vmsvgaResetGMRHandlers(PVGASTATE pThis)
{
    PVMSVGASTATE pSVGAState = (PVMSVGASTATE)pThis->svga.pSVGAState;

    for (uint32_t i = 0; i < RT_ELEMENTS(pSVGAState->aGMR); i++)
    {
        PGMR pGMR = &pSVGAState->aGMR[i];

        if (pGMR->numDescriptors)
        {
            for (uint32_t j = 0; j < pGMR->numDescriptors; j++)
            {
                int rc = PGMHandlerPhysicalReset(PDMDevHlpGetVM(pThis->pDevInsR3), pGMR->paDesc[j].GCPhys);
                AssertRC(rc);
            }
        }
    }
    return VINF_SUCCESS;
}

# endif /* IN_RING3 */
#endif /* DEBUG_GMR_ACCESS */

/* -=-=-=-=-=- Ring 3 -=-=-=-=-=- */

#ifdef IN_RING3

/**
 * Marks the FIFO non-busy, notifying any waiting EMTs.
 *
 * @param   pThis           The VGA state.
 * @param   pSVGAState      Pointer to the ring-3 only SVGA state data.
 * @param   offFifoMin      The start byte offset of the command FIFO.
 */
static void vmsvgaFifoSetNotBusy(PVGASTATE pThis, PVMSVGASTATE pSVGAState, uint32_t offFifoMin)
{
    ASMAtomicAndU32(&pThis->svga.fBusy, ~VMSVGA_BUSY_F_FIFO);
    if (VMSVGA_IS_VALID_FIFO_REG(SVGA_FIFO_BUSY, offFifoMin))
        vmsvgaSafeFifoBusyRegUpdate(pThis, pThis->svga.fBusy != 0);

    /* Wake up any waiting EMTs. */
    if (pSVGAState->cBusyDelayedEmts > 0)
    {
#ifdef VMSVGA_USE_EMT_HALT_CODE
        PVM pVM = PDMDevHlpGetVM(pThis->pDevInsR3);
        VMCPUID idCpu = VMCpuSetFindLastPresentInternal(&pSVGAState->BusyDelayedEmts);
        if (idCpu != NIL_VMCPUID)
        {
            VMR3NotifyCpuDeviceReady(pVM, idCpu);
            while (idCpu-- > 0)
                if (VMCPUSET_IS_PRESENT(&pSVGAState->BusyDelayedEmts, idCpu))
                    VMR3NotifyCpuDeviceReady(pVM, idCpu);
        }
#else
        int rc2 = RTSemEventMultiSignal(pSVGAState->hBusyDelayedEmts);
        AssertRC(rc2);
#endif
    }
}

/**
 * Reads (more) payload into the command buffer.
 *
 * @returns pbBounceBuf on success
 * @retval  (void *)1 if the thread was requested to stop.
 * @retval  NULL on FIFO error.
 *
 * @param   cbPayloadReq    The number of bytes of payload requested.
 * @param   pFIFO           The FIFO.
 * @param   offCurrentCmd   The FIFO byte offset of the current command.
 * @param   offFifoMin      The start byte offset of the command FIFO.
 * @param   offFifoMax      The end byte offset of the command FIFO.
 * @param   pbBounceBuf     The bounch buffer. Same size as the entire FIFO, so
 *                          always sufficient size.
 * @param   pcbAlreadyRead  How much payload we've already read into the bounce
 *                          buffer. (We will NEVER re-read anything.)
 * @param   pThread         The calling PDM thread handle.
 * @param   pThis           The VGA state.
 * @param   pSVGAState      Pointer to the ring-3 only SVGA state data. For
 *                          statistics collection.
 */
static void *vmsvgaFIFOGetCmdPayload(uint32_t cbPayloadReq, uint32_t volatile *pFIFO,
                                     uint32_t offCurrentCmd, uint32_t offFifoMin, uint32_t offFifoMax,
                                     uint8_t *pbBounceBuf, uint32_t *pcbAlreadyRead,
                                     PPDMTHREAD pThread, PVGASTATE pThis, PVMSVGASTATE pSVGAState)
{
    Assert(pbBounceBuf);
    Assert(pcbAlreadyRead);
    Assert(offFifoMin < offFifoMax);
    Assert(offCurrentCmd >= offFifoMin && offCurrentCmd < offFifoMax);
    Assert(offFifoMax <= VMSVGA_FIFO_SIZE);

    /*
     * Check if the requested payload size has already been satisfied                                                                                                .
     *                                                                                                                                                       .
     * When called to read more, the caller is responsible for making sure the                                                                               .
     * new command size (cbRequsted) never is smaller than what has already                                                                                  .
     * been read.
     */
    uint32_t cbAlreadyRead = *pcbAlreadyRead;
    if (cbPayloadReq <= cbAlreadyRead)
    {
        AssertLogRelReturn(cbPayloadReq == cbAlreadyRead, NULL);
        return pbBounceBuf;
    }

    /*
     * Commands bigger than the fifo buffer are invalid.
     */
    uint32_t const cbFifoCmd = offFifoMax - offFifoMin;
    AssertMsgReturnStmt(cbPayloadReq <= cbFifoCmd, ("cbPayloadReq=%#x cbFifoCmd=%#x\n", cbPayloadReq, cbFifoCmd),
                        STAM_REL_COUNTER_INC(&pSVGAState->StatFifoErrors),
                        NULL);

    /*
     * Move offCurrentCmd past the command dword.
     */
    offCurrentCmd += sizeof(uint32_t);
    if (offCurrentCmd >= offFifoMax)
        offCurrentCmd = offFifoMin;

    /*
     * Do we have sufficient payload data available already?
     */
    uint32_t cbAfter, cbBefore;
    uint32_t offNextCmd = pFIFO[SVGA_FIFO_NEXT_CMD];
    if (offNextCmd > offCurrentCmd)
    {
        if (RT_LIKELY(offNextCmd < offFifoMax))
            cbAfter = offNextCmd - offCurrentCmd;
        else
        {
            STAM_REL_COUNTER_INC(&pSVGAState->StatFifoErrors);
            LogRelMax(16, ("vmsvgaFIFOGetCmdPayload: Invalid offNextCmd=%#x (offFifoMin=%#x offFifoMax=%#x)\n",
                           offNextCmd, offFifoMin, offFifoMax));
            cbAfter = offFifoMax - offCurrentCmd;
        }
        cbBefore = 0;
    }
    else
    {
        cbAfter  = offFifoMax - offCurrentCmd;
        if (offNextCmd >= offFifoMin)
            cbBefore = offNextCmd - offFifoMin;
        else
        {
            STAM_REL_COUNTER_INC(&pSVGAState->StatFifoErrors);
            LogRelMax(16, ("vmsvgaFIFOGetCmdPayload: Invalid offNextCmd=%#x (offFifoMin=%#x offFifoMax=%#x)\n",
                           offNextCmd, offFifoMin, offFifoMax));
            cbBefore = 0;
        }
    }
    if (cbAfter + cbBefore < cbPayloadReq)
    {
        /*
         * Insufficient, must wait for it to arrive.
         */
        STAM_REL_PROFILE_START(&pSVGAState->StatFifoStalls, Stall);
        for (uint32_t i = 0;; i++)
        {
            if (pThread->enmState != PDMTHREADSTATE_RUNNING)
            {
                STAM_REL_PROFILE_STOP(&pSVGAState->StatFifoStalls, Stall);
                return (void *)(uintptr_t)1;
            }
            Log(("Guest still copying (%x vs %x) current %x next %x stop %x loop %u; sleep a bit\n",
                 cbPayloadReq, cbAfter + cbBefore, offCurrentCmd, offNextCmd, pFIFO[SVGA_FIFO_STOP], i));

            SUPSemEventWaitNoResume(pThis->svga.pSupDrvSession, pThis->svga.FIFORequestSem, i < 16 ? 1 : 2);

            offNextCmd = pFIFO[SVGA_FIFO_NEXT_CMD];
            if (offNextCmd > offCurrentCmd)
            {
                cbAfter = RT_MIN(offNextCmd, offFifoMax) - offCurrentCmd;
                cbBefore = 0;
            }
            else
            {
                cbAfter  = offFifoMax - offCurrentCmd;
                cbBefore = RT_MAX(offNextCmd, offFifoMin) - offFifoMin;
            }

            if (cbAfter + cbBefore >= cbPayloadReq)
                break;
        }
        STAM_REL_PROFILE_STOP(&pSVGAState->StatFifoStalls, Stall);
    }

    /*
     * Copy out the memory and update what pcbAlreadyRead points to.
     */
    if (cbAfter >= cbPayloadReq)
        memcpy(pbBounceBuf + cbAlreadyRead,
               (uint8_t *)pFIFO + offCurrentCmd + cbAlreadyRead,
               cbPayloadReq - cbAlreadyRead);
    else
    {
        LogFlow(("Split data buffer at %x (%u-%u)\n", offCurrentCmd, cbAfter, cbBefore));
        if (cbAlreadyRead < cbAfter)
        {
            memcpy(pbBounceBuf + cbAlreadyRead,
                   (uint8_t *)pFIFO + offCurrentCmd + cbAlreadyRead,
                   cbAfter - cbAlreadyRead);
            cbAlreadyRead = cbAfter;
        }
        memcpy(pbBounceBuf + cbAlreadyRead,
               (uint8_t *)pFIFO + offFifoMin + cbAlreadyRead - cbAfter,
               cbPayloadReq - cbAlreadyRead);
    }
    *pcbAlreadyRead = cbPayloadReq;
    return pbBounceBuf;
}

/* The async FIFO handling thread. */
static DECLCALLBACK(int) vmsvgaFIFOLoop(PPDMDEVINS pDevIns, PPDMTHREAD pThread)
{
    PVGASTATE    pThis = (PVGASTATE)pThread->pvUser;
    PVMSVGASTATE pSVGAState = (PVMSVGASTATE)pThis->svga.pSVGAState;
    int          rc;

    if (pThread->enmState == PDMTHREADSTATE_INITIALIZING)
        return VINF_SUCCESS;

    /*
     * Signal the semaphore to make sure we don't wait for 250 after a
     * suspend & resume scenario (see vmsvgaFIFOGetCmdPayload).
     */
    SUPSemEventSignal(pThis->svga.pSupDrvSession, pThis->svga.FIFORequestSem);

    /*
     * Allocate a bounce buffer for command we get from the FIFO.
     * (All code must return via the end of the function to free this buffer.)
     */
    uint8_t *pbBounceBuf = (uint8_t *)RTMemAllocZ(VMSVGA_FIFO_SIZE);
    AssertReturn(pbBounceBuf, VERR_NO_MEMORY);

    LogFlow(("vmsvgaFIFOLoop: started loop\n"));
    uint32_t volatile * const pFIFO = pThis->svga.pFIFOR3;
    while (pThread->enmState == PDMTHREADSTATE_RUNNING)
    {
# if defined(RT_OS_DARWIN) && defined(VBOX_WITH_VMSVGA3D)
        /*
         * Should service the run loop every so often.
         */
        if (pThis->svga.f3DEnabled)
            vmsvga3dCocoaServiceRunLoop();
# endif

        /*
         * Wait for at most 250 ms to start polling.
         */
        rc = SUPSemEventWaitNoResume(pThis->svga.pSupDrvSession, pThis->svga.FIFORequestSem, 250);
        AssertBreak(RT_SUCCESS(rc) || rc == VERR_TIMEOUT || rc == VERR_INTERRUPTED);
        if (pThread->enmState != PDMTHREADSTATE_RUNNING)
        {
            LogFlow(("vmsvgaFIFOLoop: thread state %x\n", pThread->enmState));
            break;
        }
        if (rc == VERR_TIMEOUT)
        {
            if (pFIFO[SVGA_FIFO_NEXT_CMD] == pFIFO[SVGA_FIFO_STOP])
                continue;
            STAM_REL_COUNTER_INC(&pSVGAState->StatFifoTodoTimeout);

            Log(("vmsvgaFIFOLoop: timeout\n"));
        }
        else if (pFIFO[SVGA_FIFO_NEXT_CMD] != pFIFO[SVGA_FIFO_STOP])
            STAM_REL_COUNTER_INC(&pSVGAState->StatFifoTodoWoken);

        Log(("vmsvgaFIFOLoop: enabled=%d configured=%d busy=%d\n", pThis->svga.fEnabled, pThis->svga.fConfigured, pThis->svga.pFIFOR3[SVGA_FIFO_BUSY]));
        Log(("vmsvgaFIFOLoop: min  %x max  %x\n", pFIFO[SVGA_FIFO_MIN], pFIFO[SVGA_FIFO_MAX]));
        Log(("vmsvgaFIFOLoop: next %x stop %x\n", pFIFO[SVGA_FIFO_NEXT_CMD], pFIFO[SVGA_FIFO_STOP]));

        /*
         * Handle external commands.
         */
        if (pThis->svga.u8FIFOExtCommand != VMSVGA_FIFO_EXTCMD_NONE)
        {
            switch (pThis->svga.u8FIFOExtCommand)
            {
            case VMSVGA_FIFO_EXTCMD_RESET:
                Log(("vmsvgaFIFOLoop: reset the fifo thread.\n"));
# ifdef VBOX_WITH_VMSVGA3D
                if (pThis->svga.f3DEnabled)
                {
                    /* The 3d subsystem must be reset from the fifo thread. */
                    vmsvga3dReset(pThis);
                }
# endif
                break;

            case VMSVGA_FIFO_EXTCMD_TERMINATE:
                Log(("vmsvgaFIFOLoop: terminate the fifo thread.\n"));
# ifdef VBOX_WITH_VMSVGA3D
                if (pThis->svga.f3DEnabled)
                {
                    /* The 3d subsystem must be shut down from the fifo thread. */
                    vmsvga3dTerminate(pThis);
                }
# endif
                break;

            case VMSVGA_FIFO_EXTCMD_SAVESTATE:
                Log(("vmsvgaFIFOLoop: VMSVGA_FIFO_EXTCMD_SAVESTATE.\n"));
# ifdef VBOX_WITH_VMSVGA3D
                vmsvga3dSaveExec(pThis, (PSSMHANDLE)pThis->svga.pFIFOExtCmdParam);
# endif
                break;

            case VMSVGA_FIFO_EXTCMD_LOADSTATE:
            {
                Log(("vmsvgaFIFOLoop: VMSVGA_FIFO_EXTCMD_LOADSTATE.\n"));
# ifdef VBOX_WITH_VMSVGA3D
                PVMSVGA_STATE_LOAD pLoadState = (PVMSVGA_STATE_LOAD)pThis->svga.pFIFOExtCmdParam;
                vmsvga3dLoadExec(pThis, pLoadState->pSSM, pLoadState->uVersion, pLoadState->uPass);
# endif
                break;
            }
            }

            pThis->svga.u8FIFOExtCommand = VMSVGA_FIFO_EXTCMD_NONE;

            /* Signal the end of the external command. */
            RTSemEventSignal(pThis->svga.FIFOExtCmdSem);
            continue;
        }

        if (    !pThis->svga.fEnabled
            ||  !pThis->svga.fConfigured)
        {
            vmsvgaFifoSetNotBusy(pThis, pSVGAState, pFIFO[SVGA_FIFO_MIN]);
            continue;   /* device not enabled. */
        }

        /*
         * Get and check the min/max values.  We ASSUME that they will remain
         * unchanged while we process requests.  A further ASSUMPTION is that
         * the guest won't mess with SVGA_FIFO_NEXT_CMD while we're busy, so
         * we don't read it back while in the loop.
         */
        uint32_t const offFifoMin    = pFIFO[SVGA_FIFO_MIN];
        uint32_t const offFifoMax    = pFIFO[SVGA_FIFO_MAX];
        uint32_t       offCurrentCmd = pFIFO[SVGA_FIFO_STOP];
        if (RT_UNLIKELY(   !VMSVGA_IS_VALID_FIFO_REG(SVGA_FIFO_STOP, offFifoMin)
                        || offFifoMax <= offFifoMin
                        || offFifoMax > VMSVGA_FIFO_SIZE
                        || (offFifoMax & 3) != 0
                        || (offFifoMin & 3) != 0
                        || offCurrentCmd < offFifoMin
                        || offCurrentCmd > offFifoMax))
        {
            STAM_REL_COUNTER_INC(&pSVGAState->StatFifoErrors);
            LogRelMax(8, ("vmsvgaFIFOLoop: Bad fifo: min=%#x stop=%#x max=%#x\n", offFifoMin, offCurrentCmd, offFifoMax));
            vmsvgaFifoSetNotBusy(pThis, pSVGAState, offFifoMin);
            continue;
        }
        if (RT_UNLIKELY(offCurrentCmd & 3))
        {
            STAM_REL_COUNTER_INC(&pSVGAState->StatFifoErrors);
            LogRelMax(8, ("vmsvgaFIFOLoop: Misaligned offCurrentCmd=%#x?\n", offCurrentCmd));
            offCurrentCmd = ~UINT32_C(3);
        }

/**
 * Macro for shortening calls to vmsvgaFIFOGetCmdPayload.
 *
 * Will break out of the switch on failure.
 * Will restart and quit the loop if the thread was requested to stop.
 *
 * @param   a_cbPayloadReq  How much payload to fetch.
 * @remarks Access a bunch of variables in the current scope!
 */
# define VMSVGAFIFO_GET_CMD_BUFFER_BREAK(a_PtrVar, a_Type, a_cbPayloadReq) \
            if (1) { \
                (a_PtrVar) = (a_Type *)vmsvgaFIFOGetCmdPayload((a_cbPayloadReq), pFIFO, offCurrentCmd, offFifoMin, offFifoMax, \
                                                               pbBounceBuf, &cbPayload, pThread, pThis, pSVGAState); \
                if (RT_UNLIKELY((uintptr_t)(a_PtrVar) < 2)) { if ((uintptr_t)(a_PtrVar) == 1) continue; break; } \
            } else do {} while (0)
/**
 * Macro for shortening calls to vmsvgaFIFOGetCmdPayload for refetching the
 * buffer after figuring out the actual command size.
 * Will break out of the switch on failure.
 * @param   a_cbPayloadReq  How much payload to fetch.
 * @remarks Access a bunch of variables in the current scope!
 */
# define VMSVGAFIFO_GET_MORE_CMD_BUFFER_BREAK(a_PtrVar, a_Type, a_cbPayloadReq) \
            if (1) { \
                VMSVGAFIFO_GET_CMD_BUFFER_BREAK(a_PtrVar, a_Type, a_cbPayloadReq); \
            } else do {} while (0)

        /*
         * Mark the FIFO as busy.
         */
        ASMAtomicWriteU32(&pThis->svga.fBusy, VMSVGA_BUSY_F_FIFO);
        if (VMSVGA_IS_VALID_FIFO_REG(SVGA_FIFO_BUSY, offFifoMin))
            ASMAtomicWriteU32(&pFIFO[SVGA_FIFO_BUSY], true);

        /*
         * Execute all queued FIFO commands.
         * Quit if pending external command or changes in the thread state.
         */
        bool fDone = false;
        while (   !(fDone = (pFIFO[SVGA_FIFO_NEXT_CMD] == offCurrentCmd))
               && pThread->enmState == PDMTHREADSTATE_RUNNING)
        {
            uint32_t cbPayload = 0;
            uint32_t u32IrqStatus = 0;
            bool     fTriggerIrq = false;

            Assert(offCurrentCmd < offFifoMax && offCurrentCmd >= offFifoMin);

            /* First check any pending actions. */
            if (ASMBitTestAndClear(&pThis->svga.u32ActionFlags, VMSVGA_ACTION_CHANGEMODE_BIT))
# ifdef VBOX_WITH_VMSVGA3D
                vmsvga3dChangeMode(pThis);
# else
                {/*nothing*/}
# endif
            /* Check for pending external commands. */
            if (pThis->svga.u8FIFOExtCommand != VMSVGA_FIFO_EXTCMD_NONE)
                break;

            /*
             * Process the command.
             */
            SVGAFifoCmdId const enmCmdId = (SVGAFifoCmdId)pFIFO[offCurrentCmd / sizeof(uint32_t)];
            LogFlow(("vmsvgaFIFOLoop: FIFO command (iCmd=0x%x) %s 0x%x\n",
                     offCurrentCmd / sizeof(uint32_t), vmsvgaFIFOCmdToString(enmCmdId), enmCmdId));
            switch (enmCmdId)
            {
            case SVGA_CMD_INVALID_CMD:
                /* Nothing to do. */
                break;

            case SVGA_CMD_FENCE:
            {
                SVGAFifoCmdFence *pCmdFence;
                VMSVGAFIFO_GET_CMD_BUFFER_BREAK(pCmdFence, SVGAFifoCmdFence, sizeof(*pCmdFence));
                if (VMSVGA_IS_VALID_FIFO_REG(SVGA_FIFO_FENCE, offFifoMin))
                {
                    Log(("vmsvgaFIFOLoop: SVGA_CMD_FENCE %x\n", pCmdFence->fence));
                    pFIFO[SVGA_FIFO_FENCE] = pCmdFence->fence;

                    if (pThis->svga.u32IrqMask & SVGA_IRQFLAG_ANY_FENCE)
                    {
                        Log(("vmsvgaFIFOLoop: any fence irq\n"));
                        u32IrqStatus |= SVGA_IRQFLAG_ANY_FENCE;
                    }
                    else
                    if (    VMSVGA_IS_VALID_FIFO_REG(SVGA_FIFO_FENCE_GOAL, offFifoMin)
                        &&  (pThis->svga.u32IrqMask & SVGA_IRQFLAG_FENCE_GOAL)
                        &&  pFIFO[SVGA_FIFO_FENCE_GOAL] == pCmdFence->fence)
                    {
                        Log(("vmsvgaFIFOLoop: fence goal reached irq (fence=%x)\n", pCmdFence->fence));
                        u32IrqStatus |= SVGA_IRQFLAG_FENCE_GOAL;
                    }
                }
                else
                    Log(("SVGA_CMD_FENCE is bogus when offFifoMin is %#x!\n", offFifoMin));
                break;
            }
            case SVGA_CMD_UPDATE:
            case SVGA_CMD_UPDATE_VERBOSE:
            {
                SVGAFifoCmdUpdate *pUpdate;
                VMSVGAFIFO_GET_CMD_BUFFER_BREAK(pUpdate, SVGAFifoCmdUpdate, sizeof(*pUpdate));
                Log(("vmsvgaFIFOLoop: UPDATE (%d,%d)(%d,%d)\n", pUpdate->x, pUpdate->y, pUpdate->width, pUpdate->height));
                vgaR3UpdateDisplay(pThis, pUpdate->x, pUpdate->y, pUpdate->width, pUpdate->height);
                break;
            }

            case SVGA_CMD_DEFINE_CURSOR:
            {
                /* Followed by bitmap data. */
                SVGAFifoCmdDefineCursor *pCursor;
                VMSVGAFIFO_GET_CMD_BUFFER_BREAK(pCursor, SVGAFifoCmdDefineCursor, sizeof(*pCursor));
                AssertFailed(); /** @todo implement when necessary. */
                break;
            }

            case SVGA_CMD_DEFINE_ALPHA_CURSOR:
            {
                /* Followed by bitmap data. */
                uint32_t cbCursorShape, cbAndMask;
                uint8_t *pCursorCopy;
                uint32_t cbCmd;

                SVGAFifoCmdDefineAlphaCursor *pCursor;
                VMSVGAFIFO_GET_CMD_BUFFER_BREAK(pCursor, SVGAFifoCmdDefineAlphaCursor, sizeof(*pCursor));

                Log(("vmsvgaFIFOLoop: ALPHA_CURSOR id=%d size (%d,%d) hotspot (%d,%d)\n", pCursor->id, pCursor->width, pCursor->height, pCursor->hotspotX, pCursor->hotspotY));

                /* Check against a reasonable upper limit to prevent integer overflows in the sanity checks below. */
                AssertBreak(pCursor->height < 2048 && pCursor->width < 2048);

                /* Refetch the bitmap data as well. */
                cbCmd = sizeof(SVGAFifoCmdDefineAlphaCursor) + pCursor->width * pCursor->height * sizeof(uint32_t) /* 32-bit BRGA format */;
                VMSVGAFIFO_GET_MORE_CMD_BUFFER_BREAK(pCursor, SVGAFifoCmdDefineAlphaCursor, cbCmd);
                /** @todo Would be more efficient to copy the data straight into pCursorCopy (memcpy below). */

                /* The mouse pointer interface always expects an AND mask followed by the color data (XOR mask). */
                cbAndMask     = (pCursor->width + 7) / 8 * pCursor->height;                         /* size of the AND mask */
                cbAndMask     = ((cbAndMask + 3) & ~3);                                             /* + gap for alignment */
                cbCursorShape = cbAndMask + pCursor->width * sizeof(uint32_t) * pCursor->height;    /* + size of the XOR mask (32-bit BRGA format) */

                pCursorCopy = (uint8_t *)RTMemAlloc(cbCursorShape);
                AssertBreak(pCursorCopy);

                Log2(("Cursor data:\n%.*Rhxd\n", pCursor->width * pCursor->height * sizeof(uint32_t), pCursor+1));

                /* Transparency is defined by the alpha bytes, so make the whole bitmap visible. */
                memset(pCursorCopy, 0xff, cbAndMask);
                /* Colour data */
                memcpy(pCursorCopy + cbAndMask, (pCursor + 1), pCursor->width * pCursor->height * sizeof(uint32_t));

                rc = pThis->pDrv->pfnVBVAMousePointerShape (pThis->pDrv,
                                                            true,
                                                            true,
                                                            pCursor->hotspotX,
                                                            pCursor->hotspotY,
                                                            pCursor->width,
                                                            pCursor->height,
                                                            pCursorCopy);
                AssertRC(rc);

                if (pSVGAState->Cursor.fActive)
                    RTMemFree(pSVGAState->Cursor.pData);

                pSVGAState->Cursor.fActive  = true;
                pSVGAState->Cursor.xHotspot = pCursor->hotspotX;
                pSVGAState->Cursor.yHotspot = pCursor->hotspotY;
                pSVGAState->Cursor.width    = pCursor->width;
                pSVGAState->Cursor.height   = pCursor->height;
                pSVGAState->Cursor.cbData   = cbCursorShape;
                pSVGAState->Cursor.pData    = pCursorCopy;
                break;
            }

            case SVGA_CMD_ESCAPE:
            {
                /* Followed by nsize bytes of data. */
                SVGAFifoCmdEscape *pEscape;
                VMSVGAFIFO_GET_CMD_BUFFER_BREAK(pEscape, SVGAFifoCmdEscape, sizeof(*pEscape));

                /* Refetch the command buffer with the variable data; undo size increase (ugly) */
                AssertBreak(pEscape->size < VMSVGA_FIFO_SIZE);
                uint32_t cbCmd = sizeof(SVGAFifoCmdEscape) + pEscape->size;
                VMSVGAFIFO_GET_MORE_CMD_BUFFER_BREAK(pEscape, SVGAFifoCmdEscape, cbCmd);

                if (pEscape->nsid == SVGA_ESCAPE_NSID_VMWARE)
                {
                    AssertBreak(pEscape->size >= sizeof(uint32_t));
                    uint32_t cmd = *(uint32_t *)(pEscape + 1);
                    Log(("vmsvgaFIFOLoop: ESCAPE (%x %x) VMWARE cmd=%x\n", pEscape->nsid, pEscape->size, cmd));

                    switch (cmd)
                    {
                    case SVGA_ESCAPE_VMWARE_VIDEO_SET_REGS:
                    {
                        SVGAEscapeVideoSetRegs *pVideoCmd = (SVGAEscapeVideoSetRegs *)(pEscape + 1);
                        AssertBreak(pEscape->size >= sizeof(pVideoCmd->header));
                        uint32_t cRegs = (pEscape->size - sizeof(pVideoCmd->header)) / sizeof(pVideoCmd->items[0]);

                        Log(("SVGA_ESCAPE_VMWARE_VIDEO_SET_REGS: stream %x\n", pVideoCmd->header.streamId));
                        for (uint32_t iReg = 0; iReg < cRegs; iReg++)
                        {
                            Log(("SVGA_ESCAPE_VMWARE_VIDEO_SET_REGS: reg %x val %x\n", pVideoCmd->items[iReg].registerId, pVideoCmd->items[iReg].value));
                        }
                        break;
                    }

                    case SVGA_ESCAPE_VMWARE_VIDEO_FLUSH:
                        SVGAEscapeVideoFlush *pVideoCmd = (SVGAEscapeVideoFlush *)(pEscape + 1);
                        AssertBreak(pEscape->size >= sizeof(*pVideoCmd));
                        Log(("SVGA_ESCAPE_VMWARE_VIDEO_FLUSH: stream %x\n", pVideoCmd->streamId));
                        break;
                    }
                }
                else
                    Log(("vmsvgaFIFOLoop: ESCAPE %x %x\n", pEscape->nsid, pEscape->size));

                break;
            }
# ifdef VBOX_WITH_VMSVGA3D
            case SVGA_CMD_DEFINE_GMR2:
            {
                SVGAFifoCmdDefineGMR2 *pCmd;
                VMSVGAFIFO_GET_CMD_BUFFER_BREAK(pCmd, SVGAFifoCmdDefineGMR2, sizeof(*pCmd));
                Log(("vmsvgaFIFOLoop: SVGA_CMD_DEFINE_GMR2 id=%x %x pages\n", pCmd->gmrId, pCmd->numPages));

                /* Validate current GMR id. */
                AssertBreak(pCmd->gmrId < VMSVGA_MAX_GMR_IDS);
                AssertBreak(pCmd->numPages <= VMSVGA_MAX_GMR_PAGES);

                if (!pCmd->numPages)
                {
                    vmsvgaGMRFree(pThis, pCmd->gmrId);
                }
                else
                {
                    PGMR pGMR = &pSVGAState->aGMR[pCmd->gmrId];
                    pGMR->cMaxPages = pCmd->numPages;
                }
                /* everything done in remap */
                break;
            }

            case SVGA_CMD_REMAP_GMR2:
            {
                /* Followed by page descriptors or guest ptr. */
                SVGAFifoCmdRemapGMR2 *pCmd;
                VMSVGAFIFO_GET_CMD_BUFFER_BREAK(pCmd, SVGAFifoCmdRemapGMR2, sizeof(*pCmd));
                uint32_t cbPageDesc = (pCmd->flags & SVGA_REMAP_GMR2_PPN64) ? sizeof(uint64_t) : sizeof(uint32_t);
                uint32_t cbCmd;
                uint64_t *paNewPage64 = NULL;

                Log(("vmsvgaFIFOLoop: SVGA_CMD_REMAP_GMR2 id=%x flags=%x offset=%x npages=%x\n", pCmd->gmrId, pCmd->flags, pCmd->offsetPages, pCmd->numPages));
                AssertBreak(pCmd->gmrId < VMSVGA_MAX_GMR_IDS);

                /* Calculate the size of what comes after next and fetch it. */
                cbCmd = sizeof(SVGAFifoCmdRemapGMR2);
                if (pCmd->flags & SVGA_REMAP_GMR2_VIA_GMR)
                    cbCmd += sizeof(SVGAGuestPtr);
                else
                if (pCmd->flags & SVGA_REMAP_GMR2_SINGLE_PPN)
                {
                    cbCmd         += cbPageDesc;
                    pCmd->numPages = 1;
                }
                else
                {
                    AssertBreak(pCmd->numPages <= VMSVGA_FIFO_SIZE);
                    cbCmd += cbPageDesc * pCmd->numPages;
                }
                VMSVGAFIFO_GET_MORE_CMD_BUFFER_BREAK(pCmd, SVGAFifoCmdRemapGMR2, cbCmd);

                /* Validate current GMR id. */
                AssertBreak(pCmd->gmrId < VMSVGA_MAX_GMR_IDS);
                PGMR pGMR = &pSVGAState->aGMR[pCmd->gmrId];
                AssertBreak(pCmd->offsetPages + pCmd->numPages <= pGMR->cMaxPages);
                AssertBreak(!pCmd->offsetPages || pGMR->paDesc); /** @todo */

                /* Save the old page descriptors as an array of page addresses (>> PAGE_SHIFT) */
                if (pGMR->paDesc)
                {
                    uint32_t idxPage = 0;
                    paNewPage64 = (uint64_t *)RTMemAllocZ(pGMR->cMaxPages * sizeof(uint64_t));
                    AssertBreak(paNewPage64);

                    for (uint32_t i = 0; i < pGMR->numDescriptors; i++)
                    {
                        for (uint32_t j = 0; j < pGMR->paDesc[i].numPages; j++)
                        {
                            paNewPage64[idxPage++] = (pGMR->paDesc[i].GCPhys + j * PAGE_SIZE) >> PAGE_SHIFT;
                        }
                    }
                    AssertBreak(idxPage == pGMR->cbTotal >> PAGE_SHIFT);
                }

                /* Free the old GMR if present. */
                if (pGMR->paDesc)
                    RTMemFree(pGMR->paDesc);

                /* Allocate the maximum amount possible (everything non-continuous) */
                pGMR->paDesc = (PVMSVGAGMRDESCRIPTOR)RTMemAllocZ(pGMR->cMaxPages * sizeof(VMSVGAGMRDESCRIPTOR));
                AssertBreak(pGMR->paDesc);

                if (pCmd->flags & SVGA_REMAP_GMR2_VIA_GMR)
                {
                    /** @todo */
                    AssertFailed();
                }
                else
                {
                    uint32_t            *pPage32 = (uint32_t *)(pCmd + 1);
                    uint64_t            *pPage64 = (uint64_t *)(pCmd + 1);
                    uint32_t             iDescriptor = 0;
                    RTGCPHYS             GCPhys;
                    PVMSVGAGMRDESCRIPTOR paDescOld = NULL;
                    bool                 fGCPhys64 = !!(pCmd->flags & SVGA_REMAP_GMR2_PPN64);

                    if (paNewPage64)
                    {
                        /* Overwrite the old page array with the new page values. */
                        for (uint32_t i = pCmd->offsetPages; i < pCmd->offsetPages + pCmd->numPages; i++)
                        {
                            if (pCmd->flags & SVGA_REMAP_GMR2_PPN64)
                                paNewPage64[i] = pPage64[i - pCmd->offsetPages];
                            else
                                paNewPage64[i] = pPage32[i - pCmd->offsetPages];
                        }
                        /* Use the updated page array instead of the command data. */
                        fGCPhys64      = true;
                        pPage64        = paNewPage64;
                        pCmd->numPages = pGMR->cbTotal >> PAGE_SHIFT;
                    }

                    if (fGCPhys64)
                        GCPhys = (pPage64[0] << PAGE_SHIFT) & 0x00000FFFFFFFFFFFULL;    /* seeing rubbish in the top bits with certain linux guests*/
                    else
                        GCPhys = pPage32[0] << PAGE_SHIFT;

                    pGMR->paDesc[0].GCPhys    = GCPhys;
                    pGMR->paDesc[0].numPages  = 1;
                    pGMR->cbTotal             = PAGE_SIZE;

                    for (uint32_t i = 1; i < pCmd->numPages; i++)
                    {
                        if (pCmd->flags & SVGA_REMAP_GMR2_PPN64)
                            GCPhys = (pPage64[i] << PAGE_SHIFT) & 0x00000FFFFFFFFFFFULL;    /* seeing rubbish in the top bits with certain linux guests*/
                        else
                            GCPhys = pPage32[i] << PAGE_SHIFT;

                        /* Continuous physical memory? */
                        if (GCPhys == pGMR->paDesc[iDescriptor].GCPhys + pGMR->paDesc[iDescriptor].numPages * PAGE_SIZE)
                        {
                            Assert(pGMR->paDesc[iDescriptor].numPages);
                            pGMR->paDesc[iDescriptor].numPages++;
                            LogFlow(("Page %x GCPhys=%RGp successor\n", i, GCPhys));
                        }
                        else
                        {
                            iDescriptor++;
                            pGMR->paDesc[iDescriptor].GCPhys   = GCPhys;
                            pGMR->paDesc[iDescriptor].numPages = 1;
                            LogFlow(("Page %x GCPhys=%RGp\n", i, pGMR->paDesc[iDescriptor].GCPhys));
                        }

                        pGMR->cbTotal += PAGE_SIZE;
                    }
                    LogFlow(("Nr of descriptors %x\n", iDescriptor + 1));
                    pGMR->numDescriptors = iDescriptor + 1;
                }

                if (paNewPage64)
                    RTMemFree(paNewPage64);

#  ifdef DEBUG_GMR_ACCESS
                VMR3ReqCallWait(PDMDevHlpGetVM(pThis->pDevInsR3), VMCPUID_ANY, (PFNRT)vmsvgaRegisterGMR, 2, pThis->pDevInsR3, pCmd->gmrId);
#  endif
                break;
            }
# endif // VBOX_WITH_VMSVGA3D
            case SVGA_CMD_DEFINE_SCREEN:
            {
                /* Note! The size of this command is specified by the guest and depends on capabilities. */
                Assert(!(pThis->svga.pFIFOR3[SVGA_FIFO_CAPABILITIES] & SVGA_FIFO_CAP_SCREEN_OBJECT));
                SVGAFifoCmdDefineScreen *pCmd;
                VMSVGAFIFO_GET_CMD_BUFFER_BREAK(pCmd, SVGAFifoCmdDefineScreen, sizeof(pCmd->screen.structSize));
                RT_BZERO(&pCmd->screen.id, sizeof(*pCmd) - RT_OFFSETOF(SVGAFifoCmdDefineScreen, screen.structSize));
                VMSVGAFIFO_GET_MORE_CMD_BUFFER_BREAK(pCmd, SVGAFifoCmdDefineScreen, RT_MAX(sizeof(pCmd->screen.structSize), pCmd->screen.structSize));

                Log(("vmsvgaFIFOLoop: SVGA_CMD_DEFINE_SCREEN id=%x flags=%x size=(%d,%d) root=(%d,%d)\n", pCmd->screen.id, pCmd->screen.flags, pCmd->screen.size.width, pCmd->screen.size.height, pCmd->screen.root.x, pCmd->screen.root.y));
                if (pCmd->screen.flags & SVGA_SCREEN_HAS_ROOT)
                    Log(("vmsvgaFIFOLoop: SVGA_CMD_DEFINE_SCREEN flags SVGA_SCREEN_HAS_ROOT\n"));
                if (pCmd->screen.flags & SVGA_SCREEN_IS_PRIMARY)
                    Log(("vmsvgaFIFOLoop: SVGA_CMD_DEFINE_SCREEN flags SVGA_SCREEN_IS_PRIMARY\n"));
                if (pCmd->screen.flags & SVGA_SCREEN_FULLSCREEN_HINT)
                    Log(("vmsvgaFIFOLoop: SVGA_CMD_DEFINE_SCREEN flags SVGA_SCREEN_FULLSCREEN_HINT\n"));
                if (pCmd->screen.flags & SVGA_SCREEN_DEACTIVATE )
                    Log(("vmsvgaFIFOLoop: SVGA_CMD_DEFINE_SCREEN flags SVGA_SCREEN_DEACTIVATE \n"));
                if (pCmd->screen.flags & SVGA_SCREEN_BLANKING)
                    Log(("vmsvgaFIFOLoop: SVGA_CMD_DEFINE_SCREEN flags SVGA_SCREEN_BLANKING\n"));

                /** @todo multi monitor support and screen object capabilities.   */
                pThis->svga.uWidth  = pCmd->screen.size.width;
                pThis->svga.uHeight = pCmd->screen.size.height;
                vmsvgaChangeMode(pThis);
                break;
            }

            case SVGA_CMD_DESTROY_SCREEN:
            {
                SVGAFifoCmdDestroyScreen *pCmd;
                VMSVGAFIFO_GET_CMD_BUFFER_BREAK(pCmd, SVGAFifoCmdDestroyScreen, sizeof(*pCmd));

                Log(("vmsvgaFIFOLoop: SVGA_CMD_DESTROY_SCREEN id=%x\n", pCmd->screenId));
                break;
            }
# ifdef VBOX_WITH_VMSVGA3D
            case SVGA_CMD_DEFINE_GMRFB:
            {
                SVGAFifoCmdDefineGMRFB *pCmd;
                VMSVGAFIFO_GET_CMD_BUFFER_BREAK(pCmd, SVGAFifoCmdDefineGMRFB, sizeof(*pCmd));

                Log(("vmsvgaFIFOLoop: SVGA_CMD_DEFINE_GMRFB gmr=%x offset=%x bytesPerLine=%x bpp=%d color depth=%d\n", pCmd->ptr.gmrId, pCmd->ptr.offset, pCmd->bytesPerLine, pCmd->format.s.bitsPerPixel, pCmd->format.s.colorDepth));
                pSVGAState->GMRFB.ptr          = pCmd->ptr;
                pSVGAState->GMRFB.bytesPerLine = pCmd->bytesPerLine;
                pSVGAState->GMRFB.format       = pCmd->format;
                break;
            }

            case SVGA_CMD_BLIT_GMRFB_TO_SCREEN:
            {
                uint32_t width, height;
                SVGAFifoCmdBlitGMRFBToScreen *pCmd;
                VMSVGAFIFO_GET_CMD_BUFFER_BREAK(pCmd, SVGAFifoCmdBlitGMRFBToScreen, sizeof(*pCmd));

                Log(("vmsvgaFIFOLoop: SVGA_CMD_BLIT_GMRFB_TO_SCREEN src=(%d,%d) dest id=%d (%d,%d)(%d,%d)\n", pCmd->srcOrigin.x, pCmd->srcOrigin.y, pCmd->destScreenId, pCmd->destRect.left, pCmd->destRect.top, pCmd->destRect.right, pCmd->destRect.bottom));

                /** @todo Support GMRFB.format.s.bitsPerPixel != pThis->svga.uBpp   */
                AssertBreak(pSVGAState->GMRFB.format.s.bitsPerPixel == pThis->svga.uBpp);
                AssertBreak(pCmd->destScreenId == 0);

                if (pCmd->destRect.left < 0)
                    pCmd->destRect.left = 0;
                if (pCmd->destRect.top < 0)
                    pCmd->destRect.top = 0;
                if (pCmd->destRect.right < 0)
                    pCmd->destRect.right = 0;
                if (pCmd->destRect.bottom < 0)
                    pCmd->destRect.bottom = 0;

                width  = pCmd->destRect.right - pCmd->destRect.left;
                height = pCmd->destRect.bottom - pCmd->destRect.top;

                if (    width == 0
                    ||  height == 0)
                    break;  /* Nothing to do. */

                /* Clip to screen dimensions. */
                if (width > pThis->svga.uWidth)
                    width = pThis->svga.uWidth;
                if (height > pThis->svga.uHeight)
                    height = pThis->svga.uHeight;

                unsigned offsetSource = (pCmd->srcOrigin.x * pSVGAState->GMRFB.format.s.bitsPerPixel) / 8 + pSVGAState->GMRFB.bytesPerLine * pCmd->srcOrigin.y;
                unsigned offsetDest   = (pCmd->destRect.left * RT_ALIGN(pThis->svga.uBpp, 8)) / 8 + pThis->svga.cbScanline * pCmd->destRect.top;
                unsigned cbCopyWidth  = (width * RT_ALIGN(pThis->svga.uBpp, 8)) / 8;

                AssertBreak(offsetDest < pThis->vram_size);

                rc = vmsvgaGMRTransfer(pThis, SVGA3D_WRITE_HOST_VRAM, pThis->CTX_SUFF(vram_ptr) + offsetDest, pThis->svga.cbScanline, pSVGAState->GMRFB.ptr, offsetSource, pSVGAState->GMRFB.bytesPerLine, cbCopyWidth, height);
                AssertRC(rc);
                vgaR3UpdateDisplay(pThis, pCmd->destRect.left, pCmd->destRect.top, pCmd->destRect.right - pCmd->destRect.left, pCmd->destRect.bottom - pCmd->destRect.top);
                break;
            }

            case SVGA_CMD_BLIT_SCREEN_TO_GMRFB:
            {
                SVGAFifoCmdBlitScreenToGMRFB *pCmd;
                VMSVGAFIFO_GET_CMD_BUFFER_BREAK(pCmd, SVGAFifoCmdBlitScreenToGMRFB, sizeof(*pCmd));

                /* Note! This can fetch 3d render results as well!! */
                Log(("vmsvgaFIFOLoop: SVGA_CMD_BLIT_SCREEN_TO_GMRFB dest=(%d,%d) src id=%d (%d,%d)(%d,%d)\n", pCmd->destOrigin.x, pCmd->destOrigin.y, pCmd->srcScreenId, pCmd->srcRect.left, pCmd->srcRect.top, pCmd->srcRect.right, pCmd->srcRect.bottom));
                AssertFailed();
                break;
            }
# endif // VBOX_WITH_VMSVGA3D
            case SVGA_CMD_ANNOTATION_FILL:
            {
                SVGAFifoCmdAnnotationFill *pCmd;
                VMSVGAFIFO_GET_CMD_BUFFER_BREAK(pCmd, SVGAFifoCmdAnnotationFill, sizeof(*pCmd));

                Log(("vmsvgaFIFOLoop: SVGA_CMD_ANNOTATION_FILL red=%x green=%x blue=%x\n", pCmd->color.s.r, pCmd->color.s.g, pCmd->color.s.b));
                pSVGAState->colorAnnotation = pCmd->color;
                break;
            }

            case SVGA_CMD_ANNOTATION_COPY:
            {
                SVGAFifoCmdAnnotationCopy *pCmd;
                VMSVGAFIFO_GET_CMD_BUFFER_BREAK(pCmd, SVGAFifoCmdAnnotationCopy, sizeof(*pCmd));

                Log(("vmsvgaFIFOLoop: SVGA_CMD_ANNOTATION_COPY\n"));
                AssertFailed();
                break;
            }

            /** @todo SVGA_CMD_RECT_COPY  - see with ubuntu */

            default:
# ifdef VBOX_WITH_VMSVGA3D
                if (    enmCmdId >= SVGA_3D_CMD_BASE
                    &&  enmCmdId <  SVGA_3D_CMD_MAX)
                {
                    /* All 3d commands start with a common header, which defines the size of the command. */
                    SVGA3dCmdHeader *pHdr;
                    VMSVGAFIFO_GET_CMD_BUFFER_BREAK(pHdr, SVGA3dCmdHeader, sizeof(*pHdr));
                    AssertBreak(pHdr->size < VMSVGA_FIFO_SIZE);
                    uint32_t cbCmd = sizeof(SVGA3dCmdHeader) + pHdr->size;
                    VMSVGAFIFO_GET_MORE_CMD_BUFFER_BREAK(pHdr, SVGA3dCmdHeader, cbCmd);

/**
 * Check that the 3D command has at least a_cbMin of payload bytes after the
 * header.  Will break out of the switch if it doesn't.
 */
#  define VMSVGAFIFO_CHECK_3D_CMD_MIN_SIZE_BREAK(a_cbMin) \
    AssertMsgBreak((a_cbMin) <= pHdr->size, ("size=%#x a_cbMin=%#zx\n", pHdr->size, (size_t)(a_cbMin)))
                    switch ((int)enmCmdId)
                    {
                    case SVGA_3D_CMD_SURFACE_DEFINE:
                    {
                        uint32_t                cMipLevels;
                        SVGA3dCmdDefineSurface *pCmd = (SVGA3dCmdDefineSurface *)(pHdr + 1);
                        VMSVGAFIFO_CHECK_3D_CMD_MIN_SIZE_BREAK(sizeof(*pCmd));

                        cMipLevels = (pHdr->size - sizeof(*pCmd)) / sizeof(SVGA3dSize);
                        rc = vmsvga3dSurfaceDefine(pThis, pCmd->sid, (uint32_t)pCmd->surfaceFlags, pCmd->format, pCmd->face, 0, SVGA3D_TEX_FILTER_NONE, cMipLevels, (SVGA3dSize *)(pCmd + 1));
#  ifdef DEBUG_GMR_ACCESS
                        VMR3ReqCallWait(PDMDevHlpGetVM(pThis->pDevInsR3), VMCPUID_ANY, (PFNRT)vmsvgaResetGMRHandlers, 1, pThis);
#  endif
                        break;
                    }

                    case SVGA_3D_CMD_SURFACE_DEFINE_V2:
                    {
                        uint32_t                   cMipLevels;
                        SVGA3dCmdDefineSurface_v2 *pCmd = (SVGA3dCmdDefineSurface_v2 *)(pHdr + 1);
                        VMSVGAFIFO_CHECK_3D_CMD_MIN_SIZE_BREAK(sizeof(*pCmd));

                        cMipLevels = (pHdr->size - sizeof(*pCmd)) / sizeof(SVGA3dSize);
                        rc = vmsvga3dSurfaceDefine(pThis, pCmd->sid, pCmd->surfaceFlags, pCmd->format, pCmd->face, pCmd->multisampleCount, pCmd->autogenFilter, cMipLevels, (SVGA3dSize *)(pCmd + 1));
                        break;
                    }

                    case SVGA_3D_CMD_SURFACE_DESTROY:
                    {
                        SVGA3dCmdDestroySurface *pCmd = (SVGA3dCmdDestroySurface *)(pHdr + 1);
                        VMSVGAFIFO_CHECK_3D_CMD_MIN_SIZE_BREAK(sizeof(*pCmd));
                        rc = vmsvga3dSurfaceDestroy(pThis, pCmd->sid);
                        break;
                    }

                    case SVGA_3D_CMD_SURFACE_COPY:
                    {
                        uint32_t              cCopyBoxes;
                        SVGA3dCmdSurfaceCopy *pCmd = (SVGA3dCmdSurfaceCopy *)(pHdr + 1);
                        VMSVGAFIFO_CHECK_3D_CMD_MIN_SIZE_BREAK(sizeof(*pCmd));

                        cCopyBoxes = (pHdr->size - sizeof(pCmd)) / sizeof(SVGA3dCopyBox);
                        rc = vmsvga3dSurfaceCopy(pThis, pCmd->dest, pCmd->src, cCopyBoxes, (SVGA3dCopyBox *)(pCmd + 1));
                        break;
                    }

                    case SVGA_3D_CMD_SURFACE_STRETCHBLT:
                    {
                        SVGA3dCmdSurfaceStretchBlt *pCmd = (SVGA3dCmdSurfaceStretchBlt *)(pHdr + 1);
                        VMSVGAFIFO_CHECK_3D_CMD_MIN_SIZE_BREAK(sizeof(*pCmd));

                        rc = vmsvga3dSurfaceStretchBlt(pThis, pCmd->dest, pCmd->boxDest, pCmd->src, pCmd->boxSrc, pCmd->mode);
                        break;
                    }

                    case SVGA_3D_CMD_SURFACE_DMA:
                    {
                        uint32_t             cCopyBoxes;
                        SVGA3dCmdSurfaceDMA *pCmd = (SVGA3dCmdSurfaceDMA *)(pHdr + 1);
                        VMSVGAFIFO_CHECK_3D_CMD_MIN_SIZE_BREAK(sizeof(*pCmd));

                        cCopyBoxes = (pHdr->size - sizeof(*pCmd)) / sizeof(SVGA3dCopyBox);
                        STAM_PROFILE_START(&pSVGAState->StatR3CmdSurfaceDMA, a);
                        rc = vmsvga3dSurfaceDMA(pThis, pCmd->guest, pCmd->host, pCmd->transfer, cCopyBoxes, (SVGA3dCopyBox *)(pCmd + 1));
                        STAM_PROFILE_STOP(&pSVGAState->StatR3CmdSurfaceDMA, a);
                        break;
                    }

                    case SVGA_3D_CMD_BLIT_SURFACE_TO_SCREEN:
                    {
                        uint32_t                      cRects;
                        SVGA3dCmdBlitSurfaceToScreen *pCmd = (SVGA3dCmdBlitSurfaceToScreen *)(pHdr + 1);
                        VMSVGAFIFO_CHECK_3D_CMD_MIN_SIZE_BREAK(sizeof(*pCmd));

                        cRects = (pHdr->size - sizeof(*pCmd)) / sizeof(SVGASignedRect);
                        rc = vmsvga3dSurfaceBlitToScreen(pThis, pCmd->destScreenId, pCmd->destRect, pCmd->srcImage, pCmd->srcRect, cRects, (SVGASignedRect *)(pCmd + 1));
                        break;
                    }

                    case SVGA_3D_CMD_CONTEXT_DEFINE:
                    {
                        SVGA3dCmdDefineContext *pCmd = (SVGA3dCmdDefineContext *)(pHdr + 1);
                        VMSVGAFIFO_CHECK_3D_CMD_MIN_SIZE_BREAK(sizeof(*pCmd));

                        rc = vmsvga3dContextDefine(pThis, pCmd->cid);
                        break;
                    }

                    case SVGA_3D_CMD_CONTEXT_DESTROY:
                    {
                        SVGA3dCmdDestroyContext *pCmd = (SVGA3dCmdDestroyContext *)(pHdr + 1);
                        VMSVGAFIFO_CHECK_3D_CMD_MIN_SIZE_BREAK(sizeof(*pCmd));

                        rc = vmsvga3dContextDestroy(pThis, pCmd->cid);
                        break;
                    }

                    case SVGA_3D_CMD_SETTRANSFORM:
                    {
                        SVGA3dCmdSetTransform *pCmd = (SVGA3dCmdSetTransform *)(pHdr + 1);
                        VMSVGAFIFO_CHECK_3D_CMD_MIN_SIZE_BREAK(sizeof(*pCmd));

                        rc = vmsvga3dSetTransform(pThis, pCmd->cid, pCmd->type, pCmd->matrix);
                        break;
                    }

                    case SVGA_3D_CMD_SETZRANGE:
                    {
                        SVGA3dCmdSetZRange *pCmd = (SVGA3dCmdSetZRange *)(pHdr + 1);
                        VMSVGAFIFO_CHECK_3D_CMD_MIN_SIZE_BREAK(sizeof(*pCmd));

                        rc = vmsvga3dSetZRange(pThis, pCmd->cid, pCmd->zRange);
                        break;
                    }

                    case SVGA_3D_CMD_SETRENDERSTATE:
                    {
                        uint32_t                 cRenderStates;
                        SVGA3dCmdSetRenderState *pCmd = (SVGA3dCmdSetRenderState *)(pHdr + 1);
                        VMSVGAFIFO_CHECK_3D_CMD_MIN_SIZE_BREAK(sizeof(*pCmd));

                        cRenderStates = (pHdr->size - sizeof(*pCmd)) / sizeof(SVGA3dRenderState);
                        rc = vmsvga3dSetRenderState(pThis, pCmd->cid, cRenderStates, (SVGA3dRenderState *)(pCmd + 1));
                        break;
                    }

                    case SVGA_3D_CMD_SETRENDERTARGET:
                    {
                        SVGA3dCmdSetRenderTarget *pCmd = (SVGA3dCmdSetRenderTarget *)(pHdr + 1);
                        VMSVGAFIFO_CHECK_3D_CMD_MIN_SIZE_BREAK(sizeof(*pCmd));

                        rc = vmsvga3dSetRenderTarget(pThis, pCmd->cid, pCmd->type, pCmd->target);
                        break;
                    }

                    case SVGA_3D_CMD_SETTEXTURESTATE:
                    {
                        uint32_t                  cTextureStates;
                        SVGA3dCmdSetTextureState *pCmd = (SVGA3dCmdSetTextureState *)(pHdr + 1);
                        VMSVGAFIFO_CHECK_3D_CMD_MIN_SIZE_BREAK(sizeof(*pCmd));

                        cTextureStates = (pHdr->size - sizeof(*pCmd)) / sizeof(SVGA3dTextureState);
                        rc = vmsvga3dSetTextureState(pThis, pCmd->cid, cTextureStates, (SVGA3dTextureState *)(pCmd + 1));
                        break;
                    }

                    case SVGA_3D_CMD_SETMATERIAL:
                    {
                        SVGA3dCmdSetMaterial *pCmd = (SVGA3dCmdSetMaterial *)(pHdr + 1);
                        VMSVGAFIFO_CHECK_3D_CMD_MIN_SIZE_BREAK(sizeof(*pCmd));

                        rc = vmsvga3dSetMaterial(pThis, pCmd->cid, pCmd->face, &pCmd->material);
                        break;
                    }

                    case SVGA_3D_CMD_SETLIGHTDATA:
                    {
                        SVGA3dCmdSetLightData *pCmd = (SVGA3dCmdSetLightData *)(pHdr + 1);
                        VMSVGAFIFO_CHECK_3D_CMD_MIN_SIZE_BREAK(sizeof(*pCmd));

                        rc = vmsvga3dSetLightData(pThis, pCmd->cid, pCmd->index, &pCmd->data);
                        break;
                    }

                    case SVGA_3D_CMD_SETLIGHTENABLED:
                    {
                        SVGA3dCmdSetLightEnabled *pCmd = (SVGA3dCmdSetLightEnabled *)(pHdr + 1);
                        VMSVGAFIFO_CHECK_3D_CMD_MIN_SIZE_BREAK(sizeof(*pCmd));

                        rc = vmsvga3dSetLightEnabled(pThis, pCmd->cid, pCmd->index, pCmd->enabled);
                        break;
                    }

                    case SVGA_3D_CMD_SETVIEWPORT:
                    {
                        SVGA3dCmdSetViewport *pCmd = (SVGA3dCmdSetViewport *)(pHdr + 1);
                        VMSVGAFIFO_CHECK_3D_CMD_MIN_SIZE_BREAK(sizeof(*pCmd));

                        rc = vmsvga3dSetViewPort(pThis, pCmd->cid, &pCmd->rect);
                        break;
                    }

                    case SVGA_3D_CMD_SETCLIPPLANE:
                    {
                        SVGA3dCmdSetClipPlane *pCmd = (SVGA3dCmdSetClipPlane *)(pHdr + 1);
                        VMSVGAFIFO_CHECK_3D_CMD_MIN_SIZE_BREAK(sizeof(*pCmd));

                        rc = vmsvga3dSetClipPlane(pThis, pCmd->cid, pCmd->index, pCmd->plane);
                        break;
                    }

                    case SVGA_3D_CMD_CLEAR:
                    {
                        SVGA3dCmdClear  *pCmd = (SVGA3dCmdClear *)(pHdr + 1);
                        VMSVGAFIFO_CHECK_3D_CMD_MIN_SIZE_BREAK(sizeof(*pCmd));
                        uint32_t         cRects;

                        cRects = (pHdr->size - sizeof(*pCmd)) / sizeof(SVGA3dRect);
                        rc = vmsvga3dCommandClear(pThis, pCmd->cid, pCmd->clearFlag, pCmd->color, pCmd->depth, pCmd->stencil, cRects, (SVGA3dRect *)(pCmd + 1));
                        break;
                    }

                    case SVGA_3D_CMD_PRESENT:
                    {
                        SVGA3dCmdPresent *pCmd = (SVGA3dCmdPresent *)(pHdr + 1);
                        VMSVGAFIFO_CHECK_3D_CMD_MIN_SIZE_BREAK(sizeof(*pCmd));
                        uint32_t          cRects;

                        cRects = (pHdr->size - sizeof(*pCmd)) / sizeof(SVGA3dCopyRect);

                        STAM_PROFILE_START(&pSVGAState->StatR3CmdPresent, a);
                        rc = vmsvga3dCommandPresent(pThis, pCmd->sid, cRects, (SVGA3dCopyRect *)(pCmd + 1));
                        STAM_PROFILE_STOP(&pSVGAState->StatR3CmdPresent, a);
                        break;
                    }

                    case SVGA_3D_CMD_SHADER_DEFINE:
                    {
                        SVGA3dCmdDefineShader *pCmd = (SVGA3dCmdDefineShader *)(pHdr + 1);
                        VMSVGAFIFO_CHECK_3D_CMD_MIN_SIZE_BREAK(sizeof(*pCmd));
                        uint32_t               cbData;

                        cbData = (pHdr->size - sizeof(*pCmd));
                        rc = vmsvga3dShaderDefine(pThis, pCmd->cid, pCmd->shid, pCmd->type, cbData, (uint32_t *)(pCmd + 1));
                        break;
                    }

                    case SVGA_3D_CMD_SHADER_DESTROY:
                    {
                        SVGA3dCmdDestroyShader *pCmd = (SVGA3dCmdDestroyShader *)(pHdr + 1);
                        VMSVGAFIFO_CHECK_3D_CMD_MIN_SIZE_BREAK(sizeof(*pCmd));

                        rc = vmsvga3dShaderDestroy(pThis, pCmd->cid, pCmd->shid, pCmd->type);
                        break;
                    }

                    case SVGA_3D_CMD_SET_SHADER:
                    {
                        SVGA3dCmdSetShader *pCmd = (SVGA3dCmdSetShader *)(pHdr + 1);
                        VMSVGAFIFO_CHECK_3D_CMD_MIN_SIZE_BREAK(sizeof(*pCmd));

                        rc = vmsvga3dShaderSet(pThis, NULL, pCmd->cid, pCmd->type, pCmd->shid);
                        break;
                    }

                    case SVGA_3D_CMD_SET_SHADER_CONST:
                    {
                        SVGA3dCmdSetShaderConst *pCmd = (SVGA3dCmdSetShaderConst *)(pHdr + 1);
                        VMSVGAFIFO_CHECK_3D_CMD_MIN_SIZE_BREAK(sizeof(*pCmd));

                        uint32_t cRegisters = (pHdr->size - sizeof(*pCmd)) / sizeof(pCmd->values) + 1;
                        rc = vmsvga3dShaderSetConst(pThis, pCmd->cid, pCmd->reg, pCmd->type, pCmd->ctype, cRegisters, pCmd->values);
                        break;
                    }

                    case SVGA_3D_CMD_DRAW_PRIMITIVES:
                    {
                        SVGA3dCmdDrawPrimitives *pCmd = (SVGA3dCmdDrawPrimitives *)(pHdr + 1);
                        VMSVGAFIFO_CHECK_3D_CMD_MIN_SIZE_BREAK(sizeof(*pCmd));
                        uint32_t                 cVertexDivisor;

                        cVertexDivisor = (pHdr->size - sizeof(*pCmd) - sizeof(SVGA3dVertexDecl) * pCmd->numVertexDecls - sizeof(SVGA3dPrimitiveRange) * pCmd->numRanges);
                        Assert(pCmd->numRanges <= SVGA3D_MAX_DRAW_PRIMITIVE_RANGES);
                        Assert(pCmd->numVertexDecls <= SVGA3D_MAX_VERTEX_ARRAYS);
                        Assert(!cVertexDivisor || cVertexDivisor == pCmd->numVertexDecls);

                        SVGA3dVertexDecl     *pVertexDecl    = (SVGA3dVertexDecl *)(pCmd + 1);
                        SVGA3dPrimitiveRange *pNumRange      = (SVGA3dPrimitiveRange *) (&pVertexDecl[pCmd->numVertexDecls]);
                        SVGA3dVertexDivisor  *pVertexDivisor = (cVertexDivisor) ? (SVGA3dVertexDivisor *)(&pNumRange[pCmd->numRanges]) : NULL;

                        STAM_PROFILE_START(&pSVGAState->StatR3CmdDrawPrimitive, a);
                        rc = vmsvga3dDrawPrimitives(pThis, pCmd->cid, pCmd->numVertexDecls, pVertexDecl, pCmd->numRanges, pNumRange, cVertexDivisor, pVertexDivisor);
                        STAM_PROFILE_STOP(&pSVGAState->StatR3CmdDrawPrimitive, a);
                        break;
                    }

                    case SVGA_3D_CMD_SETSCISSORRECT:
                    {
                        SVGA3dCmdSetScissorRect *pCmd = (SVGA3dCmdSetScissorRect *)(pHdr + 1);
                        VMSVGAFIFO_CHECK_3D_CMD_MIN_SIZE_BREAK(sizeof(*pCmd));

                        rc = vmsvga3dSetScissorRect(pThis, pCmd->cid, &pCmd->rect);
                        break;
                    }

                    case SVGA_3D_CMD_BEGIN_QUERY:
                    {
                        SVGA3dCmdBeginQuery *pCmd = (SVGA3dCmdBeginQuery *)(pHdr + 1);
                        VMSVGAFIFO_CHECK_3D_CMD_MIN_SIZE_BREAK(sizeof(*pCmd));

                        rc = vmsvga3dQueryBegin(pThis, pCmd->cid, pCmd->type);
                        break;
                    }

                    case SVGA_3D_CMD_END_QUERY:
                    {
                        SVGA3dCmdEndQuery *pCmd = (SVGA3dCmdEndQuery *)(pHdr + 1);
                        VMSVGAFIFO_CHECK_3D_CMD_MIN_SIZE_BREAK(sizeof(*pCmd));

                        rc = vmsvga3dQueryEnd(pThis, pCmd->cid, pCmd->type, pCmd->guestResult);
                        break;
                    }

                    case SVGA_3D_CMD_WAIT_FOR_QUERY:
                    {
                        SVGA3dCmdWaitForQuery *pCmd = (SVGA3dCmdWaitForQuery *)(pHdr + 1);
                        VMSVGAFIFO_CHECK_3D_CMD_MIN_SIZE_BREAK(sizeof(*pCmd));

                        rc = vmsvga3dQueryWait(pThis, pCmd->cid, pCmd->type, pCmd->guestResult);
                        break;
                    }

                    case SVGA_3D_CMD_GENERATE_MIPMAPS:
                    {
                        SVGA3dCmdGenerateMipmaps *pCmd = (SVGA3dCmdGenerateMipmaps *)(pHdr + 1);
                        VMSVGAFIFO_CHECK_3D_CMD_MIN_SIZE_BREAK(sizeof(*pCmd));

                        rc = vmsvga3dGenerateMipmaps(pThis, pCmd->sid, pCmd->filter);
                        break;
                    }

                    case SVGA_3D_CMD_ACTIVATE_SURFACE:
                    case SVGA_3D_CMD_DEACTIVATE_SURFACE:
                        /* context id + surface id? */
                        break;

                    default:
                        STAM_REL_COUNTER_INC(&pSVGAState->StatFifoUnkCmds);
                        AssertFailed();
                        break;
                    }
                }
                else
# endif // VBOX_WITH_VMSVGA3D
                {
                    STAM_REL_COUNTER_INC(&pSVGAState->StatFifoUnkCmds);
                    AssertFailed();
                }
            }

            /* Go to the next slot */
            Assert(cbPayload + sizeof(uint32_t) <= offFifoMax - offFifoMin);
            offCurrentCmd += RT_ALIGN_32(cbPayload + sizeof(uint32_t), sizeof(uint32_t));
            if (offCurrentCmd >= offFifoMax)
            {
                offCurrentCmd -= offFifoMax - offFifoMin;
                Assert(offCurrentCmd >= offFifoMin);
                Assert(offCurrentCmd <  offFifoMax);
            }
            ASMAtomicWriteU32(&pFIFO[SVGA_FIFO_STOP], offCurrentCmd);
            STAM_REL_COUNTER_INC(&pSVGAState->StatFifoCommands);

            /*
             * Raise IRQ if required.  Must enter the critical section here
             * before making final decisions here, otherwise cubebench and
             * others may end up waiting forever.
             */
            if (   u32IrqStatus
                || (pThis->svga.u32IrqMask & SVGA_IRQFLAG_FIFO_PROGRESS))
            {
                PDMCritSectEnter(&pThis->CritSect, VERR_IGNORED);

                /* FIFO progress might trigger an interrupt. */
                if (pThis->svga.u32IrqMask & SVGA_IRQFLAG_FIFO_PROGRESS)
                {
                    Log(("vmsvgaFIFOLoop: fifo progress irq\n"));
                    u32IrqStatus |= SVGA_IRQFLAG_FIFO_PROGRESS;
                }

                /* Unmasked IRQ pending? */
                if (pThis->svga.u32IrqMask & u32IrqStatus)
                {
                    Log(("vmsvgaFIFOLoop: Trigger interrupt with status %x\n", u32IrqStatus));
                    ASMAtomicOrU32(&pThis->svga.u32IrqStatus, u32IrqStatus);
                    PDMDevHlpPCISetIrq(pDevIns, 0, 1);
                }

                PDMCritSectLeave(&pThis->CritSect);
            }
        }

        /* If really done, clear the busy flag. */
        if (fDone)
        {
            Log(("vmsvgaFIFOLoop: emptied the FIFO next=%x stop=%x\n", pFIFO[SVGA_FIFO_NEXT_CMD], offCurrentCmd));
            vmsvgaFifoSetNotBusy(pThis, pSVGAState, offFifoMin);
        }
    }

    /*
     * Free the bounce buffer. (There are no returns above!)
     */
    RTMemFree(pbBounceBuf);

    return VINF_SUCCESS;
}

/**
 * Free the specified GMR
 *
 * @param   pThis           VGA device instance data.
 * @param   idGMR           GMR id
 */
void vmsvgaGMRFree(PVGASTATE pThis, uint32_t idGMR)
{
    PVMSVGASTATE pSVGAState = (PVMSVGASTATE)pThis->svga.pSVGAState;

    /* Free the old descriptor if present. */
    if (pSVGAState->aGMR[idGMR].numDescriptors)
    {
        PGMR pGMR = &pSVGAState->aGMR[idGMR];
# ifdef DEBUG_GMR_ACCESS
        VMR3ReqCallWait(PDMDevHlpGetVM(pThis->pDevInsR3), VMCPUID_ANY, (PFNRT)vmsvgaDeregisterGMR, 2, pThis->pDevInsR3, idGMR);
# endif

        Assert(pGMR->paDesc);
        RTMemFree(pGMR->paDesc);
        pGMR->paDesc         = NULL;
        pGMR->numDescriptors = 0;
        pGMR->cbTotal        = 0;
        pGMR->cMaxPages      = 0;
    }
    Assert(!pSVGAState->aGMR[idGMR].cbTotal);
}

/**
 * Copy from a GMR to host memory or vice versa
 *
 * @returns VBox status code.
 * @param   pThis           VGA device instance data.
 * @param   enmTransferType Transfer type (read/write)
 * @param   pbDst           Host destination pointer
 * @param   cbDestPitch     Destination buffer pitch
 * @param   src             GMR description
 * @param   offSrc          Source buffer offset
 * @param   cbSrcPitch      Source buffer pitch
 * @param   cbWidth         Source width in bytes
 * @param   cHeight         Source height
 */
int vmsvgaGMRTransfer(PVGASTATE pThis, const SVGA3dTransferType enmTransferType, uint8_t *pbDst, int32_t cbDestPitch,
                      SVGAGuestPtr src, uint32_t offSrc, int32_t cbSrcPitch, uint32_t cbWidth, uint32_t cHeight)
{
    PVMSVGASTATE            pSVGAState = (PVMSVGASTATE)pThis->svga.pSVGAState;
    PGMR                    pGMR;
    int                     rc;
    PVMSVGAGMRDESCRIPTOR    pDesc;
    unsigned                offDesc = 0;

    Log(("vmsvgaGMRTransfer: gmr=%x offset=%x pitch=%d cbWidth=%d cHeight=%d; src offset=%d src pitch=%d\n",
         src.gmrId, src.offset, cbDestPitch, cbWidth, cHeight, offSrc, cbSrcPitch));
    Assert(cbWidth && cHeight);

    /* Shortcut for the framebuffer. */
    if (src.gmrId == SVGA_GMR_FRAMEBUFFER)
    {
        offSrc += src.offset;
        AssertMsgReturn(src.offset < pThis->vram_size,
                        ("src.offset=%#x offSrc=%#x cbSrcPitch=%#x cHeight=%#x cbWidth=%#x cbTotal=%#x vram_size=%#x\n",
                         src.offset, offSrc, cbSrcPitch, cHeight, cbWidth, pThis->vram_size),
                        VERR_INVALID_PARAMETER);
        AssertMsgReturn(offSrc + cbSrcPitch * (cHeight - 1) + cbWidth <= pThis->vram_size,
                        ("src.offset=%#x offSrc=%#x cbSrcPitch=%#x cHeight=%#x cbWidth=%#x cbTotal=%#x vram_size=%#x\n",
                         src.offset, offSrc, cbSrcPitch, cHeight, cbWidth, pThis->vram_size),
                        VERR_INVALID_PARAMETER);

        uint8_t *pSrc  = pThis->CTX_SUFF(vram_ptr) + offSrc;

        if (enmTransferType == SVGA3D_READ_HOST_VRAM)
        {
            /* switch src & dest */
            uint8_t *pTemp      = pbDst;
            int32_t cbTempPitch = cbDestPitch;

            pbDst = pSrc;
            pSrc  = pTemp;

            cbDestPitch = cbSrcPitch;
            cbSrcPitch  = cbTempPitch;
        }

        if (   pThis->svga.cbScanline == (uint32_t)cbDestPitch
            && cbWidth                == (uint32_t)cbDestPitch
            && cbSrcPitch             == cbDestPitch)
        {
            memcpy(pbDst, pSrc, cbWidth * cHeight);
        }
        else
        {
            for(uint32_t i = 0; i < cHeight; i++)
            {
                memcpy(pbDst, pSrc, cbWidth);

                pbDst += cbDestPitch;
                pSrc  += cbSrcPitch;
            }
        }
        return VINF_SUCCESS;
    }

    AssertReturn(src.gmrId < VMSVGA_MAX_GMR_IDS, VERR_INVALID_PARAMETER);
    pGMR   = &pSVGAState->aGMR[src.gmrId];
    pDesc  = pGMR->paDesc;

    offSrc += src.offset;
    AssertMsgReturn(src.offset < pGMR->cbTotal,
                    ("src.gmrId=%#x src.offset=%#x offSrc=%#x cbSrcPitch=%#x cHeight=%#x cbWidth=%#x cbTotal=%#x\n",
                     src.gmrId, src.offset, offSrc, cbSrcPitch, cHeight, cbWidth, pGMR->cbTotal),
                    VERR_INVALID_PARAMETER);
    AssertMsgReturn(offSrc + cbSrcPitch * (cHeight - 1) + cbWidth <= pGMR->cbTotal,
                    ("src.gmrId=%#x src.offset=%#x offSrc=%#x cbSrcPitch=%#x cHeight=%#x cbWidth=%#x cbTotal=%#x\n",
                     src.gmrId, src.offset, offSrc, cbSrcPitch, cHeight, cbWidth, pGMR->cbTotal),
                    VERR_INVALID_PARAMETER);

    for (uint32_t i = 0; i < cHeight; i++)
    {
        uint32_t cbCurrentWidth = cbWidth;
        uint32_t offCurrent     = offSrc;
        uint8_t *pCurrentDest   = pbDst;

        /* Find the right descriptor */
        while (offDesc + pDesc->numPages * PAGE_SIZE <= offCurrent)
        {
            offDesc += pDesc->numPages * PAGE_SIZE;
            AssertReturn(offDesc < pGMR->cbTotal, VERR_INTERNAL_ERROR); /* overflow protection */
            pDesc++;
        }

        while (cbCurrentWidth)
        {
            uint32_t cbToCopy;

            if (offCurrent + cbCurrentWidth <= offDesc + pDesc->numPages * PAGE_SIZE)
            {
                cbToCopy = cbCurrentWidth;
            }
            else
            {
                cbToCopy = (offDesc + pDesc->numPages * PAGE_SIZE - offCurrent);
                AssertReturn(cbToCopy <= cbCurrentWidth, VERR_INVALID_PARAMETER);
            }

            LogFlow(("vmsvgaGMRTransfer: %s phys=%RGp\n", (enmTransferType == SVGA3D_WRITE_HOST_VRAM) ? "READ" : "WRITE", pDesc->GCPhys + offCurrent - offDesc));

            if (enmTransferType == SVGA3D_WRITE_HOST_VRAM)
                rc = PDMDevHlpPhysRead(pThis->CTX_SUFF(pDevIns), pDesc->GCPhys + offCurrent - offDesc, pCurrentDest, cbToCopy);
            else
                rc = PDMDevHlpPhysWrite(pThis->CTX_SUFF(pDevIns), pDesc->GCPhys + offCurrent - offDesc, pCurrentDest, cbToCopy);
            AssertRCBreak(rc);

            cbCurrentWidth -= cbToCopy;
            offCurrent     += cbToCopy;
            pCurrentDest   += cbToCopy;

            /* Go to the next descriptor if there's anything left. */
            if (cbCurrentWidth)
            {
                offDesc += pDesc->numPages * PAGE_SIZE;
                pDesc++;
            }
        }

        offSrc  += cbSrcPitch;
        pbDst   += cbDestPitch;
    }

    return VINF_SUCCESS;
}

/**
 * Unblock the FIFO I/O thread so it can respond to a state change.
 *
 * @returns VBox status code.
 * @param   pDevIns     The VGA device instance.
 * @param   pThread     The send thread.
 */
static DECLCALLBACK(int) vmsvgaFIFOLoopWakeUp(PPDMDEVINS pDevIns, PPDMTHREAD pThread)
{
    PVGASTATE pThis = (PVGASTATE)pThread->pvUser;
    Log(("vmsvgaFIFOLoopWakeUp\n"));
    return SUPSemEventSignal(pThis->svga.pSupDrvSession, pThis->svga.FIFORequestSem);
}

/**
 * Enables or disables dirty page tracking for the framebuffer
 *
 * @param   pThis           VGA device instance data.
 * @param   fTraces         Enable/disable traces
 */
static void vmsvgaSetTraces(PVGASTATE pThis, bool fTraces)
{
    if (    (!pThis->svga.fConfigured || !pThis->svga.fEnabled)
        &&  !fTraces)
    {
        //Assert(pThis->svga.fTraces);
        Log(("vmsvgaSetTraces: *not* allowed to disable dirty page tracking when the device is in legacy mode.\n"));
        return;
    }

    pThis->svga.fTraces = fTraces;
    if (pThis->svga.fTraces)
    {
        unsigned cbFrameBuffer = pThis->vram_size;

        Log(("vmsvgaSetTraces: enable dirty page handling for the frame buffer only (%x bytes)\n", 0));
        if (pThis->svga.uHeight != VMSVGA_VAL_UNINITIALIZED)
        {
            Assert(pThis->svga.cbScanline);
            /* Hardware enabled; return real framebuffer size .*/
            cbFrameBuffer = (uint32_t)pThis->svga.uHeight * pThis->svga.cbScanline;
            cbFrameBuffer = RT_ALIGN(cbFrameBuffer, PAGE_SIZE);
        }

        if (!pThis->svga.fVRAMTracking)
        {
            Log(("vmsvgaSetTraces: enable frame buffer dirty page tracking. (%x bytes; vram %x)\n", cbFrameBuffer, pThis->vram_size));
            vgaR3RegisterVRAMHandler(pThis, cbFrameBuffer);
            pThis->svga.fVRAMTracking = true;
        }
    }
    else
    {
        if (pThis->svga.fVRAMTracking)
        {
            Log(("vmsvgaSetTraces: disable frame buffer dirty page tracking\n"));
            vgaR3UnregisterVRAMHandler(pThis);
            pThis->svga.fVRAMTracking = false;
        }
    }
}

/**
 * Callback function for mapping a PCI I/O region.
 *
 * @return VBox status code.
 * @param   pPciDev         Pointer to PCI device.
 *                          Use pPciDev->pDevIns to get the device instance.
 * @param   iRegion         The region number.
 * @param   GCPhysAddress   Physical address of the region.
 *                          If iType is PCI_ADDRESS_SPACE_IO, this is an
 *                          I/O port, else it's a physical address.
 *                          This address is *NOT* relative
 *                          to pci_mem_base like earlier!
 * @param   enmType         One of the PCI_ADDRESS_SPACE_* values.
 */
DECLCALLBACK(int) vmsvgaR3IORegionMap(PPCIDEVICE pPciDev, int iRegion, RTGCPHYS GCPhysAddress, uint32_t cb, PCIADDRESSSPACE enmType)
{
    int         rc;
    PPDMDEVINS  pDevIns = pPciDev->pDevIns;
    PVGASTATE   pThis = PDMINS_2_DATA(pDevIns, PVGASTATE);

    Log(("vgasvgaR3IORegionMap: iRegion=%d GCPhysAddress=%RGp cb=%#x enmType=%d\n", iRegion, GCPhysAddress, cb, enmType));
    if (enmType == PCI_ADDRESS_SPACE_IO)
    {
        AssertReturn(iRegion == 0, VERR_INTERNAL_ERROR);
        rc = PDMDevHlpIOPortRegister(pDevIns, (RTIOPORT)GCPhysAddress, cb, 0,
                                     vmsvgaIOWrite, vmsvgaIORead, NULL /* OutStr */, NULL /* InStr */, "VMSVGA");
        if (RT_FAILURE(rc))
            return rc;
        if (pThis->fR0Enabled)
        {
            rc = PDMDevHlpIOPortRegisterR0(pDevIns, (RTIOPORT)GCPhysAddress, cb, 0,
                                           "vmsvgaIOWrite", "vmsvgaIORead", NULL, NULL, "VMSVGA");
            if (RT_FAILURE(rc))
                return rc;
        }
        if (pThis->fGCEnabled)
        {
            rc = PDMDevHlpIOPortRegisterRC(pDevIns, (RTIOPORT)GCPhysAddress, cb, 0,
                                           "vmsvgaIOWrite", "vmsvgaIORead", NULL, NULL, "VMSVGA");
            if (RT_FAILURE(rc))
                return rc;
        }

        pThis->svga.BasePort = GCPhysAddress;
        Log(("vmsvgaR3IORegionMap: base port = %x\n", pThis->svga.BasePort));
    }
    else
    {
        AssertReturn(iRegion == 2 && enmType == PCI_ADDRESS_SPACE_MEM, VERR_INTERNAL_ERROR);
        if (GCPhysAddress != NIL_RTGCPHYS)
        {
            /*
             * Mapping the FIFO RAM.
             */
            rc = PDMDevHlpMMIO2Map(pDevIns, iRegion, GCPhysAddress);
            AssertRC(rc);

# ifdef DEBUG_FIFO_ACCESS
            if (RT_SUCCESS(rc))
            {
                rc = PGMHandlerPhysicalRegister(PDMDevHlpGetVM(pDevIns), GCPhysAddress, GCPhysAddress + (VMSVGA_FIFO_SIZE - 1),
                                                pThis->svga.hFifoAccessHandlerType, pThis, NIL_RTR0PTR, NIL_RTRCPTR,
                                                "VMSVGA FIFO");
                AssertRC(rc);
            }
# endif
            if (RT_SUCCESS(rc))
            {
                pThis->svga.GCPhysFIFO = GCPhysAddress;
                Log(("vmsvgaR3IORegionMap: FIFO address = %RGp\n", GCPhysAddress));
            }
        }
        else
        {
            Assert(pThis->svga.GCPhysFIFO);
# ifdef DEBUG_FIFO_ACCESS
            rc = PGMHandlerPhysicalDeregister(PDMDevHlpGetVM(pDevIns), pThis->svga.GCPhysFIFO);
            AssertRC(rc);
# endif
            pThis->svga.GCPhysFIFO = 0;
        }

    }
    return VINF_SUCCESS;
}


/**
 * @callback_method_impl{FNDBGFHANDLERDEV}
 */
static DECLCALLBACK(void) vmsvgaInfo(PPDMDEVINS pDevIns, PCDBGFINFOHLP pHlp, const char *pszArgs)
{
    PVGASTATE    pThis = PDMINS_2_DATA(pDevIns, PVGASTATE);
    PVMSVGASTATE pSVGAState = (PVMSVGASTATE)pThis->svga.pSVGAState;

    pHlp->pfnPrintf(pHlp, "Extension enabled:  %RTbool\n", pThis->svga.fEnabled);
    pHlp->pfnPrintf(pHlp, "Configured:         %RTbool\n", pThis->svga.fConfigured);
    pHlp->pfnPrintf(pHlp, "Base I/O port:      %#x\n", pThis->svga.BasePort);
    pHlp->pfnPrintf(pHlp, "Host windows ID:    %#RX64\n", pThis->svga.u64HostWindowId);
    pHlp->pfnPrintf(pHlp, "FIFO address:       %RGp\n", pThis->svga.GCPhysFIFO);
    pHlp->pfnPrintf(pHlp, "FIFO size:          %u (%#x)\n", pThis->svga.cbFIFO, pThis->svga.cbFIFO);
    pHlp->pfnPrintf(pHlp, "FIFO external cmd:  %#x\n", pThis->svga.u8FIFOExtCommand);
    pHlp->pfnPrintf(pHlp, "Busy:               %#x\n", pThis->svga.fBusy);
    pHlp->pfnPrintf(pHlp, "Traces:             %RTbool (effective: %RTbool)\n", pThis->svga.fTraces, pThis->svga.fVRAMTracking);
    pHlp->pfnPrintf(pHlp, "Guest ID:           %#x (%d)\n", pThis->svga.u32GuestId, pThis->svga.u32GuestId);
    pHlp->pfnPrintf(pHlp, "IRQ status:         %#x\n", pThis->svga.u32IrqStatus);
    pHlp->pfnPrintf(pHlp, "IRQ mask:           %#x\n", pThis->svga.u32IrqMask);
    pHlp->pfnPrintf(pHlp, "Pitch lock:         %#x\n", pThis->svga.u32PitchLock);
    pHlp->pfnPrintf(pHlp, "Current GMR ID:     %#x\n", pThis->svga.u32CurrentGMRId);
    pHlp->pfnPrintf(pHlp, "Capabilites reg:    %#x\n", pThis->svga.u32RegCaps);
    pHlp->pfnPrintf(pHlp, "Index reg:          %#x\n", pThis->svga.u32IndexReg);
    pHlp->pfnPrintf(pHlp, "Action flags:       %#x\n", pThis->svga.u32ActionFlags);
    pHlp->pfnPrintf(pHlp, "Max display size:   %ux%u\n", pThis->svga.u32MaxWidth, pThis->svga.u32MaxHeight);
    pHlp->pfnPrintf(pHlp, "Display size:       %ux%u %ubpp\n", pThis->svga.uWidth, pThis->svga.uHeight, pThis->svga.uBpp);
    pHlp->pfnPrintf(pHlp, "Scanline:           %u (%#x)\n", pThis->svga.cbScanline, pThis->svga.cbScanline);
    pHlp->pfnPrintf(pHlp, "Viewport position:  %ux%u\n", pThis->svga.viewport.x, pThis->svga.viewport.y);
    pHlp->pfnPrintf(pHlp, "Viewport size:      %ux%u\n", pThis->svga.viewport.cx, pThis->svga.viewport.cy);

    pHlp->pfnPrintf(pHlp, "Cursor active:      %RTbool\n", pSVGAState->Cursor.fActive);
    pHlp->pfnPrintf(pHlp, "Cursor hotspot:     %ux%u\n", pSVGAState->Cursor.xHotspot, pSVGAState->Cursor.yHotspot);
    pHlp->pfnPrintf(pHlp, "Cursor size:        %ux%u\n", pSVGAState->Cursor.width, pSVGAState->Cursor.height);
    pHlp->pfnPrintf(pHlp, "Cursor byte size:   %u (%#x)\n", pSVGAState->Cursor.cbData, pSVGAState->Cursor.cbData);

    pHlp->pfnPrintf(pHlp, "3D enabled:         %RTbool\n", pThis->svga.f3DEnabled);

}


/**
 * @copydoc FNSSMDEVLOADEXEC
 */
int vmsvgaLoadExec(PPDMDEVINS pDevIns, PSSMHANDLE pSSM, uint32_t uVersion, uint32_t uPass)
{
    PVGASTATE    pThis = PDMINS_2_DATA(pDevIns, PVGASTATE);
    PVMSVGASTATE pSVGAState = (PVMSVGASTATE)pThis->svga.pSVGAState;
    int          rc;

    /* Load our part of the VGAState */
    rc = SSMR3GetStructEx(pSSM, &pThis->svga, sizeof(pThis->svga), 0, g_aVGAStateSVGAFields, NULL);
    AssertRCReturn(rc, rc);

    /* Load the framebuffer backup. */
    rc = SSMR3GetMem(pSSM, pThis->svga.pFrameBufferBackup, VMSVGA_FRAMEBUFFER_BACKUP_SIZE);
    AssertRCReturn(rc, rc);

    /* Load the VMSVGA state. */
    rc = SSMR3GetStructEx(pSSM, pSVGAState, sizeof(*pSVGAState), 0, g_aVMSVGASTATEFields, NULL);
    AssertRCReturn(rc, rc);

    /* Load the active cursor bitmaps. */
    if (pSVGAState->Cursor.fActive)
    {
        pSVGAState->Cursor.pData = RTMemAlloc(pSVGAState->Cursor.cbData);
        AssertReturn(pSVGAState->Cursor.pData, VERR_NO_MEMORY);

        rc = SSMR3GetMem(pSSM, pSVGAState->Cursor.pData, pSVGAState->Cursor.cbData);
        AssertRCReturn(rc, rc);
    }

    /* Load the GMR state */
    for (uint32_t i = 0; i < RT_ELEMENTS(pSVGAState->aGMR); i++)
    {
        PGMR pGMR = &pSVGAState->aGMR[i];

        rc = SSMR3GetStructEx(pSSM, pGMR, sizeof(*pGMR), 0, g_aGMRFields, NULL);
        AssertRCReturn(rc, rc);

        if (pGMR->numDescriptors)
        {
            /* Allocate the maximum amount possible (everything non-continuous) */
            Assert(pGMR->cMaxPages || pGMR->cbTotal);
            pGMR->paDesc = (PVMSVGAGMRDESCRIPTOR)RTMemAllocZ((pGMR->cMaxPages) ? pGMR->cMaxPages : (pGMR->cbTotal >> PAGE_SHIFT) * sizeof(VMSVGAGMRDESCRIPTOR));
            AssertReturn(pGMR->paDesc, VERR_NO_MEMORY);

            for (uint32_t j = 0; j < pGMR->numDescriptors; j++)
            {
                rc = SSMR3GetStructEx(pSSM, &pGMR->paDesc[j], sizeof(pGMR->paDesc[j]), 0, g_aVMSVGAGMRDESCRIPTORFields, NULL);
                AssertRCReturn(rc, rc);
            }
        }
    }

# ifdef VBOX_WITH_VMSVGA3D
    if (pThis->svga.f3DEnabled)
    {
        VMSVGA_STATE_LOAD loadstate;

        loadstate.pSSM     = pSSM;
        loadstate.uVersion = uVersion;
        loadstate.uPass    = uPass;

        /* Save the 3d state in the FIFO thread. */
        pThis->svga.u8FIFOExtCommand = VMSVGA_FIFO_EXTCMD_LOADSTATE;
        pThis->svga.pFIFOExtCmdParam = (void *)&loadstate;
        /* Hack alert: resume the IO thread as it has been suspended before the destruct callback.
         * The PowerOff notification isn't working, so not an option in this case.
         */
        PDMR3ThreadResume(pThis->svga.pFIFOIOThread);
        SUPSemEventSignal(pThis->svga.pSupDrvSession, pThis->svga.FIFORequestSem);
        /* Wait for the end of the command. */
        rc = RTSemEventWait(pThis->svga.FIFOExtCmdSem, RT_INDEFINITE_WAIT);
        AssertRC(rc);
        PDMR3ThreadSuspend(pThis->svga.pFIFOIOThread);
    }
# endif

    return VINF_SUCCESS;
}

/**
 * Reinit the video mode after the state has been loaded.
 */
int vmsvgaLoadDone(PPDMDEVINS pDevIns)
{
    PVGASTATE    pThis = PDMINS_2_DATA(pDevIns, PVGASTATE);
    PVMSVGASTATE pSVGAState = (PVMSVGASTATE)pThis->svga.pSVGAState;

    pThis->last_bpp = VMSVGA_VAL_UNINITIALIZED;   /* force mode reset */
    vmsvgaChangeMode(pThis);

    /* Set the active cursor. */
    if (pSVGAState->Cursor.fActive)
    {
        int rc;

        rc = pThis->pDrv->pfnVBVAMousePointerShape (pThis->pDrv,
                                                    true,
                                                    true,
                                                    pSVGAState->Cursor.xHotspot,
                                                    pSVGAState->Cursor.yHotspot,
                                                    pSVGAState->Cursor.width,
                                                    pSVGAState->Cursor.height,
                                                    pSVGAState->Cursor.pData);
        AssertRC(rc);
    }
    return VINF_SUCCESS;
}

/**
 * @copydoc FNSSMDEVSAVEEXEC
 */
int vmsvgaSaveExec(PPDMDEVINS pDevIns, PSSMHANDLE pSSM)
{
    PVGASTATE    pThis = PDMINS_2_DATA(pDevIns, PVGASTATE);
    PVMSVGASTATE pSVGAState = (PVMSVGASTATE)pThis->svga.pSVGAState;
    int          rc;

    /* Save our part of the VGAState */
    rc = SSMR3PutStructEx(pSSM, &pThis->svga, sizeof(pThis->svga), 0, g_aVGAStateSVGAFields, NULL);
    AssertRCReturn(rc, rc);

    /* Save the framebuffer backup. */
    rc = SSMR3PutMem(pSSM, pThis->svga.pFrameBufferBackup, VMSVGA_FRAMEBUFFER_BACKUP_SIZE);
    AssertRCReturn(rc, rc);

    /* Save the VMSVGA state. */
    rc = SSMR3PutStructEx(pSSM, pSVGAState, sizeof(*pSVGAState), 0, g_aVMSVGASTATEFields, NULL);
    AssertRCReturn(rc, rc);

    /* Save the active cursor bitmaps. */
    if (pSVGAState->Cursor.fActive)
    {
        rc = SSMR3PutMem(pSSM, pSVGAState->Cursor.pData, pSVGAState->Cursor.cbData);
        AssertRCReturn(rc, rc);
    }

    /* Save the GMR state */
    for (uint32_t i = 0; i < RT_ELEMENTS(pSVGAState->aGMR); i++)
    {
        rc = SSMR3PutStructEx(pSSM, &pSVGAState->aGMR[i], sizeof(pSVGAState->aGMR[i]), 0, g_aGMRFields, NULL);
        AssertRCReturn(rc, rc);

        for (uint32_t j = 0; j < pSVGAState->aGMR[i].numDescriptors; j++)
        {
            rc = SSMR3PutStructEx(pSSM, &pSVGAState->aGMR[i].paDesc[j], sizeof(pSVGAState->aGMR[i].paDesc[j]), 0, g_aVMSVGAGMRDESCRIPTORFields, NULL);
            AssertRCReturn(rc, rc);
        }
    }

# ifdef VBOX_WITH_VMSVGA3D
    if (pThis->svga.f3DEnabled)
    {
        /* Save the 3d state in the FIFO thread. */
        pThis->svga.u8FIFOExtCommand = VMSVGA_FIFO_EXTCMD_SAVESTATE;
        pThis->svga.pFIFOExtCmdParam = (void *)pSSM;
        /* Hack alert: resume the IO thread as it has been suspended before the destruct callback.
         * The PowerOff notification isn't working, so not an option in this case.
         */
        PDMR3ThreadResume(pThis->svga.pFIFOIOThread);
        SUPSemEventSignal(pThis->svga.pSupDrvSession, pThis->svga.FIFORequestSem);
        /* Wait for the end of the external command. */
        rc = RTSemEventWait(pThis->svga.FIFOExtCmdSem, RT_INDEFINITE_WAIT);
        AssertRC(rc);
        PDMR3ThreadSuspend(pThis->svga.pFIFOIOThread);
    }
# endif
    return VINF_SUCCESS;
}

/**
 * Resets the SVGA hardware state
 *
 * @returns VBox status code.
 * @param   pDevIns     The device instance.
 */
int vmsvgaReset(PPDMDEVINS pDevIns)
{
    PVGASTATE pThis = PDMINS_2_DATA(pDevIns, PVGASTATE);
    PVMSVGASTATE pSVGAState = (PVMSVGASTATE)pThis->svga.pSVGAState;

    /* Reset before init? */
    if (!pSVGAState)
        return VINF_SUCCESS;

    Log(("vmsvgaReset\n"));

    pThis->svga.pFIFOR3[SVGA_FIFO_NEXT_CMD]         = pThis->svga.pFIFOR3[SVGA_FIFO_STOP]                 = 0;

    /* Reset the FIFO thread. */
    pThis->svga.u8FIFOExtCommand = VMSVGA_FIFO_EXTCMD_RESET;
    SUPSemEventSignal(pThis->svga.pSupDrvSession, pThis->svga.FIFORequestSem);
    /* Wait for the end of the termination sequence. */
    int rc = RTSemEventWait(pThis->svga.FIFOExtCmdSem, 10000);
    AssertRC(rc);

    pThis->svga.cScratchRegion = VMSVGA_SCRATCH_SIZE;
    memset(pThis->svga.au32ScratchRegion, 0, sizeof(pThis->svga.au32ScratchRegion));
    memset(pThis->svga.pSVGAState, 0, sizeof(VMSVGASTATE));
    memset(pThis->svga.pFrameBufferBackup, 0, VMSVGA_FRAMEBUFFER_BACKUP_SIZE);

    /* Register caps. */
    pThis->svga.u32RegCaps = SVGA_CAP_GMR | SVGA_CAP_GMR2 | SVGA_CAP_CURSOR | SVGA_CAP_CURSOR_BYPASS_2 | SVGA_CAP_EXTENDED_FIFO | SVGA_CAP_IRQMASK | SVGA_CAP_PITCHLOCK | SVGA_CAP_TRACES | SVGA_CAP_SCREEN_OBJECT_2 | SVGA_CAP_ALPHA_CURSOR;
# ifdef VBOX_WITH_VMSVGA3D
    pThis->svga.u32RegCaps |= SVGA_CAP_3D;
# endif

    /* Setup FIFO capabilities. */
    pThis->svga.pFIFOR3[SVGA_FIFO_CAPABILITIES] = SVGA_FIFO_CAP_FENCE | SVGA_FIFO_CAP_CURSOR_BYPASS_3 | SVGA_FIFO_CAP_GMR2 | SVGA_FIFO_CAP_3D_HWVERSION_REVISED | SVGA_FIFO_CAP_SCREEN_OBJECT_2;

    /* Valid with SVGA_FIFO_CAP_SCREEN_OBJECT_2 */
    pThis->svga.pFIFOR3[SVGA_FIFO_CURSOR_SCREEN_ID] = SVGA_ID_INVALID;

    /* VRAM tracking is enabled by default during bootup. */
    pThis->svga.fVRAMTracking = true;
    pThis->svga.fEnabled      = false;

    /* Invalidate current settings. */
    pThis->svga.uWidth     = VMSVGA_VAL_UNINITIALIZED;
    pThis->svga.uHeight    = VMSVGA_VAL_UNINITIALIZED;
    pThis->svga.uBpp       = VMSVGA_VAL_UNINITIALIZED;
    pThis->svga.cbScanline = 0;

    return rc;
}

/**
 * Cleans up the SVGA hardware state
 *
 * @returns VBox status code.
 * @param   pDevIns     The device instance.
 */
int vmsvgaDestruct(PPDMDEVINS pDevIns)
{
    PVGASTATE pThis = PDMINS_2_DATA(pDevIns, PVGASTATE);
    PVMSVGASTATE pSVGAState = (PVMSVGASTATE)pThis->svga.pSVGAState;
    int          rc;

    /* Stop the FIFO thread. */
    pThis->svga.u8FIFOExtCommand = VMSVGA_FIFO_EXTCMD_TERMINATE;
    /* Hack alert: resume the IO thread as it has been suspended before the destruct callback.
     * The PowerOff notification isn't working, so not an option in this case.
     */
    PDMR3ThreadResume(pThis->svga.pFIFOIOThread);
    SUPSemEventSignal(pThis->svga.pSupDrvSession, pThis->svga.FIFORequestSem);

    /* Wait for the end of the termination sequence. */
    rc = RTSemEventWait(pThis->svga.FIFOExtCmdSem, 10000);
    AssertRC(rc);
    PDMR3ThreadSuspend(pThis->svga.pFIFOIOThread);

    if (pSVGAState)
    {
# ifndef VMSVGA_USE_EMT_HALT_CODE
        if (pSVGAState->hBusyDelayedEmts != NIL_RTSEMEVENTMULTI)
        {
            RTSemEventMultiDestroy(pSVGAState->hBusyDelayedEmts);
            pSVGAState->hBusyDelayedEmts = NIL_RTSEMEVENT;
        }
# endif
        if (pSVGAState->Cursor.fActive)
            RTMemFree(pSVGAState->Cursor.pData);

        for (unsigned i = 0; i < RT_ELEMENTS(pSVGAState->aGMR); i++)
        {
            if (pSVGAState->aGMR[i].paDesc)
                RTMemFree(pSVGAState->aGMR[i].paDesc);
        }
        RTMemFree(pSVGAState);
    }
    if (pThis->svga.pFrameBufferBackup)
        RTMemFree(pThis->svga.pFrameBufferBackup);
    if (pThis->svga.FIFOExtCmdSem != NIL_RTSEMEVENT)
    {
        RTSemEventDestroy(pThis->svga.FIFOExtCmdSem);
        pThis->svga.FIFOExtCmdSem = NIL_RTSEMEVENT;
    }
    if (pThis->svga.FIFORequestSem != NIL_SUPSEMEVENT)
    {
        SUPSemEventClose(pThis->svga.pSupDrvSession, pThis->svga.FIFORequestSem);
        pThis->svga.FIFORequestSem = NIL_SUPSEMEVENT;
    }

    return VINF_SUCCESS;
}

/**
 * Initialize the SVGA hardware state
 *
 * @returns VBox status code.
 * @param   pDevIns     The device instance.
 */
int vmsvgaInit(PPDMDEVINS pDevIns)
{
    PVGASTATE       pThis = PDMINS_2_DATA(pDevIns, PVGASTATE);
    PVMSVGASTATE    pSVGAState;
    PVM             pVM   = PDMDevHlpGetVM(pDevIns);
    int             rc;

    pThis->svga.cScratchRegion = VMSVGA_SCRATCH_SIZE;
    memset(pThis->svga.au32ScratchRegion, 0, sizeof(pThis->svga.au32ScratchRegion));

    pThis->svga.pSVGAState = RTMemAllocZ(sizeof(VMSVGASTATE));
    AssertReturn(pThis->svga.pSVGAState, VERR_NO_MEMORY);
    pSVGAState = (PVMSVGASTATE)pThis->svga.pSVGAState;

    /* Necessary for creating a backup of the text mode frame buffer when switching into svga mode. */
    pThis->svga.pFrameBufferBackup = RTMemAllocZ(VMSVGA_FRAMEBUFFER_BACKUP_SIZE);
    AssertReturn(pThis->svga.pFrameBufferBackup, VERR_NO_MEMORY);

    /* Create event semaphore. */
    pThis->svga.pSupDrvSession = PDMDevHlpGetSupDrvSession(pDevIns);

    rc = SUPSemEventCreate(pThis->svga.pSupDrvSession, &pThis->svga.FIFORequestSem);
    if (RT_FAILURE(rc))
    {
        Log(("%s: Failed to create event semaphore for FIFO handling.\n", __FUNCTION__));
        return rc;
    }

    /* Create event semaphore. */
    rc = RTSemEventCreate(&pThis->svga.FIFOExtCmdSem);
    if (RT_FAILURE(rc))
    {
        Log(("%s: Failed to create event semaphore for external fifo cmd handling.\n", __FUNCTION__));
        return rc;
    }

# ifndef VMSVGA_USE_EMT_HALT_CODE
    /* Create semaphore for delaying EMTs wait for the FIFO to stop being busy. */
    rc = RTSemEventMultiCreate(&pSVGAState->hBusyDelayedEmts);
    AssertRCReturn(rc, rc);
# endif

    /* Register caps. */
    pThis->svga.u32RegCaps = SVGA_CAP_GMR | SVGA_CAP_GMR2 | SVGA_CAP_CURSOR | SVGA_CAP_CURSOR_BYPASS_2 | SVGA_CAP_EXTENDED_FIFO | SVGA_CAP_IRQMASK | SVGA_CAP_PITCHLOCK | SVGA_CAP_TRACES | SVGA_CAP_SCREEN_OBJECT_2 | SVGA_CAP_ALPHA_CURSOR;
# ifdef VBOX_WITH_VMSVGA3D
    pThis->svga.u32RegCaps |= SVGA_CAP_3D;
# endif

    /* Setup FIFO capabilities. */
    pThis->svga.pFIFOR3[SVGA_FIFO_CAPABILITIES] = SVGA_FIFO_CAP_FENCE | SVGA_FIFO_CAP_CURSOR_BYPASS_3 | SVGA_FIFO_CAP_GMR2 | SVGA_FIFO_CAP_3D_HWVERSION_REVISED | SVGA_FIFO_CAP_SCREEN_OBJECT_2;

    /* Valid with SVGA_FIFO_CAP_SCREEN_OBJECT_2 */
    pThis->svga.pFIFOR3[SVGA_FIFO_CURSOR_SCREEN_ID] = SVGA_ID_INVALID;

    pThis->svga.pFIFOR3[SVGA_FIFO_3D_HWVERSION] = pThis->svga.pFIFOR3[SVGA_FIFO_3D_HWVERSION_REVISED] = 0;    /* no 3d available. */
# ifdef VBOX_WITH_VMSVGA3D
    if (pThis->svga.f3DEnabled)
    {
        rc = vmsvga3dInit(pThis);
        if (RT_FAILURE(rc))
        {
            LogRel(("VMSVGA3d: 3D support disabled! (vmsvga3dInit -> %Rrc)\n", rc));
            pThis->svga.f3DEnabled = false;
        }
    }
# endif
    /* VRAM tracking is enabled by default during bootup. */
    pThis->svga.fVRAMTracking = true;

    /* Invalidate current settings. */
    pThis->svga.uWidth     = VMSVGA_VAL_UNINITIALIZED;
    pThis->svga.uHeight    = VMSVGA_VAL_UNINITIALIZED;
    pThis->svga.uBpp       = VMSVGA_VAL_UNINITIALIZED;
    pThis->svga.cbScanline = 0;

    pThis->svga.u32MaxWidth  = VBE_DISPI_MAX_YRES;
    pThis->svga.u32MaxHeight = VBE_DISPI_MAX_XRES;
    while (pThis->svga.u32MaxWidth * pThis->svga.u32MaxHeight * 4 /* 32 bpp */ > pThis->vram_size)
    {
        pThis->svga.u32MaxWidth  -= 256;
        pThis->svga.u32MaxHeight -= 256;
    }
    Log(("VMSVGA: Maximum size (%d,%d)\n", pThis->svga.u32MaxWidth, pThis->svga.u32MaxHeight));

# ifdef DEBUG_GMR_ACCESS
    /* Register the GMR access handler type. */
    rc = PGMR3HandlerPhysicalTypeRegister(PDMDevHlpGetVM(pThis->pDevInsR3), PGMPHYSHANDLERKIND_WRITE,
                                          vmsvgaR3GMRAccessHandler,
                                          NULL, NULL, NULL,
                                          NULL, NULL, NULL,
                                          "VMSVGA GMR", &pThis->svga.hGmrAccessHandlerType);
    AssertRCReturn(rc, rc);
# endif
# ifdef DEBUG_FIFO_ACCESS
    rc = PGMR3HandlerPhysicalTypeRegister(PDMDevHlpGetVM(pThis->pDevInsR3), PGMPHYSHANDLERKIND_ALL,
                                          vmsvgaR3FIFOAccessHandler,
                                          NULL, NULL, NULL,
                                          NULL, NULL, NULL,
                                          "VMSVGA FIFO", &pThis->svga.hFifoAccessHandlerType);
    AssertRCReturn(rc, rc);
#endif

    /* Create the async IO thread. */
    rc = PDMDevHlpThreadCreate(pDevIns, &pThis->svga.pFIFOIOThread, pThis, vmsvgaFIFOLoop, vmsvgaFIFOLoopWakeUp, 0,
                                RTTHREADTYPE_IO, "VMSVGA FIFO");
    if (RT_FAILURE(rc))
    {
        AssertMsgFailed(("%s: Async IO Thread creation for FIFO handling failed rc=%d\n", __FUNCTION__, rc));
        return rc;
    }

    /*
     * Statistics.
     */
    STAM_REG(pVM, &pSVGAState->StatR3CmdPresent,        STAMTYPE_PROFILE, "/Devices/VMSVGA/3d/Cmd/Present",  STAMUNIT_TICKS_PER_CALL, "Profiling of Present.");
    STAM_REG(pVM, &pSVGAState->StatR3CmdDrawPrimitive,  STAMTYPE_PROFILE, "/Devices/VMSVGA/3d/Cmd/DrawPrimitive",  STAMUNIT_TICKS_PER_CALL, "Profiling of DrawPrimitive.");
    STAM_REG(pVM, &pSVGAState->StatR3CmdSurfaceDMA,     STAMTYPE_PROFILE, "/Devices/VMSVGA/3d/Cmd/SurfaceDMA",  STAMUNIT_TICKS_PER_CALL, "Profiling of SurfaceDMA.");
    STAM_REL_REG(pVM, &pSVGAState->StatBusyDelayEmts,   STAMTYPE_PROFILE, "/Devices/VMSVGA/EmtDelayOnBusyFifo",  STAMUNIT_TICKS_PER_CALL, "Time we've delayed EMTs because of busy FIFO thread.");
    STAM_REL_REG(pVM, &pSVGAState->StatFifoCommands,    STAMTYPE_COUNTER, "/Devices/VMSVGA/FifoCommands",  STAMUNIT_OCCURENCES, "FIFO command counter.");
    STAM_REL_REG(pVM, &pSVGAState->StatFifoErrors,      STAMTYPE_COUNTER, "/Devices/VMSVGA/FifoErrors",  STAMUNIT_OCCURENCES, "FIFO error counter.");
    STAM_REL_REG(pVM, &pSVGAState->StatFifoUnkCmds,     STAMTYPE_COUNTER, "/Devices/VMSVGA/FifoUnknownCommands",  STAMUNIT_OCCURENCES, "FIFO unknown command counter.");
    STAM_REL_REG(pVM, &pSVGAState->StatFifoTodoTimeout, STAMTYPE_COUNTER, "/Devices/VMSVGA/FifoTodoTimeout",  STAMUNIT_OCCURENCES, "Number of times we discovered pending work after a wait timeout.");
    STAM_REL_REG(pVM, &pSVGAState->StatFifoTodoWoken,   STAMTYPE_COUNTER, "/Devices/VMSVGA/FifoTodoWoken",  STAMUNIT_OCCURENCES, "Number of times we discovered pending work after being woken up.");
    STAM_REL_REG(pVM, &pSVGAState->StatFifoStalls,      STAMTYPE_PROFILE, "/Devices/VMSVGA/FifoStalls",  STAMUNIT_TICKS_PER_CALL, "Profiling of FIFO stalls (waiting for guest to finish copying data).");

    /*
     * Info handlers.
     */
    PDMDevHlpDBGFInfoRegister(pDevIns, "vmsvga", "Basic VMSVGA device state details", vmsvgaInfo);

    return VINF_SUCCESS;
}

# ifdef VBOX_WITH_VMSVGA3D
/** Names for the vmsvga 3d capabilities, prefixed with format type hint char. */
static const char * const g_apszVmSvgaDevCapNames[] =
{
    "x3D",                           /* = 0 */
    "xMAX_LIGHTS",
    "xMAX_TEXTURES",
    "xMAX_CLIP_PLANES",
    "xVERTEX_SHADER_VERSION",
    "xVERTEX_SHADER",
    "xFRAGMENT_SHADER_VERSION",
    "xFRAGMENT_SHADER",
    "xMAX_RENDER_TARGETS",
    "xS23E8_TEXTURES",
    "xS10E5_TEXTURES",
    "xMAX_FIXED_VERTEXBLEND",
    "xD16_BUFFER_FORMAT",
    "xD24S8_BUFFER_FORMAT",
    "xD24X8_BUFFER_FORMAT",
    "xQUERY_TYPES",
    "xTEXTURE_GRADIENT_SAMPLING",
    "rMAX_POINT_SIZE",
    "xMAX_SHADER_TEXTURES",
    "xMAX_TEXTURE_WIDTH",
    "xMAX_TEXTURE_HEIGHT",
    "xMAX_VOLUME_EXTENT",
    "xMAX_TEXTURE_REPEAT",
    "xMAX_TEXTURE_ASPECT_RATIO",
    "xMAX_TEXTURE_ANISOTROPY",
    "xMAX_PRIMITIVE_COUNT",
    "xMAX_VERTEX_INDEX",
    "xMAX_VERTEX_SHADER_INSTRUCTIONS",
    "xMAX_FRAGMENT_SHADER_INSTRUCTIONS",
    "xMAX_VERTEX_SHADER_TEMPS",
    "xMAX_FRAGMENT_SHADER_TEMPS",
    "xTEXTURE_OPS",
    "xSURFACEFMT_X8R8G8B8",
    "xSURFACEFMT_A8R8G8B8",
    "xSURFACEFMT_A2R10G10B10",
    "xSURFACEFMT_X1R5G5B5",
    "xSURFACEFMT_A1R5G5B5",
    "xSURFACEFMT_A4R4G4B4",
    "xSURFACEFMT_R5G6B5",
    "xSURFACEFMT_LUMINANCE16",
    "xSURFACEFMT_LUMINANCE8_ALPHA8",
    "xSURFACEFMT_ALPHA8",
    "xSURFACEFMT_LUMINANCE8",
    "xSURFACEFMT_Z_D16",
    "xSURFACEFMT_Z_D24S8",
    "xSURFACEFMT_Z_D24X8",
    "xSURFACEFMT_DXT1",
    "xSURFACEFMT_DXT2",
    "xSURFACEFMT_DXT3",
    "xSURFACEFMT_DXT4",
    "xSURFACEFMT_DXT5",
    "xSURFACEFMT_BUMPX8L8V8U8",
    "xSURFACEFMT_A2W10V10U10",
    "xSURFACEFMT_BUMPU8V8",
    "xSURFACEFMT_Q8W8V8U8",
    "xSURFACEFMT_CxV8U8",
    "xSURFACEFMT_R_S10E5",
    "xSURFACEFMT_R_S23E8",
    "xSURFACEFMT_RG_S10E5",
    "xSURFACEFMT_RG_S23E8",
    "xSURFACEFMT_ARGB_S10E5",
    "xSURFACEFMT_ARGB_S23E8",
    "xMISSING62",
    "xMAX_VERTEX_SHADER_TEXTURES",
    "xMAX_SIMULTANEOUS_RENDER_TARGETS",
    "xSURFACEFMT_V16U16",
    "xSURFACEFMT_G16R16",
    "xSURFACEFMT_A16B16G16R16",
    "xSURFACEFMT_UYVY",
    "xSURFACEFMT_YUY2",
    "xMULTISAMPLE_NONMASKABLESAMPLES",
    "xMULTISAMPLE_MASKABLESAMPLES",
    "xALPHATOCOVERAGE",
    "xSUPERSAMPLE",
    "xAUTOGENMIPMAPS",
    "xSURFACEFMT_NV12",
    "xSURFACEFMT_AYUV",
    "xMAX_CONTEXT_IDS",
    "xMAX_SURFACE_IDS",
    "xSURFACEFMT_Z_DF16",
    "xSURFACEFMT_Z_DF24",
    "xSURFACEFMT_Z_D24S8_INT",
    "xSURFACEFMT_BC4_UNORM",
    "xSURFACEFMT_BC5_UNORM", /* 83 */
};
# endif


/**
 * Power On notification.
 *
 * @returns VBox status.
 * @param   pDevIns     The device instance data.
 *
 * @remarks Caller enters the device critical section.
 */
DECLCALLBACK(void) vmsvgaR3PowerOn(PPDMDEVINS pDevIns)
{
    PVGASTATE pThis = PDMINS_2_DATA(pDevIns, PVGASTATE);
    int       rc;

# ifdef VBOX_WITH_VMSVGA3D
    if (pThis->svga.f3DEnabled)
    {
        rc = vmsvga3dPowerOn(pThis);

        if (RT_SUCCESS(rc))
        {
            bool              fSavedBuffering = RTLogRelSetBuffering(true);
            SVGA3dCapsRecord *pCaps;
            SVGA3dCapPair    *pData;
            uint32_t          idxCap  = 0;

            /* 3d hardware version; latest and greatest */
            pThis->svga.pFIFOR3[SVGA_FIFO_3D_HWVERSION_REVISED] = SVGA3D_HWVERSION_CURRENT;
            pThis->svga.pFIFOR3[SVGA_FIFO_3D_HWVERSION]         = SVGA3D_HWVERSION_CURRENT;

            pCaps = (SVGA3dCapsRecord *)&pThis->svga.pFIFOR3[SVGA_FIFO_3D_CAPS];
            pCaps->header.type   = SVGA3DCAPS_RECORD_DEVCAPS;
            pData = (SVGA3dCapPair *)&pCaps->data;

            /* Fill out all 3d capabilities. */
            for (unsigned i = 0; i < SVGA3D_DEVCAP_MAX; i++)
            {
                uint32_t val = 0;

                rc = vmsvga3dQueryCaps(pThis, i, &val);
                if (RT_SUCCESS(rc))
                {
                    pData[idxCap][0] = i;
                    pData[idxCap][1] = val;
                    idxCap++;
                    if (g_apszVmSvgaDevCapNames[i][0] == 'x')
                        LogRel(("VMSVGA3d: cap[%u]=%#010x {%s}\n", i, val, &g_apszVmSvgaDevCapNames[i][1]));
                    else
                        LogRel(("VMSVGA3d: cap[%u]=%d.%04u {%s}\n", i, (int)*(float *)&val, (unsigned)(*(float *)&val * 10000) % 10000,
                                &g_apszVmSvgaDevCapNames[i][1]));
                }
                else
                    LogRel(("VMSVGA3d: cap[%u]=failed rc=%Rrc! {%s}\n", i, rc, &g_apszVmSvgaDevCapNames[i][1]));
            }
            pCaps->header.length = (sizeof(pCaps->header) + idxCap * sizeof(SVGA3dCapPair)) / sizeof(uint32_t);
            pCaps = (SVGA3dCapsRecord *)((uint32_t *)pCaps + pCaps->header.length);

            /* Mark end of record array. */
            pCaps->header.length = 0;

            RTLogRelSetBuffering(fSavedBuffering);
        }
    }
# endif // VBOX_WITH_VMSVGA3D
}

#endif /* IN_RING3 */

