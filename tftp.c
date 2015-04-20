/***
 @file tftp.c
 @date 06/04/2015 10:49:23
 @author Cristian
 @brief Simple TFTP Server
 */

#include <string.h>     // memcpy
#include "tftp.h"

/* Trivial File Transfer Protocol */

/* TFTP Packet types. */
#define RRQ     1               /* read request */
#define WRQ     2               /* write request */
#define DATA    3               /* data packet */
#define ACK     4               /* acknowledgement */
#define ERROR   5               /* error code */

typedef enum {
	TFTP_MODE_NETASCII = 0,
	TFTP_MODE_OCTET,
	TFTP_MODE_INVALID
} tTFTPMode;

struct tftphdr {
	short opcode; /* packet type */
	short block; /* block # */
	char data[1]; /* data or error string */
};

#define TFTP_HEADER 4
#define DATA_SIZE (SEGSIZE + TFTP_HEADER) /* SEGSIZE declared in TFTP.H as 512 */

/* structure of a TFTP instance */
typedef struct _tftp {
	char *szFileName; /* Filename supplied by caller */

	char *Buffer; /* Buffer supplied by caller */
	UINT32 BufferSize; /* Buffer size supplied by caller */

	SOCKET Sock; /* Socket used for transfer */
	char *PacketBuffer; /* Packet Buffer */
	UINT32 Length; /* Length of packet send and reveive */

	UINT32 BufferUsed; /* Amount of "Buffer" used */
	UINT32 FileSize; /* Size of specified file */
	UINT16 NextBlock; /* Next expected block */
	UINT16 ErrorCode; /* TFTP error code from server */
	int MaxSyncError; /* Max SYNC errors remaining */

	struct sockaddr_in localaddr; /* inaddr for RECV */
	struct sockaddr_in peeraddr; /* inaddr for SEND */
} TFTP;

#define MAX_SYNC_TRIES          4   /* Max retries */
#define TFTP_SOCK_TIMEOUT       10  /* Packet Timeout in Seconds */

/* Application connection notification callback. */
static tTFTPRequest g_pfnRequest;

/* Local functions */
static int tftpSocketRestart(TFTP *);
static int tftpReadPacket(TFTP *);
static int tftpProcessPacket(TFTP *);
static int tftpGetMode(char *, UINT32, tTFTPMode *);
static int tftpChangeListenPort(TFTP *);
static void tftpErrorBuild(TFTP *, tTFTPError);
static void tftpDataBuild(TFTP *);
static int tftpSend(TFTP *);
static int tftpReSync(TFTP *);
static void tftpFlushPackets(TFTP *);

/**
 @brief TFTP Server, Main task management.
 */
int dtask_tftp(SOCKET s, UINT32 unused) {
	TFTP *pTftp;
	int rc; /* Return Code */
	struct timeval timeout;

	/* If the callback function is null, abort */
	if (g_pfnRequest == NULL)
		goto ABORT;

	/* Malloc Parameter Structure */
	if (!(pTftp = mmAlloc(sizeof(TFTP))))
		goto ABORT;

	/* Initialize parameters to "NULL" */
	bzero(pTftp, sizeof(TFTP));

	/* Malloc Packet Data Buffer */
	if (!(pTftp->PacketBuffer = mmAlloc(DATA_SIZE))) {
		goto ABORT;
	}

	/* Initialize address and local port */
	bzero(&pTftp->localaddr, sizeof(struct sockaddr_in));
	pTftp->localaddr.sin_family = AF_INET;
	pTftp->localaddr.sin_addr.s_addr = 0;
	pTftp->localaddr.sin_port = htons(PORT_TFTP);

	/* Save a copy of the socket */
	pTftp->Sock = s;

	/* Set the socket IO timeout */
	timeout.tv_sec = TFTP_SOCK_TIMEOUT;
	timeout.tv_usec = 0;

	if (setsockopt(pTftp->Sock, SOL_SOCKET, SO_SNDTIMEO, &timeout,
			sizeof(timeout)) < 0)
		goto ABORT;

	if (setsockopt(pTftp->Sock, SOL_SOCKET, SO_RCVTIMEO, &timeout,
			sizeof(timeout)) < 0)
		goto ABORT;

	/* Main loop */
	for (;;) {
		/* Try and get a reply packet */
		rc = tftpReadPacket(pTftp);
		if (rc < 0)
			goto ABORT;

		/* Process the reply packet */
		rc = tftpProcessPacket(pTftp);
		if (rc < 0)
			goto ABORT;

		/* If done, break out of loop */
		if (rc == 1)
			break;
	}

	rc = tftpSocketRestart(pTftp);
	if (rc < 0) {
		goto ABORT;
	}

	/* Since the socket is still open, return "1" */
	/* (we need to leave UDP sockets open) */
	rc = 1;
	goto LEAVE;

	ABORT: rc = 0;
	if (pTftp->Sock != INVALID_SOCKET)
		fdClose(pTftp->Sock);
	LEAVE: if (pTftp->PacketBuffer)
		mmFree(pTftp->PacketBuffer);
	mmFree(pTftp);

	return (rc);
}

