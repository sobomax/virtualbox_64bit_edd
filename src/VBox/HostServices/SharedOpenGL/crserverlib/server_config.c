    /* Copyright (c) 2001, Stanford University
 * All rights reserved
 *
 * See the file LICENSE.txt for information on redistributing this software.
 */

#include <string.h>
#include "cr_mem.h"
#include "cr_environment.h"
#include "cr_string.h"
#include "cr_error.h"
#include "cr_glstate.h"
#include "server.h"

#ifdef WINDOWS
#pragma warning( disable: 4706 )
#endif

static void
setDefaults(void)
{
    if (!cr_server.tcpip_port)
        cr_server.tcpip_port = DEFAULT_SERVER_PORT;
    cr_server.run_queue = NULL;
    cr_server.optimizeBucket = 1;
    cr_server.useL2 = 0;
    cr_server.maxBarrierCount = 0;
    cr_server.ignore_papi = 0;
    cr_server.only_swap_once = 0;
    cr_server.overlapBlending = 0;
    cr_server.debug_barriers = 0;
    cr_server.sharedDisplayLists = 0;
    cr_server.sharedTextureObjects = 0;
    cr_server.sharedPrograms = 0;
    cr_server.sharedWindows = 0;
    cr_server.useDMX = 0;
    cr_server.vpProjectionMatrixParameter = -1;
    cr_server.vpProjectionMatrixVariable = NULL;
    cr_server.currentProgram = 0;

    cr_server.num_overlap_intens = 0;
    cr_server.overlap_intens = 0;
    crMemset(&cr_server.MainContextInfo, 0, sizeof (cr_server.MainContextInfo));

    crMatrixInit(&cr_server.viewMatrix[0]);
    crMatrixInit(&cr_server.viewMatrix[1]);
    crMatrixInit(&cr_server.projectionMatrix[0]);
    crMatrixInit(&cr_server.projectionMatrix[1]);
    cr_server.currentEye = -1;

    cr_server.uniqueWindows = 0;

    cr_server.screenCount = 0;
    cr_server.bUsePBOForReadback = GL_FALSE;
    cr_server.bWindowsInitiallyHidden = GL_FALSE;

    cr_server.pfnNotifyEventCB = NULL;
}

/* Check if host reports minimal OpenGL capabilities.
 * For example, on Windows host this may happen if host has no graphics
 * card drivers installed or drivers were not properly signed or VBox
 * is running via remote desktop session etc. Currently, we take care
 * about Windows host only when specific RENDERER and VERSION strings
 * returned in this case. Later this check should be expanded to the
 * rest of hosts. */
static bool crServerHasInsufficientCaps()
{
    const char *sRealRender;
    const char *sRealVersion;

    if (!cr_server.head_spu)
        return true;

    sRealRender  = cr_server.head_spu->dispatch_table.GetString(GL_REAL_RENDERER);
    sRealVersion = cr_server.head_spu->dispatch_table.GetString(GL_REAL_VERSION);

    if (sRealRender && RTStrCmp(sRealRender, "GDI Generic") == 0)
        if (sRealVersion && RTStrCmp(sRealVersion, "1.1.0") == 0)
            return true;
    return false;
}

