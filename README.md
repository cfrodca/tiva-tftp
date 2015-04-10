# tiva-tftp
TFTP server for TI-RTOS v2

Based on:

- lwIP TFTP server from revision 2.1.0.12573 of the Tiva Utility Library.
- TFTP client from TI Network Developer's Kit (NDK) v2.24

Initialization:

- Call the function void tftpInit(tTFTPRequest pfnRequest) and pass the callback application:

	int (*tTFTPRequest)(char *fileName, char **buffer, UINT32 *bufferSize, UINT16 block);

	Where:

	- char **buffer: is the file data provided by the application
	- UINT32 *bufferSize: is the size of the data
	- UINT16 block: is the segment of 512 Bytes of the file to transmit

- The server daemon must be created and destroyed from a file netHooks.c:

	static HANDLE hTftp = 0;

	void netOpenHook()
	{
    		// Create our local servers
		hTftp = DaemonNew( SOCK_DGRAM, 0, PORT_TFTP, dtask_tftp, OS_TASKPRINORM, OS_TASKSTKNORM, 0, 1 );
	}

	void netCloseHook()
	{
    		DaemonFree(hTftp);
	}