/**
 @brief Restart the server listening port
 */
static int tftpSocketRestart(TFTP *pTftp) {
	int rc; /* Return Code */
	
	pTftp->localaddr.sin_port = htons(PORT_TFTP);

	if (bind(pTftp->Sock, (struct sockaddr *) &pTftp->localaddr,
			sizeof(pTftp->localaddr)) < 0) {
		rc = TFTPERROR_SOCKET;
	} else
		rc = 0;

	return (rc);
}

/**
 @brief Change the server listening port
 */
static int tftpChangeListenPort(TFTP *pTftp) {
	struct timeval timeout;
	int rc; /* Return Code */

	/* Close the socket */
	if (pTftp->Sock != INVALID_SOCKET)
		fdClose(pTftp->Sock);

	/* Create UDP socket */
	pTftp->Sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (pTftp->Sock == INVALID_SOCKET) {
		rc = TFTPERROR_SOCKET;
		goto ABORT;
	}

	/* Assign the same client port */
	pTftp->localaddr.sin_port = pTftp->peeraddr.sin_port;

	/* Assign a new local name */
	if (bind(pTftp->Sock, (struct sockaddr *) &pTftp->localaddr,
			sizeof(pTftp->localaddr)) < 0) {
		rc = TFTPERROR_SOCKET;
		goto ABORT;
	}

	/* Assign new timeouts */
	timeout.tv_sec = TFTP_SOCK_TIMEOUT;
	timeout.tv_usec = 0;

	if (setsockopt(pTftp->Sock, SOL_SOCKET, SO_SNDTIMEO, &timeout,
			sizeof(timeout)) < 0) {
		rc = TFTPERROR_SOCKET;
		goto ABORT;
	}

	if (setsockopt(pTftp->Sock, SOL_SOCKET, SO_RCVTIMEO, &timeout,
			sizeof(timeout)) < 0) {
		rc = TFTPERROR_SOCKET;
		goto ABORT;
	}
	rc = 0;

	ABORT: return (rc);
}

/**
 @brief Read packet from the client
 */
static int tftpReadPacket(TFTP *pTftp) {
	int rc = 0;
	int addrLength;
	struct tftphdr *ReadBuffer;
	INT32 BytesRead;
	UINT32 TimeStart;

	ReadBuffer = (struct tftphdr *) pTftp->PacketBuffer;
	TimeStart = llTimerGetTime(0);

	RETRY:
	/* Don't allow stray traffic to keep us alive */
	if ((TimeStart + TFTP_SOCK_TIMEOUT) <= llTimerGetTime(0)) {
		BytesRead = 0;
		goto ABORT;
	}

	/* Attempt to read data */
	addrLength = sizeof(pTftp->peeraddr);
	BytesRead = (int) recvfrom(pTftp->Sock, ReadBuffer, DATA_SIZE, 0,
			(struct sockaddr *) &pTftp->peeraddr, &addrLength);

	/* Handle read errors first */
	if (BytesRead < 0) {
		/* On a timeout error, ABORT with no error */
		/* Else report error */
		if (fdError() == EWOULDBLOCK)
			BytesRead = 0;
		else
			rc = TFTPERROR_SOCKET;
		goto ABORT;
	}

	/* If the local port is NOT TFTP, then it must match the peer */
	/* peer. */
	if (pTftp->localaddr.sin_port != htons(PORT_TFTP)) {
		if (pTftp->peeraddr.sin_port != pTftp->peeraddr.sin_port)
			goto RETRY;
	}

	ABORT: pTftp->Length = (UINT32) BytesRead; /*  Store number of bytes read */
	return (rc);
}

/**
 @brief Process the packet read from tftpReadPacket()
 @retval  1 Operation completed
 @retval  0 Operation in process
 @retval <0 Error condition
 */
