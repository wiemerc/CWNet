/*
 * serecho.c - simple test program for the serial interface that just echos 
 *             everything it receives
 *
 * Copyright(C) 2017, Constantin Wiemer
 */


#include <devices/serial.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <exec/types.h>
#include <proto/alib.h>
#include <proto/exec.h>
#include <stdio.h>
#include <string.h>


#define BUF_SIZE 256


int main()
{
    struct MsgPort *port;
    struct IOExtSer *req;
    struct IOTArray termchars = {0x00000000, 0x00000000};
    char rxbuf[BUF_SIZE], txbuf[BUF_SIZE];

    if ((port = CreateMsgPort())) {
        if ((req = (struct IOExtSer *) CreateExtIO(port, sizeof(struct IOExtSer)))) {
            if (OpenDevice("serial.device", 0l, (struct IORequest *) req, 0l) == 0) {
                /* configure device to terminate read requests on NUL bytes */
                req->IOSer.io_Command = SDCMD_SETPARAMS;
                req->io_SerFlags     |= SERF_EOFMODE;
                req->io_TermArray     = termchars;
                if (DoIO((struct IORequest *) req) == 0) {
                    while(1) {
                        /* read message */
                        req->IOSer.io_Command = CMD_READ;
                        req->IOSer.io_Length  = BUF_SIZE;
                        req->IOSer.io_Data    = (APTR) rxbuf;
                        if (DoIO((struct IORequest *) req) != 0) {
                            printf("reading from serial device failed: error = %d\n", req->IOSer.io_Error);
                            break;
                        }
                        printf("message received: %s\n", rxbuf);
                        if (rxbuf[0] == '.') {
                            /* client wants us to terminate */
                            printf("terminating...\n");
                            break;
                        }
                        else {
                            /* send answer */
                            printf("sending answer...\n");
                            strncpy(txbuf, "ECHO: ", BUF_SIZE);
                            strncat (txbuf, rxbuf, BUF_SIZE - 7);
                            req->IOSer.io_Command = CMD_WRITE;
                            req->IOSer.io_Length  = -1;
                            req->IOSer.io_Data    = (APTR) txbuf;

                            if (DoIO((struct IORequest *) req) != 0) {
                                printf("writing to serial device failed: error = %d\n", req->IOSer.io_Error);
                                break;
                            }
                        }
                    }
                }
                else {
                    printf("configuring serial device failed: error = %d\n", req->IOSer.io_Error);
                }
                CloseDevice((struct IORequest *) req);
            }
            else {
                printf("could not open serial device\n");
            }
            DeleteExtIO((struct IORequest *) req);
        }
        else {
            printf("could not create IOExtSer request\n");
        }
        DeleteMsgPort(port);
    }
    else {
        printf("could not open message port\n");
    }

    return 0;
}

