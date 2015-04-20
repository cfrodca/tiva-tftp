/*
 *  ======== netHooks.c ========
 *  This file contains hook functions to start the send and recv threads
 */
 
#include <xdc/std.h>
#include <ti/ndk/inc/netmain.h>

#include "tftp.h"

static HANDLE hTftp = 0;

void netOpenHook()
{
    // Create our local servers
	hTftp = DaemonNew( SOCK_DGRAM, 0, PORT_TFTP, dtask_tftp,
                       OS_TASKPRINORM, OS_TASKSTKNORM, 0, 1 );
}

void netCloseHook()
{
    DaemonFree(hTftp);
}