static int tftpProcessPacket(TFTP *pTftp) {
	int rc = 0;
	UINT16 OpCode;
	UINT16 ServerBlock;
	UINT8	len;
	tTFTPMode mode;
	tTFTPError error;

	struct tftphdr *ReadBuffer;

	ReadBuffer = (struct tftphdr *) pTftp->PacketBuffer;

	/* Check for a bad packet - abort on error */
	if (!pTftp->Length) {
		rc = TFTPERROR_FAILED;
		goto ABORT;
	}

	OpCode = (UINT16) ntohs(ReadBuffer->opcode);
	switch (OpCode) {
	case RRQ:
		/* Check tftp mode */
		rc = tftpGetMode((char *) ReadBuffer, pTftp->Length, &mode);
		if (rc < 0)
			break;

		/* This server only support octet mode (can add more modes) */
		if (mode != TFTP_MODE_OCTET) {
			rc = TFTPERROR_FAILED;
			break;
		}

		/* Extract the file name */
		pTftp->szFileName = (char *) &ReadBuffer->block;
		len = strlen(pTftp->szFileName) + 1;

		/* Malloc file name */
		if (!(pTftp->szFileName = mmAlloc(len))) {
			rc = TFTPERROR_FAILED;
			break;
		}
		bcopy((char *) &ReadBuffer->block, pTftp->szFileName, len );

		/* Respond first data block */
		pTftp->NextBlock = 1;

		/* Malloc App Data Buffer */
		if (!(pTftp->Buffer = mmAlloc(SEGSIZE))) {
			rc = TFTPERROR_FAILED;
			break;
		}

		/* Send request to the application */
		error = g_pfnRequest(pTftp->szFileName, pTftp->Buffer,
				&pTftp->BufferSize, pTftp->NextBlock);

		if (error != TFTP_ERR_NONE) {
			/* Send error packet */
			tftpErrorBuild(pTftp, error);
			tftpSend(pTftp);
			rc = TFTPERROR_FAILED;
			break;
		}

		/* Change the server listening local port */
		rc = tftpChangeListenPort(pTftp);
		if (rc < 0)
			break;

		if (pTftp->BufferSize > SEGSIZE) {
			rc = TFTPERROR_FAILED;
			break;
		}

		/* Build the packet */
		tftpDataBuild(pTftp);
		rc = tftpSend(pTftp);
		if (rc < 0)
			break;

		/* Increment next expected BLOCK */
		pTftp->NextBlock++;
		goto LEAVE;

	case ACK:
		/* Received Data, verify BLOCK correct */
		ServerBlock = (UINT16) ntohs(ReadBuffer->block);

		/* If this is not the block we're expecting, resync */
		if ((pTftp->NextBlock - 1) != ServerBlock) {
			rc = tftpReSync(pTftp);
			pTftp->Length = 0;
			if (rc < 0)
				break;

			goto LEAVE;
		}

		/* reset Sync Counter */
		pTftp->MaxSyncError = MAX_SYNC_TRIES;

		/* This is the last data block? */
		if (pTftp->BufferSize < SEGSIZE) {
			// All blocks were sent
			rc = 1;
			break;
		}

		/* Send request to the application */
		error = g_pfnRequest(pTftp->szFileName, pTftp->Buffer,
				&pTftp->BufferSize, pTftp->NextBlock);

		if (error != TFTP_ERR_NONE) {
			/* Send error packet */
			tftpErrorBuild(pTftp, error);
			tftpSend(pTftp);
			rc = TFTPERROR_FAILED;
			break;
		}

		if (pTftp->BufferSize > SEGSIZE) {
			rc = TFTPERROR_FAILED;
			break;
		}

		/* Build the packet */
		tftpDataBuild(pTftp);
		rc = tftpSend(pTftp);
		if (rc < 0)
			break;

		/* Increment next expected BLOCK */
		pTftp->NextBlock++;
		goto LEAVE;

	default:
		break;
	}

ABORT:
	if (pTftp->szFileName)
		mmFree(pTftp->szFileName);
	if (pTftp->Buffer)
		mmFree(pTftp->Buffer);
LEAVE:
	return (rc);
}

/**
 @brief Parses the request string to determine the transfer mode, 
 netascii, octet for this request.
 */
