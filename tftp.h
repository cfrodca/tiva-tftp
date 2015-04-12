/*
 * tftp.h
 *
 *  Created on: 6/04/2015
 *      Author: Cristian
 */

#ifndef SERVERS_TFTP_H_
#define SERVERS_TFTP_H_

#include <ti/ndk/inc/netmain.h>
#include <ti/ndk/inc/_stack.h>

#define SEGSIZE   512     /* data segment size */

#define PORT_TFTP 69

int dtask_tftp( SOCKET s, UINT32 unused );
typedef int (*tTFTPRequest)(char *fileName, char *buffer, UINT32 *bufferSize, UINT16 block);
void tftpInit(tTFTPRequest pfnRequest);


#endif /* SERVERS_TFTP_H_ */