void crServerSetVBoxConfiguration()
{
    CRMuralInfo *defaultMural;
    char response[8096];

    char **spuchain;
    int num_spus;
    int *spu_ids;
    char **spu_names;
    char *spu_dir = NULL;
    int i;
    /* Quadrics defaults */
    int my_rank = 0;
    int low_context = CR_QUADRICS_DEFAULT_LOW_CONTEXT;
    int high_context = CR_QUADRICS_DEFAULT_HIGH_CONTEXT;
    unsigned char key[16]= {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    char hostname[1024];
    char **clientchain, **clientlist;
    GLint dims[4];
    const char * env;

    defaultMural = (CRMuralInfo *) crHashtableSearch(cr_server.muralTable, 0);
    CRASSERT(defaultMural);

    setDefaults();

    /*
     * Get my hostname
     */
    if (crGetHostname(hostname, sizeof(hostname)))
    {
        crError("CRServer: Couldn't get my own hostname?");
    }

    strcpy(response, "1 0 render");
    crDebug("CRServer: my SPU chain: %s", response);

    /* response will describe the SPU chain.
     * Example "2 5 wet 6 render"
     */
    spuchain = crStrSplit(response, " ");
    num_spus = crStrToInt(spuchain[0]);
    spu_ids = (int *) crAlloc(num_spus * sizeof(*spu_ids));
    spu_names = (char **) crAlloc((num_spus + 1) * sizeof(*spu_names));
    for (i = 0; i < num_spus; i++)
    {
        spu_ids[i] = crStrToInt(spuchain[2 * i + 1]);
        spu_names[i] = crStrdup(spuchain[2 * i + 2]);
        crDebug("SPU %d/%d: (%d) \"%s\"", i + 1, num_spus, spu_ids[i],
                        spu_names[i]);
    }
    spu_names[i] = NULL;

    //spu_dir = crStrdup(response);
    crNetSetRank(0);
    crNetSetContextRange(32, 35);
    crNetSetNodeRange("iam0", "iamvis20");
    crNetSetKey(key,sizeof(key));
    crNetSetKey(key,sizeof(key));
    cr_server.tcpip_port = 7000;

        /*cr_server.optimizeBucket = crStrToInt(response);
        cr_server.localTileSpec = crStrToInt(response);
        cr_server.useL2 = crStrToInt(response);
        cr_server.ignore_papi = crStrToInt(response);
        if (crMothershipGetServerParam(conn, response, "overlap_blending"))
        {
            if (!crStrcmp(response, "blend"))
                cr_server.overlapBlending = 1;
            else if (!crStrcmp(response, "knockout"))
                cr_server.overlapBlending = 2;
        }
        if (crMothershipGetServerParam(conn, response, "overlap_levels"))
        cr_server.only_swap_once = crStrToInt(response);
        cr_server.debug_barriers = crStrToInt(response);
        cr_server.sharedDisplayLists = crStrToInt(response);
        cr_server.sharedTextureObjects = crStrToInt(response);
        cr_server.sharedPrograms = crStrToInt(response);
        cr_server.sharedWindows = crStrToInt(response);
        cr_server.uniqueWindows = crStrToInt(response);
        cr_server.useDMX = crStrToInt(response);
        if (crMothershipGetServerParam(conn, response, "vertprog_projection_param"))
        if (crMothershipGetServerParam(conn, response, "stereo_view"))
        if (crMothershipGetServerParam(conn, response, "view_matrix"))
        if (crMothershipGetServerParam(conn, response, "right_view_matrix"))
        if (crMothershipGetServerParam(conn, response, "projection_matrix"))
        if (crMothershipGetServerParam(conn, response, "right_projection_matrix"))*/

    crDebug("CRServer: my port number is %d", cr_server.tcpip_port);

    /*
     * Load the SPUs
     */
    cr_server.head_spu =
        crSPULoadChain(num_spus, spu_ids, spu_names, spu_dir, &cr_server);

    env = crGetenv( "CR_SERVER_DEFAULT_VISUAL_BITS" );
    if (env != NULL && env[0] != '\0')
    {
        unsigned int bits = (unsigned int)crStrParseI32(env, 0);
        if (bits <= CR_ALL_BITS)
            cr_server.fVisualBitsDefault = bits;
        else
            crWarning("invalid bits option %c", bits);
    }
    else
        cr_server.fVisualBitsDefault = CR_RGB_BIT | CR_ALPHA_BIT | CR_DOUBLE_BIT;

    env = crGetenv("CR_SERVER_CAPS");
    if (env && env[0] != '\0')
    {
        cr_server.u32Caps = crStrParseI32(env, 0);
        cr_server.u32Caps &= CR_VBOX_CAPS_ALL;
    }
    else
    {
        cr_server.u32Caps = CR_VBOX_CAP_TEX_PRESENT
                | CR_VBOX_CAP_CMDVBVA
                | CR_VBOX_CAP_CMDBLOCKS
                | CR_VBOX_CAP_GETATTRIBSLOCATIONS
                | CR_VBOX_CAP_CMDBLOCKS_FLUSH
                ;
    }

    if (crServerHasInsufficientCaps())
    {
        crDebug("Cfg: report minimal OpenGL capabilities");
        cr_server.u32Caps |= CR_VBOX_CAP_HOST_CAPS_NOT_SUFFICIENT;
    }

    crInfo("Cfg: u32Caps(%#x), fVisualBitsDefault(%#x)",
            cr_server.u32Caps,
            cr_server.fVisualBitsDefault);

    /* Need to do this as early as possible */

    cr_server.head_spu->dispatch_table.GetChromiumParametervCR(GL_WINDOW_POSITION_CR, 0, GL_INT, 2, &dims[0]);
    cr_server.head_spu->dispatch_table.GetChromiumParametervCR(GL_WINDOW_SIZE_CR, 0, GL_INT, 2, &dims[2]);
    
    defaultMural->gX = dims[0];
    defaultMural->gY = dims[1];
    defaultMural->width = dims[2];
    defaultMural->height = dims[3];

    crFree(spu_ids);
    crFreeStrings(spu_names);
    crFreeStrings(spuchain);
    if (spu_dir)
        crFree(spu_dir);

    cr_server.mtu = 1024 * 30;

    /*
     * Get a list of all the clients talking to me.
     */
    if (cr_server.vncMode) {
        /* we're inside a vnc viewer */
        /*if (!crMothershipSendString( conn, response, "getvncclient %s", hostname ))
            crError( "Bad Mothership response: %s", response );*/
    }
    else {
        //crMothershipGetClients(conn, response);
        strcpy(response, "1 tcpip 1");
    }

    crDebug("CRServer: my clients: %s", response);

    /*
     * 'response' will now contain a number indicating the number of clients
     * of this server, followed by a comma-separated list of protocol/SPU ID
     * pairs.
     * Example: "3 tcpip 1,gm 2,via 10"
     */
    clientchain = crStrSplitn(response, " ", 1);
    cr_server.numClients = crStrToInt(clientchain[0]);
    if (cr_server.numClients == 0)
    {
        crError("I have no clients!  What's a poor server to do?");
    }
    clientlist = crStrSplit(clientchain[1], ",");

    /*
     * Connect to initial set of clients.
     * Call crNetAcceptClient() for each client.
     * Also, look for a client that's _not_ using the file: protocol.
     */
    for (i = 0; i < cr_server.numClients; i++)
    {
        CRClient *newClient = (CRClient *) crCalloc(sizeof(CRClient));
#ifdef VBOX
        sscanf(clientlist[i], "%1023s %d", cr_server.protocol, &(newClient->spu_id));
#else
        sscanf(clientlist[i], "%s %d", cr_server.protocol, &(newClient->spu_id));
#endif
        newClient->conn = crNetAcceptClient(cr_server.protocol, NULL,
                                            cr_server.tcpip_port,
                                            cr_server.mtu, 0);
        newClient->currentCtxInfo = &cr_server.MainContextInfo;
        crServerAddToRunQueue(newClient);

        cr_server.clients[i] = newClient;
    }

    /* set default client and mural */
    if (cr_server.numClients > 0) {
         cr_server.curClient = cr_server.clients[0];
         cr_server.curClient->currentMural = defaultMural;
         cr_server.client_spu_id =cr_server.clients[0]->spu_id;
    }

    crFreeStrings(clientchain);
    crFreeStrings(clientlist);

    /* Ask the mothership for the tile info */
    //crServerGetTileInfoFromMothership(conn, defaultMural);

    if (cr_server.vncMode) {
        /* In vnc mode, we reset the mothership configuration so that it can be
         * used by subsequent OpenGL apps without having to spawn a new mothership
         * on a new port.
         */
        crDebug("CRServer: Resetting mothership to initial state");
        //crMothershipReset(conn);
    }

    //crMothershipDisconnect(conn);
}

void crServerSetVBoxConfigurationHGCM()
{
    CRMuralInfo *defaultMural;

    int spu_ids[1]     = {0};
    char *spu_names[1] = {"render"};
    char *spu_dir = NULL;
    int i;
    GLint dims[4];
    const char * env;

    defaultMural = (CRMuralInfo *) crHashtableSearch(cr_server.muralTable, 0);
    CRASSERT(defaultMural);

    //@todo should be moved to addclient so we have a chain for each client

    setDefaults();
    
    /* Load the SPUs */    
    cr_server.head_spu = crSPULoadChain(1, spu_ids, spu_names, spu_dir, &cr_server);

    if (!cr_server.head_spu)
        return;


    env = crGetenv( "CR_SERVER_DEFAULT_VISUAL_BITS" );
    if (env != NULL && env[0] != '\0')
    {
        unsigned int bits = (unsigned int)crStrParseI32(env, 0);
        if (bits <= CR_ALL_BITS)
            cr_server.fVisualBitsDefault = bits;
        else
            crWarning("invalid bits option %c", bits);
    }
    else
        cr_server.fVisualBitsDefault = CR_RGB_BIT | CR_ALPHA_BIT | CR_DOUBLE_BIT;

    env = crGetenv("CR_SERVER_CAPS");
    if (env && env[0] != '\0')
    {
        cr_server.u32Caps = crStrParseI32(env, 0);
        cr_server.u32Caps &= CR_VBOX_CAPS_ALL;
    }
    else
    {
        cr_server.u32Caps = CR_VBOX_CAP_TEX_PRESENT
                | CR_VBOX_CAP_CMDVBVA
                | CR_VBOX_CAP_CMDBLOCKS
                | CR_VBOX_CAP_GETATTRIBSLOCATIONS
                | CR_VBOX_CAP_CMDBLOCKS_FLUSH
                ;
    }

    if (crServerHasInsufficientCaps())
    {
        crDebug("Cfg: report minimal OpenGL capabilities");
        cr_server.u32Caps |= CR_VBOX_CAP_HOST_CAPS_NOT_SUFFICIENT;
    }

    crInfo("Cfg: u32Caps(%#x), fVisualBitsDefault(%#x)",
            cr_server.u32Caps,
            cr_server.fVisualBitsDefault);

    cr_server.head_spu->dispatch_table.GetChromiumParametervCR(GL_WINDOW_POSITION_CR, 0, GL_INT, 2, &dims[0]);
    cr_server.head_spu->dispatch_table.GetChromiumParametervCR(GL_WINDOW_SIZE_CR, 0, GL_INT, 2, &dims[2]);
    
    defaultMural->gX = dims[0];
    defaultMural->gY = dims[1];
    defaultMural->width = dims[2];
    defaultMural->height = dims[3];

    cr_server.mtu = 1024 * 250;

    cr_server.numClients = 0;
    strcpy(cr_server.protocol, "vboxhgcm");

    for (i = 0; i < cr_server.numClients; i++)
    {
        CRClient *newClient = (CRClient *) crCalloc(sizeof(CRClient));
        newClient->spu_id = 0;
        newClient->conn = crNetAcceptClient(cr_server.protocol, NULL,
                                            cr_server.tcpip_port,
                                            cr_server.mtu, 0);
        newClient->currentCtxInfo = &cr_server.MainContextInfo;
        crServerAddToRunQueue(newClient);

        cr_server.clients[i] = newClient;
    }

    /* set default client and mural */
    if (cr_server.numClients > 0) {
         cr_server.curClient = cr_server.clients[0];
         cr_server.curClient->currentMural = defaultMural;
         cr_server.client_spu_id =cr_server.clients[0]->spu_id;
    }
}