static int tftpGetMode(char *pui8Request, UINT32 ui32Len, tTFTPMode *mode) {
	UINT32 ui32Loop;
	UINT32 ui32Max;
	int rc = 0;

	/* Look for the first zero after the start of the filename string (skipping
	 the first two bytes of the request packet). */
	for (ui32Loop = 2; ui32Loop < ui32Len; ui32Loop++) {
		if (pui8Request[ui32Loop] == 0)
			break;
	}

	/* Skip past the zero. */
	ui32Loop++;

	/* Did we run off the end of the string? */
	if (ui32Loop >= ui32Len) {
		/* Yes - this appears to be an invalid request. */
		rc = TFTPERROR_FAILED;
		goto ABORT;
	}

	/* How much data do we have left to look for the mode string? */
	ui32Max = ui32Len - ui32Loop;

	/* All other strings are invalid or obsolete ("mail" for example). */
	*mode = TFTP_MODE_INVALID;

	/* Now determine which of the modes this request asks for.  Is it ASCII? */
	if (strncmp("netascii", (char *) &pui8Request[ui32Loop], ui32Max) == 0) {
		/* This is an ASCII file transfer. */
		*mode = TFTP_MODE_NETASCII;
	} else if (strncmp("octet", (char *) &pui8Request[ui32Loop], ui32Max)
			== 0) { /* Binary transfer? */
		/* This is a binary file transfer. */
		*mode = TFTP_MODE_OCTET;
	}

	ABORT: return (rc);
}


/**
 @brief Build TFTP error packet.
 */
static void tftpErrorBuild(TFTP *pTftp, tTFTPError eError)
{
	struct tftphdr *ERROR_Packet;

	ERROR_Packet = (struct tftphdr *) pTftp->PacketBuffer;

	/* A Error packet consists of an opcode (ERROR) followed */
	/* by a Error code and the message */

	/* Opcode = ERROR */
	ERROR_Packet->opcode = htons(ERROR);

	/* ErrorCode = Error code */
	ERROR_Packet->block = htons(eError);

	/* ErrMsg = None */
	ERROR_Packet->data[0] = 0;

	/* Calculate length of packet */
	pTftp->Length = 5;
}

/**
 @brief Build TFTP data packet.
 */
static void tftpDataBuild(TFTP *pTftp) {
	struct tftphdr *DATA_Packet;
	char *pData;

	/* Check arguments */
	if (pTftp->BufferSize > SEGSIZE) {
		return;
	}

	DATA_Packet = (struct tftphdr *) pTftp->PacketBuffer;

	/* A Data packet consists of an opcode (DATA) followed */
	/* by a Block Number and the Data */

	/* Opcode = DATA */
	DATA_Packet->opcode = htons(DATA);

	/* Block number = Current block */
	DATA_Packet->block = htons(pTftp->NextBlock);

	/* Get a pointer to the rest of the packet */
	pData = (char *) &DATA_Packet->data;

	/* Copy the data to request */
	/* increment data pointer by length of mode (and terminating '0') */

	bcopy(pTftp->Buffer, pData, (int) pTftp->BufferSize);
	pData += pTftp->BufferSize;

	/*  calculate length of packet */
	pTftp->Length = (int) (pData - (char *) DATA_Packet);
}

/**
 @brief Send a TFTP packet.
 */
static int tftpSend(TFTP *pTftp) {
	int rc = 0;
	int BytesSent;

	BytesSent = sendto(pTftp->Sock, pTftp->PacketBuffer, (int) pTftp->Length, 0,
			(struct sockaddr *) &pTftp->peeraddr, sizeof(pTftp->peeraddr));

	if (BytesSent != (int) pTftp->Length)
		rc = TFTPERROR_SOCKET;

	return (rc);
}

/**
 @brief Synchronizes a lost packet
 */
static int tftpReSync(TFTP *pTftp) {
	int rc = 0;

	/* Fluch pending input packets */
	tftpFlushPackets(pTftp);

	/* Abort if too many Sync errors */
	pTftp->MaxSyncError--;
	if (pTftp->MaxSyncError == 0) {
		rc = TFTPERROR_FAILED;
		goto ABORT;
		/* Too Many Sync Errors */
	}

	/* Back up expected block */
	pTftp->NextBlock--;

	/* Resend last packet - if we're on block ZERO, resend the initial */
	/* request. */

	/* Build the packet */
	tftpDataBuild(pTftp);

	/* Send the packet */
	rc = tftpSend(pTftp);
	if (rc < 0)
		goto ABORT;

	pTftp->NextBlock++; /*  Increment next expected BLOCK */

	ABORT: return (rc);
}

/**
 @brief Flush all input from socket
 */
static void tftpFlushPackets(TFTP *pTftp) {
	int bytesread;

	/*  Sleep for a second */
	TaskSleep(1000);

	do {
		bytesread = recv(pTftp->Sock, pTftp->PacketBuffer, DATA_SIZE,
				MSG_DONTWAIT);
	} while (bytesread > 0);
}

/**
 @brief Save a pointer to the callback function to notify TFTP server
 events
 */
void tftpInit(tTFTPRequest pfnRequest) {
	if (pfnRequest == NULL)
		return;
	g_pfnRequest = pfnRequest;
}
